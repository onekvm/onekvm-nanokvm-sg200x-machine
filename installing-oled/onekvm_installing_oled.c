// SPDX-License-Identifier: GPL-2.0-only
/*
 * NanoKVM early-mode OLED indicator.
 *
 * This bit-bangs three lines through gpiolib so the DesignWare GPIO driver's
 * output shadow remains coherent.  The initramfs does not have to load the
 * normal GPIO-I2C stack before storage migration. A responsive C906L owns
 * the display; otherwise probe the Cube OLED (0x3d) before the PCIe OLED
 * (0x3c) and select the matching canvas and display geometry.
 * Install and setup error screens are drawn from the step written to the
 * `state` parameter; Recovery keeps its official canvas.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define ONEKVM_PINMUX_BASE          0x03001000
#define ONEKVM_PINMUX_SIZE          0x100
#define ONEKVM_PINMUX_SCL           0x3c
#define ONEKVM_PINMUX_ENABLE        0x50
#define ONEKVM_PINMUX_SDA           0x58
#define ONEKVM_PINMUX_GPIO          3

#define ONEKVM_GPIO_CHIP_LABEL      "3020000.gpio"
#define ONEKVM_GPIO_SCL_OFFSET      15
#define ONEKVM_GPIO_ENABLE_OFFSET   22
#define ONEKVM_GPIO_SDA_OFFSET      27

#define ONEKVM_PCIE_OLED_ADDRESS        0x3c
#define ONEKVM_PCIE_OLED_WIDTH          64
#define ONEKVM_PCIE_OLED_PAGES          4
#define ONEKVM_PCIE_OLED_COLUMN_OFFSET  32
#define ONEKVM_PCIE_OLED_PAGE_OFFSET    8
#define ONEKVM_CUBE_OLED_ADDRESS        0x3d
#define ONEKVM_CUBE_OLED_WIDTH          128
#define ONEKVM_CUBE_OLED_PAGES          8
#define ONEKVM_CUBE_OLED_COLUMN_OFFSET  0
#define ONEKVM_CUBE_OLED_PAGE_OFFSET    0
#define ONEKVM_OLED_MAX_WIDTH           ONEKVM_CUBE_OLED_WIDTH
#define ONEKVM_OLED_MAX_FRAME_SIZE      \
	(ONEKVM_CUBE_OLED_WIDTH * ONEKVM_CUBE_OLED_PAGES)
#define ONEKVM_I2C_DELAY_US         5
#define ONEKVM_I2C_HIGH_TIMEOUT_US  100
#define ONEKVM_BLINK_PERIOD_MS      3000
#define ONEKVM_BLINK_OFF_MS         1000

static char *display_mode = "install";
module_param_named(mode, display_mode, charp, 0444);
MODULE_PARM_DESC(mode, "Screen to display: install, error or recovery");
static unsigned int initial_step;
module_param_named(step, initial_step, uint, 0444);
MODULE_PARM_DESC(step, "Install step shown at load time (0-7)");

struct onekvm_oled_profile {
	const char *name;
	u8 address;
	u8 width;
	u8 pages;
	u8 column_offset;
	u8 page_offset;
	u8 segment_remap;
	u8 com_scan;
};

static const struct onekvm_oled_profile pcie_oled_profile = {
	.name = "PCIe",
	.address = ONEKVM_PCIE_OLED_ADDRESS,
	.width = ONEKVM_PCIE_OLED_WIDTH,
	.pages = ONEKVM_PCIE_OLED_PAGES,
	.column_offset = ONEKVM_PCIE_OLED_COLUMN_OFFSET,
	.page_offset = ONEKVM_PCIE_OLED_PAGE_OFFSET,
	.segment_remap = 0xa0,
	.com_scan = 0xc0,
};

static const struct onekvm_oled_profile cube_oled_profile = {
	.name = "Cube",
	.address = ONEKVM_CUBE_OLED_ADDRESS,
	.width = ONEKVM_CUBE_OLED_WIDTH,
	.pages = ONEKVM_CUBE_OLED_PAGES,
	.column_offset = ONEKVM_CUBE_OLED_COLUMN_OFFSET,
	.page_offset = ONEKVM_CUBE_OLED_PAGE_OFFSET,
	.segment_remap = 0xa1,
	.com_scan = 0xc8,
};

static const struct onekvm_oled_profile *active_profile;

static void __iomem *onekvm_pinmux;
static u32 saved_pinmux_scl;
static u32 saved_pinmux_enable;
static u32 saved_pinmux_sda;

struct onekvm_gpio_line {
	struct gpio_desc *desc;
	unsigned int gpio;
	int saved_direction;
	int saved_value;
	bool requested;
};

static struct onekvm_gpio_line gpio_scl;
static struct onekvm_gpio_line gpio_enable;
static struct onekvm_gpio_line gpio_sda;
static bool display_ready;
static bool blink_is_off;
static const char *last_i2c_stage = "idle";
static size_t last_i2c_index;
static DEFINE_MUTEX(display_lock);
static void onekvm_blink_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(blink_work, onekvm_blink_work);

/*
 * Rendered by onekvm-extension-nanokvm-oled/src/main.cpp from
 * assets/canvases/system-recovery-pcie.json at rotation 180 degrees.
 * Source canvas SHA-256:
 * 9a88db978c2d41e086f0787a72b8ff2b387acedc67b57c7e5b4f9914418ec12c
 */
static const u8 pcie_recovery_frame[ONEKVM_PCIE_OLED_WIDTH *
				    ONEKVM_PCIE_OLED_PAGES] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x54, 0x54,
	0x54, 0x7c, 0x00, 0x38, 0x44, 0x44, 0x44, 0x7c,
	0x00, 0x38, 0x44, 0x44, 0x44, 0x38, 0x00, 0x7c,
	0x20, 0x10, 0x20, 0x7c, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x40, 0x20, 0x1c, 0x20, 0x40, 0x00, 0x24,
	0x58, 0x50, 0x50, 0x7c, 0x00, 0x44, 0x54, 0x54,
	0x54, 0x7c, 0x00, 0x70, 0x08, 0x04, 0x08, 0x70,
	0x00, 0x38, 0x44, 0x44, 0x44, 0x38, 0x00, 0x44,
	0x44, 0x44, 0x44, 0x38, 0x00, 0x44, 0x54, 0x54,
	0x54, 0x7c, 0x00, 0x24, 0x58, 0x50, 0x50, 0x7c,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * Rendered by onekvm-extension-nanokvm-oled/src/main.cpp from the Cube's
 * active two-element canvas, saved as
 * assets/canvases/system-recovery-cube.json at rotation 0 degrees.  Each byte
 * pair is run length followed by value and expands to 1024 controller bytes.
 * Source canvas SHA-256:
 * 9b1b0f4f9c56ab3e9ecf4d85c3de896a7b1d710827a6284315cde8756e665042
 */
static const u8 cube_recovery_frame_rle[] = {
	0xff, 0x00, 0x12, 0x00, 0x02, 0xfe, 0x04, 0x66,
	0x02, 0xe6, 0x02, 0x18, 0x02, 0x00, 0x02, 0xfe,
	0x06, 0x66, 0x02, 0x06, 0x02, 0x00, 0x02, 0xf8,
	0x08, 0x06, 0x02, 0x00, 0x02, 0xf8, 0x06, 0x06,
	0x02, 0xf8, 0x02, 0x00, 0x02, 0x7e, 0x02, 0x80,
	0x02, 0x00, 0x02, 0x80, 0x02, 0x7e, 0x02, 0x00,
	0x02, 0xfe, 0x06, 0x66, 0x02, 0x06, 0x02, 0x00,
	0x02, 0xfe, 0x04, 0x66, 0x02, 0xe6, 0x02, 0x18,
	0x02, 0x00, 0x02, 0x06, 0x02, 0x18, 0x02, 0xe0,
	0x02, 0x18, 0x02, 0x06, 0x22, 0x00, 0x02, 0x07,
	0x04, 0x00, 0x02, 0x01, 0x02, 0x06, 0x02, 0x00,
	0x02, 0x07, 0x08, 0x06, 0x02, 0x00, 0x02, 0x01,
	0x08, 0x06, 0x02, 0x00, 0x02, 0x01, 0x06, 0x06,
	0x02, 0x01, 0x04, 0x00, 0x02, 0x01, 0x02, 0x06,
	0x02, 0x01, 0x04, 0x00, 0x02, 0x07, 0x08, 0x06,
	0x02, 0x00, 0x02, 0x07, 0x04, 0x00, 0x02, 0x01,
	0x02, 0x06, 0x06, 0x00, 0x02, 0x07, 0x3e, 0x00,
	0x02, 0xe0, 0x02, 0x80, 0x02, 0x00, 0x02, 0x80,
	0x02, 0xe0, 0x02, 0x00, 0x02, 0x80, 0x06, 0x60,
	0x02, 0x80, 0x02, 0x00, 0x02, 0xe0, 0x06, 0x60,
	0x02, 0x80, 0x02, 0x00, 0x02, 0xe0, 0x08, 0x60,
	0x52, 0x00, 0x02, 0x7f, 0x02, 0x01, 0x02, 0x06,
	0x02, 0x01, 0x02, 0x7f, 0x02, 0x00, 0x02, 0x1f,
	0x06, 0x60, 0x02, 0x1f, 0x02, 0x00, 0x02, 0x7f,
	0x06, 0x60, 0x02, 0x1f, 0x02, 0x00, 0x02, 0x7f,
	0x06, 0x66, 0x02, 0x60, 0xff, 0x00, 0x2a, 0x00,
};

static int match_gpiochip_label(struct gpio_chip *chip, void *data)
{
	const char *label = data;

	return chip->label && !strcmp(chip->label, label);
}

static int request_gpio_line(struct gpio_chip *chip, unsigned int offset,
			     const char *label, struct onekvm_gpio_line *line)
{
	int ret;

	if (chip->base < 0 || offset >= chip->ngpio)
		return -EINVAL;
	line->gpio = chip->base + offset;
	ret = gpio_request(line->gpio, label);
	if (ret)
		return ret;
	line->desc = gpio_to_desc(line->gpio);
	if (!line->desc) {
		gpio_free(line->gpio);
		return -ENODEV;
	}
	line->requested = true;
	line->saved_direction = gpiod_get_direction(line->desc);
	if (line->saved_direction < 0) {
		ret = line->saved_direction;
		gpio_free(line->gpio);
		line->requested = false;
		line->desc = NULL;
		return ret;
	}
	line->saved_value = gpiod_get_raw_value(line->desc);
	if (line->saved_value < 0) {
		ret = line->saved_value;
		gpio_free(line->gpio);
		line->requested = false;
		line->desc = NULL;
		return ret;
	}
	return 0;
}

static void restore_gpio_line(struct onekvm_gpio_line *line,
			      const char *label)
{
	int ret;

	if (!line->requested)
		return;
	if (line->saved_direction == 0)
		ret = gpiod_direction_output_raw(line->desc,
						 line->saved_value);
	else
		ret = gpiod_direction_input(line->desc);
	if (ret)
		pr_warn("onekvm-installing-oled: failed to restore %s: %d\n",
			label, ret);
	gpio_free(line->gpio);
	memset(line, 0, sizeof(*line));
}

/* Open-drain GPIO: output-low asserts a line, input releases it. */
static int i2c_assert_low(struct gpio_desc *line)
{
	int ret = gpiod_direction_output_raw(line, 0);

	if (ret)
		return ret;
	udelay(ONEKVM_I2C_DELAY_US);
	return 0;
}

static int i2c_release(struct gpio_desc *line)
{
	int ret = gpiod_direction_input(line);

	if (ret)
		return ret;
	udelay(ONEKVM_I2C_DELAY_US);
	return 0;
}

static bool i2c_line_high(struct gpio_desc *line)
{
	return gpiod_get_raw_value(line) > 0;
}

static int i2c_wait_high(struct gpio_desc *line)
{
	unsigned int waited;

	for (waited = 0; waited < ONEKVM_I2C_HIGH_TIMEOUT_US; ++waited) {
		if (i2c_line_high(line))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int i2c_start(void)
{
	int ret;

	ret = i2c_release(gpio_sda.desc);
	if (ret)
		return ret;
	ret = i2c_release(gpio_scl.desc);
	if (ret)
		return ret;
	if (i2c_wait_high(gpio_scl.desc) || !i2c_line_high(gpio_sda.desc))
		return -EBUSY;
	ret = i2c_assert_low(gpio_sda.desc);
	return ret ? ret : i2c_assert_low(gpio_scl.desc);
}

static int i2c_stop(void)
{
	int ret;

	ret = i2c_assert_low(gpio_sda.desc);
	if (ret)
		return ret;
	ret = i2c_release(gpio_scl.desc);
	if (ret)
		return ret;
	ret = i2c_wait_high(gpio_scl.desc);
	if (ret)
		return ret;
	return i2c_release(gpio_sda.desc);
}

static int i2c_write_bit(bool high)
{
	int ret;

	if (high)
		ret = i2c_release(gpio_sda.desc);
	else
		ret = i2c_assert_low(gpio_sda.desc);
	if (ret)
		return ret;
	ret = i2c_release(gpio_scl.desc);
	if (ret)
		return ret;
	if (i2c_wait_high(gpio_scl.desc))
		return -ETIMEDOUT;
	return i2c_assert_low(gpio_scl.desc);
}

static int i2c_write_byte(u8 value)
{
	int bit;
	int ret;
	bool acknowledged;

	for (bit = 7; bit >= 0; --bit) {
		ret = i2c_write_bit(value & BIT(bit));
		if (ret)
			return ret;
	}

	ret = i2c_release(gpio_sda.desc);
	if (ret)
		return ret;
	ret = i2c_release(gpio_scl.desc);
	if (ret)
		return ret;
	ret = i2c_wait_high(gpio_scl.desc);
	acknowledged = !i2c_line_high(gpio_sda.desc);
	if (!ret)
		ret = i2c_assert_low(gpio_scl.desc);
	return ret ? ret : (acknowledged ? 0 : -ENXIO);
}

static int oled_write_address(u8 address, u8 control, const u8 *payload,
			      size_t length)
{
	size_t index;
	int ret;

	last_i2c_stage = "start";
	last_i2c_index = 0;
	ret = i2c_start();
	if (ret)
		return ret;
	last_i2c_stage = "address";
	ret = i2c_write_byte(address << 1);
	if (!ret) {
		last_i2c_stage = "control";
		ret = i2c_write_byte(control);
	}
	for (index = 0; !ret && index < length; ++index) {
		last_i2c_stage = "payload";
		last_i2c_index = index;
		ret = i2c_write_byte(payload[index]);
	}
	if (!ret)
		ret = i2c_stop();
	else
		i2c_stop();
	if (!ret)
		last_i2c_stage = "idle";
	return ret;
}

static int oled_write(u8 control, const u8 *payload, size_t length)
{
	if (!active_profile)
		return -ENODEV;
	return oled_write_address(active_profile->address, control, payload,
				  length);
}

static int oled_command(u8 command)
{
	return oled_write(0x00, &command, 1);
}

static int oled_show(const u8 *frame, size_t frame_size, bool frame_is_rle)
{
	u8 page_frame[ONEKVM_OLED_MAX_WIDTH];
	size_t rle_index = 0;
	unsigned int run_remaining = 0;
	u8 run_value = 0;
	int page;
	int ret;

	if (!active_profile)
		return -ENODEV;
	if (!frame_is_rle && frame_size != active_profile->width *
						 active_profile->pages)
		return -EINVAL;

	for (page = 0; page < active_profile->pages; ++page) {
		const u8 position[] = {
			0xb0 + active_profile->page_offset + page,
			0x10 | (active_profile->column_offset >> 4),
			active_profile->column_offset & 0x0f,
		};
		const u8 *page_data;
		int column;

		if (frame_is_rle) {
			for (column = 0; column < active_profile->width;) {
				unsigned int count;

				if (!run_remaining) {
					if (rle_index + 2 > frame_size)
						return -EINVAL;
					run_remaining = frame[rle_index++];
					run_value = frame[rle_index++];
					if (!run_remaining)
						return -EINVAL;
				}
				count = active_profile->width - column;
				if (count > run_remaining)
					count = run_remaining;
				memset(page_frame + column, run_value, count);
				column += count;
				run_remaining -= count;
			}
			page_data = page_frame;
		} else {
			page_data = frame + page * active_profile->width;
		}

		ret = oled_write(0x00, position, sizeof(position));
		if (ret)
			return ret;
		ret = oled_write(0x40, page_data, active_profile->width);
		if (ret)
			return ret;
	}
	if (frame_is_rle && (run_remaining || rle_index != frame_size))
		return -EINVAL;
	return 0;
}

static int oled_initialize(void)
{
	static const u8 sequence[] = {
		0xae, 0x00, 0x10, 0x40, 0x81, 0xcf, 0xa1, 0xc8,
		0xa6, 0xa8, 0x3f, 0xd3, 0x00, 0xd5, 0x80, 0xd9,
		0xf1, 0xda, 0x12, 0xdb, 0x40, 0x20, 0x02, 0x8d,
		0x14, 0xa4, 0xa6, 0xaf,
	};
	const u8 direction[] = {
		active_profile->segment_remap,
		active_profile->com_scan,
	};
	int ret;

	ret = oled_write(0x00, sequence, sizeof(sequence));
	if (ret)
		return ret;
	return oled_write(0x00, direction, sizeof(direction));
}

/*
 * Install and error screens are drawn here with the 5-pixel font of
 * onekvm-extension-nanokvm-oled (canvas.hpp, font width 4): letters are
 * four pixels wide, digits and symbols three, one pixel apart.
 */
static const u8 latin_font4[26][5] = {
	{ 6, 9, 15, 9, 9 },   { 14, 9, 14, 9, 14 }, { 7, 8, 8, 8, 7 },
	{ 14, 9, 9, 9, 14 },  { 15, 8, 14, 8, 15 }, { 15, 8, 14, 8, 8 },
	{ 7, 8, 11, 9, 7 },   { 9, 9, 15, 9, 9 },   { 15, 2, 2, 2, 15 },
	{ 1, 1, 1, 9, 6 },    { 9, 10, 12, 10, 9 }, { 8, 8, 8, 8, 15 },
	{ 9, 15, 15, 9, 9 },  { 9, 13, 11, 9, 9 },  { 6, 9, 9, 9, 6 },
	{ 14, 9, 14, 8, 8 },  { 6, 9, 9, 11, 7 },   { 14, 9, 14, 10, 9 },
	{ 7, 8, 6, 1, 14 },   { 15, 2, 2, 2, 2 },   { 9, 9, 9, 9, 6 },
	{ 9, 9, 9, 6, 6 },    { 9, 9, 15, 15, 9 },  { 9, 9, 6, 9, 9 },
	{ 9, 9, 6, 2, 2 },    { 15, 1, 2, 4, 15 },
};

static const u8 digit_font3[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 6, 1, 7, 4, 7 },
	{ 6, 1, 3, 1, 6 }, { 5, 5, 7, 1, 1 }, { 7, 4, 6, 1, 6 },
	{ 3, 4, 7, 5, 7 }, { 7, 1, 2, 2, 2 }, { 7, 5, 7, 5, 7 },
	{ 7, 5, 7, 1, 6 },
};

static const u8 blank_glyph[5];
static const u8 slash_glyph[5] = { 1, 1, 2, 4, 4 };
static const u8 unknown_glyph[5] = { 6, 1, 2, 0, 2 };

#define ONEKVM_INSTALL_STEPS 7

/* Step N names the work started by `nanokvm_splash install N`. */
static const char * const install_step_names[ONEKVM_INSTALL_STEPS + 1] = {
	"", "READ SYSTEM", "READ BOOT", "PARTITIONS", "WRITE BOOT",
	"WRITE SYSTEM", "USER DATA", "FINISHING",
};

static const u8 *glyph_for(char character, int *width)
{
	if (character >= 'A' && character <= 'Z') {
		*width = 4;
		return latin_font4[character - 'A'];
	}
	*width = 3;
	if (character >= '0' && character <= '9')
		return digit_font3[character - '0'];
	if (character == ' ')
		return blank_glyph;
	if (character == '/')
		return slash_glyph;
	return unknown_glyph;
}

static int text_width(const char *text, int scale)
{
	int width = 0;
	int glyph_width;

	for (; *text; ++text) {
		glyph_for(*text, &glyph_width);
		width += (glyph_width + 1) * scale;
	}
	return width ? width - scale : 0;
}

static void draw_text(u8 *frame, int width, int height, int y,
		      const char *text, int scale)
{
	int x = (width - text_width(text, scale)) / 2;

	for (; *text; ++text) {
		int glyph_width;
		const u8 *rows = glyph_for(*text, &glyph_width);
		int row;
		int column;

		for (row = 0; row < 5; ++row)
			for (column = 0; column < glyph_width; ++column) {
				int oy;
				int ox;

				if (!(rows[row] & BIT(glyph_width - 1 - column)))
					continue;
				for (oy = 0; oy < scale; ++oy)
					for (ox = 0; ox < scale; ++ox) {
						int px = x + column * scale + ox;
						int py = y + row * scale + oy;

						if (px < 0 || px >= width ||
						    py < 0 || py >= height)
							continue;
						frame[(py / 8) * width + px] |=
							BIT(py % 8);
					}
			}
		x += (glyph_width + 1) * scale;
	}
}

static bool state_error;
static unsigned int state_step;
static bool display_running;
static bool recovery_blink;
static bool rtos_live;
static void __iomem *rtos_shm;
static int screen_width;
static int screen_height;
static u8 screen_frame[ONEKVM_OLED_MAX_FRAME_SIZE];

/* Same rows as the official install canvases: PCIe y=6/14/22, Cube 2x. */
static size_t render_state(void)
{
	static const int pcie_rows[] = { 6, 14, 22 };
	static const int cube_rows[] = { 9, 27, 44 };
	const int *rows = screen_width == ONEKVM_CUBE_OLED_WIDTH ?
				  cube_rows : pcie_rows;
	int scale = screen_width == ONEKVM_CUBE_OLED_WIDTH ? 2 : 1;
	size_t size = (size_t)screen_width * screen_height / 8;
	char progress[16];
	const char *lines[3];

	snprintf(progress, sizeof(progress), "STEP %u/%u", state_step,
		 ONEKVM_INSTALL_STEPS);
	if (state_error) {
		lines[0] = "SETUP FAILED";
		lines[1] = state_step ? progress : "CHECK SD CARD";
		lines[2] = state_step ? "CHECK SD CARD" : "";
	} else {
		lines[0] = "INSTALLING";
		lines[1] = state_step ? progress : "PLEASE WAIT";
		lines[2] = install_step_names[state_step];
	}
	memset(screen_frame, 0, sizeof(screen_frame));
	draw_text(screen_frame, screen_width, screen_height, rows[0],
		  lines[0], scale);
	draw_text(screen_frame, screen_width, screen_height, rows[1],
		  lines[1], scale);
	draw_text(screen_frame, screen_width, screen_height, rows[2],
		  lines[2], scale);
	return size;
}

#define ONEKVM_OLED_SHM_PHYS     0x8FFF0000UL
#define ONEKVM_OLED_SHM_SIZE     0x1000
#define ONEKVM_OLED_SHM_MAGIC    0x44454C4FU
#define ONEKVM_OLED_SHM_SEQ      4
#define ONEKVM_OLED_SHM_ACK      8
#define ONEKVM_OLED_SHM_CMD      12
#define ONEKVM_OLED_SHM_VARIANT  16
#define ONEKVM_OLED_SHM_WIDTH    20
#define ONEKVM_OLED_SHM_HEIGHT   24
#define ONEKVM_OLED_SHM_FRAME    32
#define ONEKVM_OLED_SHM_FRAME_MAX 1024
#define ONEKVM_OLED_SHM_LOCK     (ONEKVM_OLED_SHM_FRAME + ONEKVM_OLED_SHM_FRAME_MAX)
#define ONEKVM_OLED_LOCK_LINUX   1
#define ONEKVM_OLED_CMD_FRAME    1
#define ONEKVM_OLED_CMD_BOOT     2
#define ONEKVM_OLED_CMD_RECOVERY 4
#define ONEKVM_OLED_CMD_OFF      5

/* Post one command to the C906L display service and wait for its ACK. */
static int rtos_post(u32 cmd, const u8 *frame, size_t size)
{
	unsigned int spins = 100000;
	unsigned long deadline;
	u32 ack;
	u32 seq;

	if (!rtos_shm || size > ONEKVM_OLED_SHM_FRAME_MAX)
		return -EINVAL;
	while (spins-- && readl(rtos_shm + ONEKVM_OLED_SHM_LOCK))
		cpu_relax();
	writel(ONEKVM_OLED_LOCK_LINUX, rtos_shm + ONEKVM_OLED_SHM_LOCK);
	if (frame) {
		memcpy_toio(rtos_shm + ONEKVM_OLED_SHM_FRAME, frame, size);
		memset_io(rtos_shm + ONEKVM_OLED_SHM_FRAME + size, 0,
			  ONEKVM_OLED_SHM_FRAME_MAX - size);
	}
	writel(cmd, rtos_shm + ONEKVM_OLED_SHM_CMD);
	ack = readl(rtos_shm + ONEKVM_OLED_SHM_ACK);
	seq = readl(rtos_shm + ONEKVM_OLED_SHM_SEQ) + 1;
	if (!seq)
		seq = 1;
	if (seq == ack && !++seq)
		seq = 1;
	wmb();
	writel(seq, rtos_shm + ONEKVM_OLED_SHM_SEQ);
	writel(0, rtos_shm + ONEKVM_OLED_SHM_LOCK);
	/* Warm resets can retain the magic from a previous OneKVM FIP even
	 * when the factory FIP starts no C906L image. Require a fresh ACK.
	 */
	deadline = jiffies + msecs_to_jiffies(1000);
	do {
		if (readl(rtos_shm + ONEKVM_OLED_SHM_ACK) == seq)
			return 0;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

/* Draw the current state. The caller holds display_lock. */
static int show_state_locked(void)
{
	size_t size = render_state();
	int ret;

	if (rtos_live) {
		ret = rtos_post(ONEKVM_OLED_CMD_FRAME, screen_frame, size);
	} else {
		if (blink_is_off) {
			ret = oled_command(0xaf);
			if (ret)
				return ret;
		}
		ret = oled_show(screen_frame, size, false);
	}
	if (!ret)
		blink_is_off = false;
	return ret;
}

static void onekvm_blink_work(struct work_struct *work)
{
	unsigned long delay;
	int ret;

	(void)work;
	mutex_lock(&display_lock);
	if ((!display_running || state_error) && !recovery_blink) {
		mutex_unlock(&display_lock);
		return;
	}

	if (blink_is_off) {
		if (recovery_blink) {
			ret = oled_command(0xaf);
			if (!ret)
				blink_is_off = false;
		} else {
			ret = show_state_locked();
		}
		delay = msecs_to_jiffies(ONEKVM_BLINK_PERIOD_MS -
					 ONEKVM_BLINK_OFF_MS);
	} else {
		ret = rtos_live ? rtos_post(ONEKVM_OLED_CMD_OFF, NULL, 0) :
				  oled_command(0xae);
		if (!ret)
			blink_is_off = true;
		delay = msecs_to_jiffies(ONEKVM_BLINK_OFF_MS);
	}
	if (ret && rtos_live)
		pr_warn_ratelimited("onekvm-installing-oled: C906L blink failed: %d\n",
				    ret);
	else if (ret)
		pr_warn_ratelimited("onekvm-installing-oled: blink failed: %d stage=%s index=%zu pinmux=%08x/%08x/%08x lines=%d/%d/%d enable-dir=%d\n",
				    ret, last_i2c_stage, last_i2c_index,
				    readl(onekvm_pinmux + ONEKVM_PINMUX_SCL),
				    readl(onekvm_pinmux + ONEKVM_PINMUX_ENABLE),
				    readl(onekvm_pinmux + ONEKVM_PINMUX_SDA),
				    gpiod_get_raw_value(gpio_scl.desc),
				    gpiod_get_raw_value(gpio_enable.desc),
				    gpiod_get_raw_value(gpio_sda.desc),
				    gpiod_get_direction(gpio_enable.desc));
	mutex_unlock(&display_lock);
	schedule_delayed_work(&blink_work, delay);
}

static int parse_state(const char *value, bool *error, unsigned int *step)
{
	char kind[8];
	unsigned int number = 0;
	int fields = sscanf(value, "%7s %u", kind, &number);

	if (fields < 1 || number > ONEKVM_INSTALL_STEPS)
		return -EINVAL;
	if (!strcmp(kind, "install"))
		*error = false;
	else if (!strcmp(kind, "error"))
		*error = true;
	else
		return -EINVAL;
	*step = number;
	return 0;
}

/*
 * /sys/module/onekvm_installing_oled/parameters/state takes "install N"
 * while step N runs and "error N" when setup stops; N is 0 before step 1.
 * Install screens blink every three seconds. Error screens stay on.
 */
static int state_set(const char *value, const struct kernel_param *kp)
{
	bool error;
	unsigned int step;
	int ret;

	(void)kp;
	ret = parse_state(value, &error, &step);
	if (ret)
		return ret;
	if (error)
		cancel_delayed_work_sync(&blink_work);
	mutex_lock(&display_lock);
	state_error = error;
	state_step = step;
	ret = display_running ? show_state_locked() : 0;
	mutex_unlock(&display_lock);
	if (!error && display_running)
		mod_delayed_work(system_wq, &blink_work,
				 msecs_to_jiffies(ONEKVM_BLINK_PERIOD_MS));
	return ret;
}

static int state_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	return sysfs_emit(buffer, "%s %u\n", state_error ? "error" : "install",
			  state_step);
}

static const struct kernel_param_ops state_ops = {
	.set = state_set,
	.get = state_get,
};
module_param_cb(state, &state_ops, NULL, 0644);
MODULE_PARM_DESC(state, "Install screen: 'install N' or 'error N' (N = 0-7)");

static void restore_hardware_state(void)
{
	restore_gpio_line(&gpio_sda, "SDA");
	restore_gpio_line(&gpio_scl, "SCL");
	restore_gpio_line(&gpio_enable, "enable");
	if (onekvm_pinmux) {
		writel(saved_pinmux_scl, onekvm_pinmux + ONEKVM_PINMUX_SCL);
		writel(saved_pinmux_enable,
		       onekvm_pinmux + ONEKVM_PINMUX_ENABLE);
		writel(saved_pinmux_sda, onekvm_pinmux + ONEKVM_PINMUX_SDA);
	}
}

static int prepare_hardware(void)
{
	struct gpio_chip *chip;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	struct gpio_device *gdev;
#endif
	u32 value;
	int ret;

	onekvm_pinmux = ioremap(ONEKVM_PINMUX_BASE, ONEKVM_PINMUX_SIZE);
	if (!onekvm_pinmux)
		return -ENOMEM;
	saved_pinmux_scl = readl(onekvm_pinmux + ONEKVM_PINMUX_SCL);
	saved_pinmux_enable = readl(onekvm_pinmux + ONEKVM_PINMUX_ENABLE);
	saved_pinmux_sda = readl(onekvm_pinmux + ONEKVM_PINMUX_SDA);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	gdev = gpio_device_find_by_label(ONEKVM_GPIO_CHIP_LABEL);
	chip = gdev ? gpio_device_get_chip(gdev) : NULL;
#else
	chip = gpiochip_find(ONEKVM_GPIO_CHIP_LABEL, match_gpiochip_label);
#endif
	if (!chip) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
		if (gdev)
			gpio_device_put(gdev);
#endif
		iounmap(onekvm_pinmux);
		onekvm_pinmux = NULL;
		return -EPROBE_DEFER;
	}
	ret = request_gpio_line(chip, ONEKVM_GPIO_ENABLE_OFFSET,
				"onekvm-oled-enable", &gpio_enable);
	if (ret)
		goto fail;
	ret = request_gpio_line(chip, ONEKVM_GPIO_SCL_OFFSET,
				"onekvm-oled-scl", &gpio_scl);
	if (ret)
		goto fail;
	ret = request_gpio_line(chip, ONEKVM_GPIO_SDA_OFFSET,
				"onekvm-oled-sda", &gpio_sda);
	if (ret)
		goto fail;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	gpio_device_put(gdev);
	gdev = NULL;
#endif
	value = (saved_pinmux_scl & ~0x7U) | ONEKVM_PINMUX_GPIO;
	writel(value, onekvm_pinmux + ONEKVM_PINMUX_SCL);
	value = (saved_pinmux_enable & ~0x7U) | ONEKVM_PINMUX_GPIO;
	writel(value, onekvm_pinmux + ONEKVM_PINMUX_ENABLE);
	value = (saved_pinmux_sda & ~0x7U) | ONEKVM_PINMUX_GPIO;
	writel(value, onekvm_pinmux + ONEKVM_PINMUX_SDA);

	/* Defined reset/power cycle, matching the verified U-Boot sequence. */
	ret = gpiod_direction_output_raw(gpio_enable.desc, 0);
	if (ret)
		goto fail;
	mdelay(1);
	ret = gpiod_direction_output_raw(gpio_enable.desc, 1);
	if (ret)
		goto fail;
	mdelay(5);
	ret = i2c_release(gpio_sda.desc);
	if (ret)
		goto fail;
	ret = i2c_release(gpio_scl.desc);
	if (ret)
		goto fail;
	return 0;

fail:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	if (gdev)
		gpio_device_put(gdev);
#endif
	restore_hardware_state();
	iounmap(onekvm_pinmux);
	onekvm_pinmux = NULL;
	return ret;
}

/* Use a responsive C906L display service; return -ENODEV to probe I2C. */
static int rtos_open(void)
{
	int width;
	int height;

	rtos_shm = ioremap(ONEKVM_OLED_SHM_PHYS, ONEKVM_OLED_SHM_SIZE);
	if (!rtos_shm)
		return -ENOMEM;
	width = readl(rtos_shm + ONEKVM_OLED_SHM_WIDTH);
	height = readl(rtos_shm + ONEKVM_OLED_SHM_HEIGHT);
	if (readl(rtos_shm) != ONEKVM_OLED_SHM_MAGIC ||
	    !readl(rtos_shm + ONEKVM_OLED_SHM_VARIANT) ||
	    !((width == ONEKVM_PCIE_OLED_WIDTH && height == 32) ||
	      (width == ONEKVM_CUBE_OLED_WIDTH && height == 64))) {
		iounmap(rtos_shm);
		rtos_shm = NULL;
		return -ENODEV;
	}
	screen_width = width;
	screen_height = height;
	return 0;
}

static void rtos_close(void)
{
	if (rtos_shm)
		iounmap(rtos_shm);
	rtos_shm = NULL;
	rtos_live = false;
}

static int __init onekvm_installing_oled_init(void)
{
	static const struct onekvm_oled_profile * const profiles[] = {
		&cube_oled_profile,
		&pcie_oled_profile,
	};
	bool recovery_mode;
	size_t profile_index;
	int attempt;
	int ret;

	if (!strcmp(display_mode, "recovery")) {
		recovery_mode = true;
	} else if (!strcmp(display_mode, "install") ||
		   !strcmp(display_mode, "error")) {
		recovery_mode = false;
		state_error = !strcmp(display_mode, "error");
		if (initial_step > ONEKVM_INSTALL_STEPS)
			return -EINVAL;
		state_step = initial_step;
	} else {
		pr_err("onekvm-installing-oled: invalid mode '%s'\n",
		       display_mode);
		return -EINVAL;
	}

	if (!rtos_open()) {
		rtos_live = true;
		mutex_lock(&display_lock);
		ret = recovery_mode ?
			      rtos_post(ONEKVM_OLED_CMD_RECOVERY, NULL, 0) :
			      show_state_locked();
		if (!ret && !recovery_mode)
			display_running = true;
		mutex_unlock(&display_lock);
		if (!ret) {
			if (display_running && !state_error)
				schedule_delayed_work(&blink_work,
						      msecs_to_jiffies(ONEKVM_BLINK_PERIOD_MS));
			pr_info("onekvm-installing-oled: posted %s screen to C906L\n",
				display_mode);
			return 0;
		}
		rtos_close();
		if (ret != -ETIMEDOUT) {
			pr_err("onekvm-installing-oled: C906L post failed: %d\n",
			       ret);
			return ret;
		}
		pr_info("onekvm-installing-oled: C906L did not acknowledge; probing OLED hardware\n");
	}

	ret = prepare_hardware();
	if (ret)
		goto fail;

	for (profile_index = 0; profile_index < ARRAY_SIZE(profiles);
	     ++profile_index) {
		active_profile = profiles[profile_index];
		for (attempt = 0; attempt < 4; ++attempt) {
			ret = oled_command(0xae);
			if (!ret)
				break;
			mdelay(5);
		}
		if (!ret)
			break;
	}
	if (ret) {
		active_profile = NULL;
		ret = -ENODEV;
		goto fail;
	}
	screen_width = active_profile->width;
	screen_height = active_profile->pages * 8;

	ret = oled_initialize();
	if (ret)
		goto fail;
	mutex_lock(&display_lock);
	if (recovery_mode) {
		if (active_profile == &cube_oled_profile)
			ret = oled_show(cube_recovery_frame_rle,
					ARRAY_SIZE(cube_recovery_frame_rle), true);
		else
			ret = oled_show(pcie_recovery_frame,
					ARRAY_SIZE(pcie_recovery_frame), false);
	} else {
		ret = show_state_locked();
	}
	if (!ret) {
		display_ready = true;
		display_running = !recovery_mode;
		recovery_blink = recovery_mode;
		blink_is_off = false;
	}
	mutex_unlock(&display_lock);
	if (ret)
		goto fail;

	if (recovery_mode || !state_error)
		schedule_delayed_work(&blink_work,
				      msecs_to_jiffies(ONEKVM_BLINK_PERIOD_MS));
	pr_info("onekvm-installing-oled: %s %s screen active\n",
		active_profile->name, display_mode);
	return 0;

fail:
	restore_hardware_state();
	if (onekvm_pinmux)
		iounmap(onekvm_pinmux);
	onekvm_pinmux = NULL;
	active_profile = NULL;
	return ret;
}

static void __exit onekvm_installing_oled_exit(void)
{
	static const u8 off_sequence[] = { 0x8d, 0x10, 0xae };
	static const u8 blank[ONEKVM_OLED_MAX_FRAME_SIZE];

	mutex_lock(&display_lock);
	display_running = false;
	recovery_blink = false;
	mutex_unlock(&display_lock);
	cancel_delayed_work_sync(&blink_work);
	mutex_lock(&display_lock);
	if (rtos_live) {
		/* Hand the panel back to the boot animation. */
		rtos_post(ONEKVM_OLED_CMD_BOOT, NULL, 0);
		rtos_close();
	} else if (display_ready) {
		if (blink_is_off)
			oled_command(0xaf);
		oled_show(blank, active_profile->width * active_profile->pages,
			  false);
		oled_write(0x00, off_sequence, sizeof(off_sequence));
		display_ready = false;
	}
	mutex_unlock(&display_lock);
	restore_hardware_state();
	if (onekvm_pinmux)
		iounmap(onekvm_pinmux);
	active_profile = NULL;
	pr_info("onekvm-installing-oled: display released\n");
}

module_init(onekvm_installing_oled_init);
module_exit(onekvm_installing_oled_exit);

MODULE_DESCRIPTION("OneKVM NanoKVM install, setup error and recovery OLED screens");
MODULE_AUTHOR("OneKVM");
MODULE_LICENSE("GPL");
