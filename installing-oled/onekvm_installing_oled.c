// SPDX-License-Identifier: GPL-2.0-only
/*
 * NanoKVM early-mode OLED indicator.
 *
 * This bit-bangs three lines through gpiolib so the DesignWare GPIO driver's
 * output shadow remains coherent.  The initramfs does not have to load the
 * normal GPIO-I2C stack before storage migration.  Installation mode remains
 * PCIe-only; recovery mode probes the Cube OLED (0x3d) before the PCIe OLED
 * (0x3c) and selects the matching canvas and display geometry.
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
MODULE_PARM_DESC(mode, "OLED canvas to display: install or recovery");

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
 * assets/canvases/system-installing-pcie.json at rotation 0 degrees.
 * Source canvas SHA-256:
 * d3a079b1e3e0cce1409b0dafbd2f722fe48ed4088fbe9b0dc2d9d70d16ed97e4
 */
static const u8 pcie_installing_frame[ONEKVM_PCIE_OLED_WIDTH *
				      ONEKVM_PCIE_OLED_PAGES] = {
	0xc0, 0x40, 0x40, 0x40, 0x80, 0x00, 0xc0, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x40, 0x40,
	0x40, 0x00, 0x80, 0x40, 0x40, 0x40, 0x80, 0x00,
	0x80, 0x40, 0x40, 0x40, 0x40, 0x00, 0xc0, 0x40,
	0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xc0, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x80, 0x40,
	0x40, 0x40, 0x80, 0x00, 0x40, 0x40, 0xc0, 0x40,
	0x40, 0x00, 0x40, 0x40, 0xc0, 0x40, 0x40, 0x00,
	0x07, 0x01, 0x01, 0x01, 0x00, 0x00, 0x07, 0x44,
	0x44, 0xc4, 0x44, 0x00, 0xc7, 0x85, 0x05, 0xc5,
	0x04, 0x80, 0x47, 0x41, 0x41, 0x01, 0x47, 0x40,
	0xc4, 0x45, 0x05, 0x85, 0x42, 0x40, 0x87, 0x05,
	0xc5, 0x05, 0x04, 0x00, 0x00, 0xc0, 0x00, 0x00,
	0x07, 0x02, 0x41, 0x42, 0xc7, 0x40, 0x07, 0xc1,
	0x81, 0x01, 0xc7, 0x00, 0x84, 0x44, 0x47, 0x44,
	0x04, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
	0x04, 0x07, 0x04, 0x00, 0x07, 0x00, 0x01, 0x07,
	0x00, 0x04, 0x05, 0x05, 0x02, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x80, 0x47, 0x41, 0x41, 0x87, 0x00,
	0x87, 0x44, 0x44, 0x44, 0x40, 0x07, 0x04, 0x04,
	0x04, 0x00, 0x04, 0x04, 0x07, 0x04, 0x00, 0x07,
	0x00, 0x01, 0x07, 0x00, 0x03, 0x04, 0x05, 0x07,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x03, 0x04, 0x04, 0x04, 0x03, 0x00,
	0x04, 0x05, 0x05, 0x05, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

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

/*
 * Rendered from the Cube's latest active canvas, saved as
 * assets/canvases/system-installing-cube.json at rotation 0 degrees.  Each
 * byte pair is run length followed by value and expands to 1024 controller
 * bytes.  Source canvas SHA-256:
 * 826926406bb89fb18ec68a1ea177cce8dcedd0b2c3de5deb2bd4f00ce1794173
 */
static const u8 cube_installing_frame_rle[] = {
	0x81, 0x00, 0x02, 0xfe, 0x06, 0x66, 0x02, 0x18,
	0x02, 0x00, 0x02, 0xfe, 0x0a, 0x00, 0x02, 0xfe,
	0x06, 0x66, 0x02, 0x06, 0x02, 0x00, 0x02, 0xf8,
	0x06, 0x66, 0x02, 0xf8, 0x02, 0x00, 0x02, 0x18,
	0x06, 0x66, 0x02, 0x86, 0x02, 0x00, 0x02, 0xfe,
	0x06, 0x66, 0x02, 0x06, 0x0a, 0x00, 0x02, 0xfe,
	0x02, 0x80, 0x02, 0x60, 0x02, 0x80, 0x02, 0xfe,
	0x02, 0x00, 0x02, 0xf8, 0x06, 0x66, 0x02, 0xf8,
	0x02, 0x00, 0x04, 0x06, 0x02, 0xfe, 0x04, 0x06,
	0x02, 0x00, 0x04, 0x06, 0x02, 0xfe, 0x04, 0x06,
	0x02, 0x00, 0x02, 0x07, 0x0a, 0x00, 0x02, 0x07,
	0x08, 0x06, 0x02, 0x00, 0x02, 0x07, 0x08, 0x06,
	0x02, 0x00, 0x02, 0x07, 0x06, 0x00, 0x02, 0x07,
	0x02, 0x00, 0x08, 0x06, 0x02, 0x01, 0x02, 0x00,
	0x02, 0x07, 0x08, 0x06, 0x0a, 0x00, 0x02, 0x07,
	0x02, 0x01, 0x02, 0x00, 0x02, 0x01, 0x02, 0x07,
	0x02, 0x00, 0x02, 0x07, 0x06, 0x00, 0x02, 0x07,
	0x02, 0x00, 0x04, 0x06, 0x02, 0x07, 0x04, 0x06,
	0x06, 0x00, 0x02, 0x07, 0x0a, 0x00, 0x04, 0x18,
	0x02, 0xf8, 0x04, 0x18, 0x02, 0x00, 0x02, 0xf8,
	0x02, 0x60, 0x02, 0x80, 0x02, 0x00, 0x02, 0xf8,
	0x02, 0x00, 0x02, 0x60, 0x06, 0x98, 0x02, 0x18,
	0x02, 0x00, 0x04, 0x18, 0x02, 0xf8, 0x04, 0x18,
	0x02, 0x00, 0x02, 0xe0, 0x06, 0x98, 0x02, 0xe0,
	0x02, 0x00, 0x02, 0xf8, 0x0a, 0x00, 0x02, 0xf8,
	0x0a, 0x00, 0x04, 0x18, 0x02, 0xf8, 0x04, 0x18,
	0x02, 0x00, 0x02, 0xf8, 0x02, 0x60, 0x02, 0x80,
	0x02, 0x00, 0x02, 0xf8, 0x02, 0x00, 0x02, 0xe0,
	0x02, 0x18, 0x06, 0x98, 0x0a, 0x00, 0x04, 0x18,
	0x02, 0x1f, 0x04, 0x18, 0x02, 0x00, 0x02, 0x1f,
	0x02, 0x00, 0x02, 0x01, 0x02, 0x06, 0x02, 0x1f,
	0x02, 0x00, 0x02, 0x18, 0x06, 0x19, 0x02, 0x06,
	0x06, 0x00, 0x02, 0x1f, 0x06, 0x00, 0x02, 0x1f,
	0x06, 0x01, 0x02, 0x1f, 0x02, 0x00, 0x02, 0x1f,
	0x08, 0x18, 0x02, 0x00, 0x02, 0x1f, 0x08, 0x18,
	0x02, 0x00, 0x04, 0x18, 0x02, 0x1f, 0x04, 0x18,
	0x02, 0x00, 0x02, 0x1f, 0x02, 0x00, 0x02, 0x01,
	0x02, 0x06, 0x02, 0x1f, 0x02, 0x00, 0x02, 0x07,
	0x02, 0x18, 0x04, 0x19, 0x02, 0x1f, 0x3a, 0x00,
	0x02, 0xc0, 0x06, 0x30, 0x02, 0xc0, 0x02, 0x00,
	0x02, 0xc0, 0x08, 0x30, 0x6a, 0x00, 0x02, 0x0f,
	0x06, 0x30, 0x02, 0x0f, 0x02, 0x00, 0x02, 0x30,
	0x06, 0x33, 0x02, 0x0c, 0xb5, 0x00,
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

static void onekvm_blink_work(struct work_struct *work)
{
	unsigned long delay;
	int ret;

	(void)work;
	mutex_lock(&display_lock);
	if (!display_ready) {
		mutex_unlock(&display_lock);
		return;
	}

	if (blink_is_off) {
		ret = oled_command(0xaf);
		if (!ret)
			blink_is_off = false;
		delay = msecs_to_jiffies(ONEKVM_BLINK_PERIOD_MS -
					 ONEKVM_BLINK_OFF_MS);
	} else {
		ret = oled_command(0xae);
		if (!ret)
			blink_is_off = true;
		delay = msecs_to_jiffies(ONEKVM_BLINK_OFF_MS);
	}
	if (ret)
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

#define ONEKVM_OLED_SHM_PHYS  0x8FFF0000UL
#define ONEKVM_OLED_SHM_MAGIC 0x44454C4FU
#define ONEKVM_OLED_CMD_INSTALL  3
#define ONEKVM_OLED_CMD_RECOVERY 4

static int onekvm_rtos_post(u32 cmd)
{
	void __iomem *map;
	u32 magic;
	u32 seq;

	map = ioremap(ONEKVM_OLED_SHM_PHYS, 0x1000);
	if (!map)
		return -ENOMEM;
	magic = readl(map);
	if (magic != ONEKVM_OLED_SHM_MAGIC) {
		iounmap(map);
		return -ENODEV;
	}
	writel(cmd, map + 12);
	seq = readl(map + 4) + 1;
	if (!seq)
		seq = 1;
	writel(seq, map + 4);
	iounmap(map);
	return 0;
}

static int __init onekvm_installing_oled_init(void)
{
	static const struct onekvm_oled_profile * const profiles[] = {
		&cube_oled_profile,
		&pcie_oled_profile,
	};
	const u8 *frame;
	size_t frame_size;
	bool frame_is_rle;
	const char *canvas_name;
	bool recovery_mode;
	size_t profile_index;
	size_t profile_count;
	int attempt;
	int ret;
	u32 rtos_cmd;

	if (!strcmp(display_mode, "install"))
		rtos_cmd = ONEKVM_OLED_CMD_INSTALL;
	else if (!strcmp(display_mode, "recovery"))
		rtos_cmd = ONEKVM_OLED_CMD_RECOVERY;
	else
		rtos_cmd = 0;

	if (rtos_cmd) {
		ret = onekvm_rtos_post(rtos_cmd);
		if (!ret) {
			pr_info("onekvm-installing-oled: posted %s canvas to C906L\n",
				display_mode);
			return 0;
		}
		if (ret != -ENODEV) {
			pr_err("onekvm-installing-oled: C906L post failed: %d\n",
			       ret);
			return ret;
		}
	}

	if (!strcmp(display_mode, "install")) {
		recovery_mode = false;
		frame = NULL;
		frame_size = 0;
		frame_is_rle = false;
		canvas_name = "Install OS";
	} else if (!strcmp(display_mode, "recovery")) {
		recovery_mode = true;
		frame = NULL;
		frame_size = 0;
		frame_is_rle = false;
		canvas_name = "Recovery";
	} else {
		pr_err("onekvm-installing-oled: invalid mode '%s'\n",
		       display_mode);
		return -EINVAL;
	}

	ret = prepare_hardware();
	if (ret)
		goto fail;

	profile_count = ARRAY_SIZE(profiles);
	for (profile_index = 0; profile_index < profile_count;
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

	if (active_profile == &cube_oled_profile) {
		if (recovery_mode) {
			frame = cube_recovery_frame_rle;
			frame_size = ARRAY_SIZE(cube_recovery_frame_rle);
		} else {
			frame = cube_installing_frame_rle;
			frame_size = ARRAY_SIZE(cube_installing_frame_rle);
		}
		frame_is_rle = true;
	} else {
		if (recovery_mode) {
			frame = pcie_recovery_frame;
			frame_size = ARRAY_SIZE(pcie_recovery_frame);
		} else {
			frame = pcie_installing_frame;
			frame_size = ARRAY_SIZE(pcie_installing_frame);
		}
	}
	ret = oled_initialize();
	if (ret)
		goto fail;
	ret = oled_show(frame, frame_size, frame_is_rle);
	if (ret)
		goto fail;

	display_ready = true;
	blink_is_off = false;
	schedule_delayed_work(&blink_work,
			      msecs_to_jiffies(ONEKVM_BLINK_PERIOD_MS));
	pr_info("onekvm-installing-oled: %s %s canvas active (3 second blink)\n",
		active_profile->name, canvas_name);
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

	cancel_delayed_work_sync(&blink_work);
	mutex_lock(&display_lock);
	if (display_ready) {
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

MODULE_DESCRIPTION("OneKVM NanoKVM install and recovery OLED indicator");
MODULE_AUTHOR("OneKVM");
MODULE_LICENSE("GPL");
