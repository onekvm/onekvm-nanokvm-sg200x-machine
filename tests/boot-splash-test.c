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

static void check_error_message(void)
{
    uint16_t plain[WIDTH * HEIGHT], text[WIDTH * HEIGHT];
    const char *message = "OneKVM NanoKVM storage migration: rootfs A is neither SquashFS nor EROFS; refusing initialization";
    render(plain, 0, 2, 0, &labels_en, NULL);
    render(text, 0, 2, 0, &labels_en, message);
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
    render(plain, 0, 0, 0, &labels_en, NULL);
    render(text, 0, 0, 0, &labels_en, message);
    assert(!memcmp(plain, text, sizeof(plain)));
    render(plain, 0, 2, 0, &labels_en, NULL);
    render(text, 0, 2, 0, &labels_en, "");
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

static unsigned fill_width(const uint16_t *frame)
{
    unsigned width = 0;
    for (unsigned x = 0; x < BOOT_BAR_WIDTH; x++)
        width += frame[(BOOT_BAR_Y + 1) * WIDTH + BOOT_BAR_X + x] == bar_fill();
    return width;
}

static void check_boot_progress(size_t stride)
{
    uint16_t start[WIDTH * HEIGHT], later[WIDTH * HEIGHT], scratch[WIDTH * HEIGHT];
    size_t size = stride * PANEL_HEIGHT, band, first = boot_bar_band(stride, &band);
    uint8_t *pixels = malloc(size), *expected = malloc(size), *before = malloc(size);
    assert(pixels && expected && before);

    /* The boot screen always carries the bar; other layouts never do. */
    render(start, 0, 0, 5, &labels_zh, NULL);
    render(later, 0, 0, 60, &labels_zh, NULL);
    assert(has_boot_bar(start) && has_boot_bar(later));
    assert(fill_width(start) == BOOT_BAR_WIDTH * 5 / 100);
    assert(fill_width(later) == BOOT_BAR_WIDTH * 60 / 100);
    render(scratch, 0, 0, 100, &labels_en, NULL);
    assert(fill_width(scratch) == BOOT_BAR_WIDTH);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            if (y < BOOT_BAR_Y || y >= BOOT_BAR_Y + BAR_HEIGHT ||
                x < BOOT_BAR_X || x >= BOOT_BAR_X + BOOT_BAR_WIDTH)
                assert(start[y * WIDTH + x] == later[y * WIDTH + x]);
    for (unsigned stage = 1; stage <= 7; stage++) {
        render(scratch, stage, 0, 0, &labels_en, NULL);
        assert(!has_boot_bar(scratch));
    }
    render(scratch, 0, 1, 0, &labels_en, NULL);
    assert(!has_boot_bar(scratch));
    render(scratch, 0, 2, 0, &labels_en, "message");
    assert(!has_boot_bar(scratch));

    /* A bar-only update keeps the caption and touches only the bar band. */
    memset(pixels, 0xa5, size);
    rotate_panel(pixels, stride, start);
    memcpy(before, pixels, size);
    memset(expected, 0xa5, size);
    rotate_panel(expected, stride, later);
    assert(update_boot_bar(pixels, stride, scratch, 60) == 1);
    assert(!memcmp(pixels, expected, size));
    assert(!memcmp(pixels, before, first));
    assert(!memcmp(pixels + first + band, before + first + band, size - first - band));
    unrotate_panel(scratch, pixels, stride);
    assert(!memcmp(scratch, later, sizeof(later)));

    /* Without a boot bar, for example over an install screen, nothing changes. */
    render(scratch, 3, 0, 0, &labels_en, NULL);
    rotate_panel(pixels, stride, scratch);
    memcpy(before, pixels, size);
    assert(update_boot_bar(pixels, stride, scratch, 60) == 0);
    assert(!memcmp(pixels, before, size));

    assert(parse_percent("0") == 0 && parse_percent("100") == 100 && parse_percent("085") == 85);
    assert(parse_percent("storage") == 10 && parse_percent("screen") == 78 && parse_percent("done") == 100);
    assert(parse_percent("Screen") < 0 && parse_percent("later") < 0);
    assert(parse_percent("") < 0 && parse_percent("101") < 0 && parse_percent("-1") < 0 &&
           parse_percent("5%") < 0 && parse_percent("1000") < 0);
    {
        char *bad[] = { "splash", "--output", "/dev/null", "progress", "50", NULL };
        assert(splash_cli_main(5, bad) == 2);
        char *range[] = { "splash", "--progress", "200", "boot", NULL };
        assert(splash_cli_main(4, range) == 2);
    }
    free(pixels);
    free(expected);
    free(before);
}

static void write_file(int fd, const uint8_t *pixels, size_t size)
{
    assert(pwrite(fd, pixels, size, 0) == (ssize_t)size);
}

static void read_file(int fd, uint8_t *pixels, size_t size)
{
    assert(pread(fd, pixels, size, 0) == (ssize_t)size);
}

static void check_animation(size_t stride)
{
    uint16_t frame[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
    size_t size = stride * PANEL_HEIGHT;
    uint8_t *pixels = malloc(size), *before = malloc(size);
    char path[] = "/tmp/onekvm-splash-fb-XXXXXX";
    int fd = mkstemp(path);
    assert(pixels && before && fd >= 0);
    unlink(path);

    /* Stage table: rising checkpoints; easing stays short of the next one. */
    for (int i = 0; i + 1 < BOOT_STAGES; i++) {
        unsigned next = percent_width(boot_stages[i + 1].percent), last = 0;
        assert(boot_stages[i].percent < boot_stages[i + 1].percent && boot_stages[i].next_ms);
        assert(boot_stage(percent_width(boot_stages[i].percent)) == i);
        assert(boot_stage(next - 1) == i);
        for (unsigned long ms = 0; ms <= 600000; ms = ms ? ms * 2 : 10) {
            unsigned target = boot_stage_target(i, ms);
            assert(target >= last && target < next);
            last = target;
        }
        /* Steady until the usual duration, at 90% of the way by then. */
        {
            unsigned base = percent_width(boot_stages[i].percent), usual = boot_stages[i].next_ms;
            unsigned half = boot_stage_target(i, usual / 2) - base, full = boot_stage_target(i, usual) - base;
            assert(full == (next - base) * 9 / 10);
            assert(half + 1 >= full / 2 && half <= full / 2 + 1);
        }
    }
    assert(boot_stages[BOOT_STAGES - 1].percent == 100 && boot_stage(0) == -1);

    /* Painted columns match a fully rendered bar of the same width. */
    {
        render(frame, 0, 0, 15, &labels_en, NULL);
        memset(pixels, 0xa5, size);
        rotate_panel(pixels, stride, frame);
        paint_bar(pixels, stride, percent_width(15) - 2, 70);
        unrotate_panel(frame, pixels, stride);
        render(expected, 0, 0, 0, &labels_en, NULL);
        for (unsigned y = 0; y < BAR_HEIGHT; y++)
            for (unsigned x = 0; x < BOOT_BAR_WIDTH; x++)
                expected[(BOOT_BAR_Y + y) * WIDTH + BOOT_BAR_X + x] = boot_bar_pixel(x, y, 70);
        assert(!memcmp(frame, expected, sizeof(frame)));
    }

    /* The band reader sees the bar and its fill. */
    {
        size_t band_size, offset = bar_edge_band(stride, &band_size);
        uint16_t row[WIDTH];
        render(frame, 0, 0, 40, &labels_zh, NULL);
        rotate_panel(pixels, stride, frame);
        band_bar_row(row, pixels + offset, stride);
        assert(boot_bar_row_valid(row) && boot_bar_row_fill(row) == percent_width(40));
        render(frame, 4, 0, 0, &labels_zh, NULL);
        rotate_panel(pixels, stride, frame);
        band_bar_row(row, pixels + offset, stride);
        assert(!boot_bar_row_valid(row));
    }

    /* Running the animator moves the bar forward, never past the next checkpoint. */
    render(frame, 0, 0, parse_percent("basic"), &labels_en, NULL);
    memset(pixels, 0xa5, size);
    rotate_panel(pixels, stride, frame);
    write_file(fd, pixels, size);
    memcpy(before, pixels, size);
    stop_animation = 0;
    assert(animate_fd(fd, stride, 700) == 0);
    read_file(fd, pixels, size);
    unrotate_panel(frame, pixels, stride);
    {
        unsigned fill = boot_bar_row_fill(frame + (BOOT_BAR_Y + 1) * WIDTH);
        size_t band_size, offset = bar_edge_band(stride, &band_size);
        assert(fill > percent_width(65) && fill < percent_width(78));
        assert(!memcmp(pixels, before, offset));
        assert(!memcmp(pixels + offset + band_size, before + offset + band_size, size - offset - band_size));
        for (size_t row = 0; row < PANEL_HEIGHT; row++)
            for (size_t byte = PANEL_WIDTH * 2; byte < stride; byte++)
                assert(pixels[row * stride + byte] == 0xa5);
    }

    /* A checkpoint never shortens a longer bar. */
    assert(update_boot_bar(pixels, stride, frame, 65) == 0);
    assert(update_boot_bar(pixels, stride, frame, 78) == 1);

    /* Only rows that differ are copied; others keep what the panel has. */
    {
        size_t band_size, offset = boot_bar_band(stride, &band_size);
        uint8_t *panel = malloc(size);
        assert(panel);
        memcpy(panel, pixels, size);
        memcpy(before, pixels, size);
        before[offset + stride + 5] ^= 1;
        panel[offset + 2 * stride + 7] ^= 1;
        copy_changed_rows(panel, before, offset, band_size, stride);
        assert(!memcmp(panel, before, size));
        free(panel);
    }

    /* Without a boot bar the animator stops at once and writes nothing. */
    render(frame, 0, 1, 0, &labels_en, NULL);
    rotate_panel(pixels, stride, frame);
    write_file(fd, pixels, size);
    memcpy(before, pixels, size);
    assert(animate_fd(fd, stride, 5000) == 0);
    read_file(fd, pixels, size);
    assert(!memcmp(pixels, before, size));

    close(fd);
    free(pixels);
    free(before);
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
            render(en, stage, 0, 0, &labels_en, NULL);
            render(zh, stage, 0, 0, &labels_zh, NULL);
            render(tw, stage, 0, 0, &labels_zh_tw, NULL);
            assert(memcmp(en, zh, sizeof(en)) != 0);
            assert(memcmp(en, tw, sizeof(en)) != 0);
        }
        render(en, 0, 1, 0, &labels_en, NULL);
        render(zh, 0, 1, 0, &labels_zh, NULL);
        assert(memcmp(en, zh, sizeof(en)) != 0);
        {
            uint16_t saved_en[WIDTH * HEIGHT], saved_zh[WIDTH * HEIGHT];
            render(saved_en, 0, 2, 0, &labels_en, NULL);
            render(saved_zh, 0, 2, 0, &labels_zh, NULL);
            assert(memcmp(en, saved_en, sizeof(en)) != 0);
            assert(memcmp(saved_en, saved_zh, sizeof(saved_en)) != 0);
        }
    }
    check_rotation(344);
    check_rotation(360);
    check_error_message();
    check_boot_progress(344);
    check_boot_progress(360);
    check_animation(344);
    check_animation(360);
    puts("boot splash pixel format, rotation, stride, progress and animation tests passed");
    return 0;
}
