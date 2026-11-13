// SPDX-License-Identifier: MIT
#define main splash_cli_main
#include "../boot-splash/onekvm-boot-splash.c"
#undef main
#include <assert.h>

#ifndef ONEKVM_SPLASH_GO
static void check_rotation(size_t stride)
{
    uint16_t frame[WIDTH * HEIGHT] = {0};
    size_t size = stride * PANEL_HEIGHT;
    uint8_t *pixels = malloc(size + 16);
    assert(pixels);
    memset(pixels, 0xa5, size + 16);
    frame[0] = 0x1234;
    frame[WIDTH - 1] = 0x5678;
    frame[(HEIGHT - 1) * WIDTH] = 0x9abc;
    frame[WIDTH * HEIGHT - 1] = 0xdef0;
    rotate_panel(pixels, stride, frame);
    assert(pixels[319 * stride] == 0x34 && pixels[319 * stride + 1] == 0x12);
    assert(pixels[0] == 0x78 && pixels[1] == 0x56);
    assert(pixels[319 * stride + 342] == 0xbc && pixels[319 * stride + 343] == 0x9a);
    assert(pixels[342] == 0xf0 && pixels[343] == 0xde);
    for (unsigned y = 0; y < PANEL_HEIGHT; y++)
        for (size_t x = 344; x < stride; x++)
            assert(pixels[y * stride + x] == 0xa5);
    for (unsigned i = 0; i < 16; i++)
        assert(pixels[size + i] == 0xa5);
    free(pixels);
}

static void check_error_message(void)
{
    uint16_t plain[WIDTH * HEIGHT], text[WIDTH * HEIGHT];
    const char *message = "OneKVM NanoKVM storage migration: rootfs A is neither SquashFS nor EROFS; refusing initialization";
    render(plain, 0, 2, &labels_en, NULL);
    render(text, 0, 2, &labels_en, message);
    assert(memcmp(plain, text, sizeof(plain)) != 0);
    unsigned header_pixels = 0;
    for (unsigned y = 0; y < 42; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            if (text[y * WIDTH + x] != rgb565(8, 11, 17))
                header_pixels++;
    assert(header_pixels > 100);

    /* Long and nonprintable messages must stay inside the header. */
    uint16_t bounded[WIDTH * HEIGHT + 2];
    char long_message[1024];
    memset(long_message, 'W', sizeof(long_message) - 1);
    long_message[0] = '\t';
    long_message[1] = (char)0xff;
    long_message[sizeof(long_message) - 1] = '\0';
    for (unsigned i = 0; i < WIDTH * HEIGHT + 2; i++)
        bounded[i] = 0x1234;
    draw_log_message(bounded + 1, long_message);
    assert(bounded[0] == 0x1234 && bounded[WIDTH * HEIGHT + 1] == 0x1234);
    unsigned last_row_pixels = 0;
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            uint16_t pixel = bounded[1 + y * WIDTH + x];
            if (y < 6 || y >= 41 || x < 12 || x >= 306)
                assert(pixel == 0x1234);
            if (y >= 30 && y < 41 && pixel != 0x1234)
                last_row_pixels++;
        }
    }
    assert(last_row_pixels > 0);

    /* Boot/install layouts ignore error text, and empty text keeps old layout. */
    render(plain, 0, 0, &labels_en, NULL);
    render(text, 0, 0, &labels_en, message);
    assert(!memcmp(plain, text, sizeof(plain)));
    render(plain, 0, 2, &labels_en, NULL);
    render(text, 0, 2, &labels_en, "");
    assert(!memcmp(plain, text, sizeof(plain)));

    char output[] = "/tmp/onekvm-splash-message-XXXXXX";
    int fd = mkstemp(output);
    assert(fd >= 0);
    close(fd);
    char *argv[] = { "splash", "--message", (char *)message,
                     "--language", "zh", "--output", output, "error-saved", NULL };
    assert(splash_cli_main(8, argv) == 0);
    unlink(output);
}

static void check_desk(void)
{
    struct fb_var_screeninfo var = {
        .xres = 172, .yres = 320, .xres_virtual = 172, .yres_virtual = 320,
        .bits_per_pixel = 16,
        .red = { .offset = 11, .length = 5 },
        .green = { .offset = 5, .length = 6 },
        .blue = { .offset = 0, .length = 5 },
    };
    struct fb_fix_screeninfo fix = {
        .type = FB_TYPE_PACKED_PIXELS, .visual = FB_VISUAL_TRUECOLOR,
        .line_length = 344, .smem_len = 344 * 320,
    };
    assert(valid_panel(&var, &fix));
    var.nonstd = 1;
    assert(!valid_panel(&var, &fix));
    memcpy(fix.id, "fb_jd9853", sizeof("fb_jd9853"));
    assert(valid_panel(&var, &fix));
    var.nonstd = 2;
    assert(!valid_panel(&var, &fix));
    var.nonstd = 1;
    var.red.offset = 0;
    assert(!valid_panel(&var, &fix));
    var.red.offset = 11;
    var.xoffset = 1;
    assert(!valid_panel(&var, &fix));
    var.xoffset = 0;
    fix.smem_len--;
    assert(!valid_panel(&var, &fix));
    {
        uint16_t en[WIDTH * HEIGHT], zh[WIDTH * HEIGHT], tw[WIDTH * HEIGHT];
        assert(language_labels("zh-TW") == language_labels("zh_tw"));
        assert(language_labels("unknown") == &labels_en);
        for (unsigned stage = 0; stage <= 7; stage++) {
            render(en, stage, 0, &labels_en, NULL);
            render(zh, stage, 0, &labels_zh, NULL);
            render(tw, stage, 0, &labels_zh_tw, NULL);
            assert(memcmp(en, zh, sizeof(en)) != 0);
            assert(memcmp(en, tw, sizeof(en)) != 0);
        }
        render(en, 0, 1, &labels_en, NULL);
        render(zh, 0, 1, &labels_zh, NULL);
        assert(memcmp(en, zh, sizeof(en)) != 0);
        {
            uint16_t saved_en[WIDTH * HEIGHT], saved_zh[WIDTH * HEIGHT];
            render(saved_en, 0, 2, &labels_en, NULL);
            render(saved_zh, 0, 2, &labels_zh, NULL);
            assert(memcmp(en, saved_en, sizeof(en)) != 0);
            assert(memcmp(saved_en, saved_zh, sizeof(saved_en)) != 0);
        }
    }
    check_rotation(344);
    check_rotation(360);
    check_error_message();
}
#else
static void check_go_copy(size_t stride)
{
    uint16_t frame[WIDTH * HEIGHT] = {0};
    size_t size = stride * PANEL_HEIGHT;
    uint8_t *pixels = malloc(size + 16);
    assert(pixels);
    memset(pixels, 0xa5, size + 16);
    frame[0] = 0x1234;
    frame[WIDTH - 1] = 0x5678;
    frame[(HEIGHT - 1) * WIDTH] = 0x9abc;
    frame[WIDTH * HEIGHT - 1] = 0xdef0;
    rotate_panel(pixels, stride, frame);
    /* Canvas top-left is the framebuffer's top-right, as the screen service. */
    assert(pixels[478] == 0x34 && pixels[479] == 0x12);
    assert(pixels[283 * stride + 478] == 0x78 && pixels[283 * stride + 479] == 0x56);
    assert(pixels[0] == 0xbc && pixels[1] == 0x9a);
    assert(pixels[283 * stride] == 0xf0 && pixels[283 * stride + 1] == 0xde);
    for (unsigned y = 0; y < PANEL_HEIGHT; y++)
        for (size_t x = 480; x < stride; x++)
            assert(pixels[y * stride + x] == 0xa5);
    for (unsigned i = 0; i < 16; i++)
        assert(pixels[size + i] == 0xa5);
    free(pixels);
}

static void check_go_render(void)
{
    static uint16_t bounded[WIDTH * HEIGHT + 2];
    const struct splash_labels *all[] = { &labels_en, &labels_zh, &labels_zh_tw };
    const uint16_t background = rgb565(8, 11, 17);

    for (unsigned language = 0; language < 3; language++) {
        for (unsigned mode = 0; mode <= 10; mode++) {
            unsigned stage = mode <= 7 ? mode : 0;
            int failed = mode <= 7 ? 0 : (int)mode - 8;
            const char *message = mode == 10 ? "OneKVM Go: NFS mount failed" : NULL;
            bounded[0] = bounded[WIDTH * HEIGHT + 1] = 0x1234;
            render(bounded + 1, stage, failed ? failed : (mode == 10 ? 2 : 0),
                   all[language], message);
            assert(bounded[0] == 0x1234 && bounded[WIDTH * HEIGHT + 1] == 0x1234);
        }
    }

    /* The boot logo sits in the vertically centered 172-row layout. */
    render(bounded + 1, 0, 0, &labels_en, NULL);
    unsigned top = HEIGHT, bottom = 0;
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            if (bounded[1 + y * WIDTH + x] != background) {
                if (y < top)
                    top = y;
                bottom = y;
            }
    assert(top >= TOP_PAD && bottom < TOP_PAD + LAYOUT_HEIGHT);
    assert(top + bottom > HEIGHT - 40 && top + bottom < HEIGHT + 40);

    char output[] = "/tmp/onekvm-splash-go-XXXXXX";
    int fd = mkstemp(output);
    assert(fd >= 0);
    close(fd);
    char *argv[] = { "splash", "--output", output, "boot", NULL };
    assert(splash_cli_main(4, argv) == 0);
    unlink(output);
}

static void check_go(void)
{
    struct fb_var_screeninfo var = {
        .xres = 240, .yres = 284, .xres_virtual = 240, .yres_virtual = 284,
        .bits_per_pixel = 16, .nonstd = 1,
        .red = { .offset = 11, .length = 5 },
        .green = { .offset = 5, .length = 6 },
        .blue = { .offset = 0, .length = 5 },
    };
    struct fb_fix_screeninfo fix = {
        .type = FB_TYPE_PACKED_PIXELS, .visual = FB_VISUAL_TRUECOLOR,
        .line_length = 480, .smem_len = 480 * 284,
    };
    memcpy(fix.id, "fb_jd9853", sizeof("fb_jd9853"));
    assert(!valid_panel(&var, &fix));
    memcpy(fix.id, "fb_st7789p3", sizeof("fb_st7789p3"));
    assert(valid_panel(&var, &fix));
    var.xres = 284;
    var.yres = 240;
    assert(!valid_panel(&var, &fix));
    check_go_copy(480);
    check_go_copy(512);
    check_go_render();
}
#endif

int main(void)
{
#ifdef ONEKVM_SPLASH_GO
    check_go();
    puts("Go boot splash pixel format, layout and stride tests passed");
#else
    check_desk();
    puts("boot splash pixel format, rotation and stride tests passed");
#endif
    return 0;
}
