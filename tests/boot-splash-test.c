// SPDX-License-Identifier: MIT
#define main splash_cli_main
#include "../boot-splash/onekvm-boot-splash.c"
#undef main
#include <assert.h>

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

int main(void)
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
            render(en, stage, 0, &labels_en);
            render(zh, stage, 0, &labels_zh);
            render(tw, stage, 0, &labels_zh_tw);
            assert(memcmp(en, zh, sizeof(en)) != 0);
            assert(memcmp(en, tw, sizeof(en)) != 0);
        }
        render(en, 0, 1, &labels_en);
        render(zh, 0, 1, &labels_zh);
        assert(memcmp(en, zh, sizeof(en)) != 0);
    }
    check_rotation(344);
    check_rotation(360);
    puts("boot splash pixel format, rotation and stride tests passed");
    return 0;
}
