// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef ONEKVM_SPLASH_GO
/* NanoKVM Go: landscape canvas on a 240x284 ST7789P3 framebuffer. */
enum { WIDTH = 284, HEIGHT = 240, PANEL_WIDTH = 240, PANEL_HEIGHT = 284 };
#define PANEL_FBTFT_ID "fb_st7789p3"
#else
/* Pro Desk: landscape canvas on a 172x320 JD9853 framebuffer. */
enum { WIDTH = 320, HEIGHT = 172, PANEL_WIDTH = 172, PANEL_HEIGHT = 320 };
#define PANEL_FBTFT_ID "fb_jd9853"
#endif
/* Layouts are drawn for 172 rows and centered vertically on taller canvases. */
enum { LAYOUT_HEIGHT = 172, TOP_PAD = (HEIGHT - LAYOUT_HEIGHT) / 2 };

struct splash_asset {
    unsigned width, height;
    const uint16_t *runs;
    size_t words;
};

struct splash_labels {
    const struct splash_asset *starting, *installing, *warning, *paused, *error_detail, *saved_detail;
    const struct splash_asset *stages[7];
};

#include "assets.h"
#include "log-font.h"

static const struct splash_labels *language_labels(const char *language)
{
    if (!strcmp(language, "zh") || !strcmp(language, "zh-CN"))
        return &labels_zh;
    if (!strcmp(language, "zh_tw") || !strcmp(language, "zh-TW"))
        return &labels_zh_tw;
    return &labels_en;
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void draw_asset(uint16_t *frame, const struct splash_asset *asset, unsigned y)
{
    size_t pixel = 0;
    /* Wider artwork is clipped at the right edge instead of wrapping. */
    unsigned left = asset->width < WIDTH ? (WIDTH - asset->width) / 2 : 0;

    for (size_t i = 0; i + 1 < asset->words; i += 2) {
        for (unsigned n = 0; n < asset->runs[i]; n++, pixel++) {
            unsigned row = (unsigned)(pixel / asset->width) + y;
            unsigned col = (unsigned)(pixel % asset->width) + left;
            if (row < HEIGHT && col < WIDTH && pixel < asset->width * asset->height)
                frame[row * WIDTH + col] = asset->runs[i + 1];
        }
    }
}

enum { BAR_WIDTH = 232, BAR_LEFT = (WIDTH - BAR_WIDTH) / 2 };

static void draw_bar(uint16_t *frame, unsigned width, uint16_t color)
{
    for (unsigned y = 0; y < 4; y++) {
        unsigned inset = (y == 0 || y == 3) ? 2 : 0;
        for (unsigned x = inset; x + inset < width; x++)
            frame[(TOP_PAD + 130 + y) * WIDTH + BAR_LEFT + x] = color;
    }
}

static void draw_log_message(uint16_t *frame, const char *message)
{
    enum { LEFT = 12, TOP = TOP_PAD + 6, COLUMNS = (WIDTH - 2 * LEFT) / LOG_FONT_WIDTH, ROWS = 3 };
    char text[COLUMNS * ROWS];
    size_t length = 0, offset = 0;
    while (message[length] && length < sizeof(text)) {
        unsigned char c = (unsigned char)message[length];
        text[length++] = c >= 32 && c <= 126 ? (char)c : (c < 32 ? ' ' : '?');
    }
    for (unsigned row = 0; row < ROWS && offset < length; row++) {
        size_t count = length - offset;
        int shortened = row == ROWS - 1 && (count > COLUMNS || message[length]);
        if (count > COLUMNS)
            count = COLUMNS;
        if (row < ROWS - 1 && offset + count < length) {
            size_t word_end = count;
            while (word_end && text[offset + word_end] != ' ')
                word_end--;
            if (word_end)
                count = word_end;
        }
        for (size_t col = 0; col < count; col++) {
            unsigned char c = shortened && col + 3 >= count ? '.' : (unsigned char)text[offset + col];
            for (unsigned y = 0; y < LOG_FONT_HEIGHT; y++) {
                for (unsigned x = 0; x < LOG_FONT_WIDTH; x++) {
                    if (log_font[c - 32][y] & (1u << x))
                        frame[(TOP + row * (LOG_FONT_HEIGHT + 1) + y) * WIDTH +
                              LEFT + col * LOG_FONT_WIDTH + x] = rgb565(234, 238, 245);
                }
            }
        }
        offset += count;
        while (offset < length && text[offset] == ' ')
            offset++;
    }
}

/* Storage stages advance the bar; it is not a time estimate. */
static void render(uint16_t *frame, unsigned stage, int failed,
                   const struct splash_labels *labels, const char *message)
{

    for (unsigned i = 0; i < WIDTH * HEIGHT; i++)
        frame[i] = rgb565(8, 11, 17);
    if (failed) {
        int has_message = message && *message;
        unsigned shift = TOP_PAD + (has_message ? 28 : 0);
        if (has_message)
            draw_log_message(frame, message);
        draw_asset(frame, &logo_install, 23 + shift);
        draw_asset(frame, labels->paused, 86 + shift);
        draw_asset(frame, failed > 1 ? labels->saved_detail : labels->error_detail, 114 + shift);
    } else if (stage) {
        draw_asset(frame, &logo_install, TOP_PAD + 23);
        draw_asset(frame, labels->installing, TOP_PAD + 84);
        draw_asset(frame, labels->stages[stage - 1], TOP_PAD + 108);
        draw_bar(frame, BAR_WIDTH, rgb565(31, 41, 59));
        draw_bar(frame, BAR_WIDTH * stage / 7, rgb565(22, 119, 255));
        draw_asset(frame, labels->warning, TOP_PAD + 148);
    } else {
        draw_asset(frame, &logo_boot, TOP_PAD + 48);
        draw_asset(frame, labels->starting, TOP_PAD + 125);
    }
}

static int valid_panel(const struct fb_var_screeninfo *var,
                       const struct fb_fix_screeninfo *fix)
{
    return var->xres == PANEL_WIDTH && var->yres == PANEL_HEIGHT &&
        var->xres_virtual >= PANEL_WIDTH && var->yres_virtual >= PANEL_HEIGHT &&
        var->xoffset == 0 && var->yoffset == 0 && var->bits_per_pixel == 16 &&
        var->red.offset == 11 && var->red.length == 5 && !var->red.msb_right &&
        var->green.offset == 5 && var->green.length == 6 && !var->green.msb_right &&
        var->blue.offset == 0 && var->blue.length == 5 && !var->blue.msb_right &&
        /* fbtft sets nonstd=1 even though these bitfields describe RGB565. */
        var->transp.length == 0 && !var->grayscale &&
        (!var->nonstd || (var->nonstd == 1 &&
         !strncmp(fix->id, PANEL_FBTFT_ID, sizeof(fix->id)))) &&
        fix->type == FB_TYPE_PACKED_PIXELS && fix->visual == FB_VISUAL_TRUECOLOR &&
        fix->line_length >= PANEL_WIDTH * 2 && fix->line_length <= 4096 &&
        fix->smem_len >= fix->line_length * PANEL_HEIGHT;
}

#ifdef ONEKVM_SPLASH_GO
/* Go panel is mounted 90 degrees from the framebuffer's native orientation. */
static void rotate_panel(uint8_t *pixels, size_t stride, const uint16_t *frame)
{
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            size_t offset = x * stride + (PANEL_WIDTH - 1 - y) * 2;
            uint16_t color = frame[y * WIDTH + x];
            pixels[offset] = (uint8_t)color;
            pixels[offset + 1] = (uint8_t)(color >> 8);
        }
    }
}
#else
/* Desk panel is mounted 270 degrees from the framebuffer's native orientation. */
static void rotate_panel(uint8_t *pixels, size_t stride, const uint16_t *frame)
{
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            size_t offset = (PANEL_HEIGHT - 1 - x) * stride + y * 2;
            uint16_t color = frame[y * WIDTH + x];
            pixels[offset] = (uint8_t)color;
            pixels[offset + 1] = (uint8_t)(color >> 8);
        }
    }
}
#endif

static int show_panel(const char *path, const uint16_t *frame)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *pixels;
    size_t size, done = 0;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    int result = -1;

    if (fd < 0)
        return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0)
        goto close_panel;
    if (!valid_panel(&var, &fix)) {
        errno = ENOTSUP;
        goto close_panel;
    }
    size = (size_t)fix.line_length * PANEL_HEIGHT;
    pixels = calloc(1, size);
    if (!pixels)
        goto close_panel;
    rotate_panel(pixels, fix.line_length, frame);
    while (done < size) {
        ssize_t written = pwrite(fd, pixels + done, size - done, (off_t)done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (!written)
                errno = EIO;
            goto free_pixels;
        }
        done += (size_t)written;
    }
    /* Leave the last frame visible; the normal screen service takes over. */
    (void)ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
    result = 0;
free_pixels:
    free(pixels);
close_panel:
    {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return result;
}

static int write_preview(const char *path, const uint16_t *frame)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    if (fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT) < 0)
        goto failed;
    for (unsigned i = 0; i < WIDTH * HEIGHT; i++) {
        unsigned r = (frame[i] >> 11) & 31, g = (frame[i] >> 5) & 63;
        unsigned b = frame[i] & 31;
        uint8_t rgb[] = {
            (uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 2) | (g >> 4)),
            (uint8_t)((b << 3) | (b >> 2)),
        };
        if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb))
            goto failed;
    }
    return fclose(file);
failed:
    fclose(file);
    return -1;
}

int main(int argc, char **argv)
{
    uint16_t frame[WIDTH * HEIGHT];
    const char *preview = NULL, *device = "/dev/fb0", *message = NULL;
    unsigned stage = 0;
    int failed = 0;
    const struct splash_labels *labels = &labels_en;

    while (argc >= 3) {
        if (!strcmp(argv[1], "--language"))
            labels = language_labels(argv[2]);
        else if (!strcmp(argv[1], "--message"))
            message = argv[2];
        else if (!strcmp(argv[1], "--output"))
            preview = argv[2];
        else if (!strcmp(argv[1], "--framebuffer"))
            device = argv[2];
        else
            break;
        argc -= 2;
        argv += 2;
    }
    if (argc == 2 && !strcmp(argv[1], "boot")) {
        stage = 0;
    } else if (argc == 2 && !strcmp(argv[1], "error")) {
        failed = 1;
    } else if (argc == 2 && !strcmp(argv[1], "error-saved")) {
        failed = 2;
    } else if (argc == 3 && !strcmp(argv[1], "install") &&
               strlen(argv[2]) == 1 && argv[2][0] >= '1' && argv[2][0] <= '7') {
        stage = (unsigned)(argv[2][0] - '0');
    } else {
        fprintf(stderr, "Usage: onekvm-boot-splash [--language en|zh|zh_tw] [--message TEXT] [--output FILE.ppm | "
                "--framebuffer /dev/fb0] {boot | install STAGE(1-7) | error | error-saved}\n");
        return 2;
    }
    render(frame, stage, failed, labels, message);
    if ((preview ? write_preview(preview, frame) : show_panel(device, frame)) < 0) {
        perror("onekvm-boot-splash");
        return 1;
    }
    return 0;
}
