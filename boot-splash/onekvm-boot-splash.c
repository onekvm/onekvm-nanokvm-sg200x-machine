// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 320, HEIGHT = 172, PANEL_WIDTH = 172, PANEL_HEIGHT = 320 };
enum { BAR_HEIGHT = 4, INSTALL_BAR_X = 44, INSTALL_BAR_Y = 130, INSTALL_BAR_WIDTH = 232 };
enum { BOOT_BAR_X = 80, BOOT_BAR_Y = 150, BOOT_BAR_WIDTH = 160 };

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
    unsigned left = (WIDTH - asset->width) / 2;

    for (size_t i = 0; i + 1 < asset->words; i += 2) {
        for (unsigned n = 0; n < asset->runs[i]; n++, pixel++) {
            unsigned row = (unsigned)(pixel / asset->width) + y;
            unsigned col = (unsigned)(pixel % asset->width) + left;
            if (row < HEIGHT && pixel < asset->width * asset->height)
                frame[row * WIDTH + col] = asset->runs[i + 1];
        }
    }
}

static uint16_t background(void) { return rgb565(8, 11, 17); }
static uint16_t bar_track(void) { return rgb565(31, 41, 59); }
static uint16_t bar_fill(void) { return rgb565(22, 119, 255); }

static void draw_bar(uint16_t *frame, unsigned left, unsigned top, unsigned width, uint16_t color)
{
    for (unsigned y = 0; y < BAR_HEIGHT; y++) {
        unsigned inset = (y == 0 || y == BAR_HEIGHT - 1) ? 2 : 0;
        for (unsigned x = inset; x + inset < width; x++)
            frame[(top + y) * WIDTH + left + x] = color;
    }
}

/* Pixel x of boot bar row y when `fill` pixels are filled; ends are rounded. */
static uint16_t boot_bar_pixel(unsigned x, unsigned y, unsigned fill)
{
    unsigned inset = (y == 0 || y == BAR_HEIGHT - 1) ? 2 : 0;
    if (x >= inset && x + inset < fill)
        return bar_fill();
    if (x >= inset && x + inset < BOOT_BAR_WIDTH)
        return bar_track();
    return background();
}

static unsigned percent_width(unsigned percent)
{
    return BOOT_BAR_WIDTH * percent / 100;
}

static void draw_boot_bar(uint16_t *frame, unsigned percent)
{
    for (unsigned y = 0; y < BAR_HEIGHT; y++)
        for (unsigned x = 0; x < BOOT_BAR_WIDTH; x++)
            frame[(BOOT_BAR_Y + y) * WIDTH + BOOT_BAR_X + x] =
                boot_bar_pixel(x, y, percent_width(percent));
}

/* `row` is the bar's middle row, indexed by screen x. Only a boot screen drawn
 * by this program carries the bar. */
static int boot_bar_row_valid(const uint16_t *row)
{
    uint16_t first = row[BOOT_BAR_X], last = row[BOOT_BAR_X + BOOT_BAR_WIDTH - 1];
    return row[BOOT_BAR_X - 1] == background() &&
        row[BOOT_BAR_X + BOOT_BAR_WIDTH] == background() &&
        (first == bar_track() || first == bar_fill()) &&
        (last == bar_track() || last == bar_fill());
}

static unsigned boot_bar_row_fill(const uint16_t *row)
{
    unsigned fill = 0;
    while (fill < BOOT_BAR_WIDTH && row[BOOT_BAR_X + fill] == bar_fill())
        fill++;
    return fill;
}

static int has_boot_bar(const uint16_t *frame)
{
    return boot_bar_row_valid(frame + (BOOT_BAR_Y + 1) * WIDTH);
}

/*
 * Named checkpoints draw the bar: firstboot (storage), the root system's units
 * (early, sysinit, basic) and the screen service when it opens the panel
 * (screen); its first frame replaces the bar. Percentages follow a normal Pro
 * Desk boot. Between checkpoints `animate` moves steadily toward the next one
 * over its usual duration, then slows and never reaches it.
 */
static const struct {
    const char *name;
    unsigned char percent;
    unsigned short next_ms;
} boot_stages[] = {
    { "storage", 10, 4400 }, { "early", 37, 3100 }, { "sysinit", 56, 1500 },
    { "basic", 65, 2100 }, { "screen", 78, 4000 }, { "done", 100, 0 },
};
enum { BOOT_STAGES = sizeof(boot_stages) / sizeof(boot_stages[0]) };

/* Index of the checkpoint the bar has reached, or -1 before the first. */
static int boot_stage(unsigned fill)
{
    int stage = -1;
    for (int i = 0; i < BOOT_STAGES; i++)
        if (fill >= percent_width(boot_stages[i].percent))
            stage = i;
    return stage;
}

/* Fill width `elapsed_ms` after reaching checkpoint `stage`: 90% of the way at
 * the usual time, then ever slower, never past 98%. */
static unsigned boot_stage_target(int stage, unsigned long elapsed_ms)
{
    unsigned base = percent_width(boot_stages[stage].percent);
    unsigned span = percent_width(boot_stages[stage + 1].percent) - base;
    unsigned long long usual = boot_stages[stage].next_ms, permille;

    if (elapsed_ms <= usual) {
        permille = 900 * elapsed_ms / usual;
    } else {
        unsigned long long late = elapsed_ms - usual;
        permille = 900 + 80 * late / (late + usual);
    }
    return base + (unsigned)(span * permille / 1000);
}

static void draw_log_message(uint16_t *frame, const char *message)
{
    enum { LEFT = 12, TOP = 6, COLUMNS = (WIDTH - 2 * LEFT) / LOG_FONT_WIDTH, ROWS = 3 };
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

/* Storage stages advance the install bar; it is not a time estimate. */
static void render(uint16_t *frame, unsigned stage, int failed, unsigned progress,
                   const struct splash_labels *labels, const char *message)
{
    for (unsigned i = 0; i < WIDTH * HEIGHT; i++)
        frame[i] = background();
    if (failed) {
        unsigned shift = message && *message ? 28 : 0;
        if (shift)
            draw_log_message(frame, message);
        draw_asset(frame, &logo_install, 23 + shift);
        draw_asset(frame, labels->paused, 86 + shift);
        draw_asset(frame, failed > 1 ? labels->saved_detail : labels->error_detail, 114 + shift);
    } else if (stage) {
        draw_asset(frame, &logo_install, 23);
        draw_asset(frame, labels->installing, 84);
        draw_asset(frame, labels->stages[stage - 1], 108);
        draw_bar(frame, INSTALL_BAR_X, INSTALL_BAR_Y, INSTALL_BAR_WIDTH, bar_track());
        draw_bar(frame, INSTALL_BAR_X, INSTALL_BAR_Y, INSTALL_BAR_WIDTH * stage / 7, bar_fill());
        draw_asset(frame, labels->warning, 148);
    } else {
        draw_asset(frame, &logo_boot, 48);
        draw_asset(frame, labels->starting, 125);
        draw_boot_bar(frame, progress);
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
         !strncmp(fix->id, "fb_jd9853", sizeof(fix->id)))) &&
        fix->type == FB_TYPE_PACKED_PIXELS && fix->visual == FB_VISUAL_TRUECOLOR &&
        fix->line_length >= PANEL_WIDTH * 2 && fix->line_length <= 4096 &&
        fix->smem_len >= fix->line_length * PANEL_HEIGHT;
}

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

static void unrotate_panel(uint16_t *frame, const uint8_t *pixels, size_t stride)
{
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            size_t offset = (PANEL_HEIGHT - 1 - x) * stride + y * 2;
            frame[y * WIDTH + x] = (uint16_t)(pixels[offset] | pixels[offset + 1] << 8);
        }
    }
}

/* Bar columns map to a contiguous band of native panel rows. */
static size_t boot_bar_band(size_t stride, size_t *size)
{
    *size = (size_t)BOOT_BAR_WIDTH * stride;
    return (size_t)(PANEL_HEIGHT - BOOT_BAR_X - BOOT_BAR_WIDTH) * stride;
}

/* Returns 0 when the panel shows no boot bar or the bar is already further. */
static int update_boot_bar(uint8_t *pixels, size_t stride, uint16_t *frame, unsigned progress)
{
    unrotate_panel(frame, pixels, stride);
    if (!has_boot_bar(frame) ||
        percent_width(progress) <= boot_bar_row_fill(frame + (BOOT_BAR_Y + 1) * WIDTH))
        return 0;
    draw_boot_bar(frame, progress);
    rotate_panel(pixels, stride, frame);
    return 1;
}

/* Native panel rows holding the bar plus one background pixel on each side. */
static size_t bar_edge_band(size_t stride, size_t *size)
{
    *size = (size_t)(BOOT_BAR_WIDTH + 2) * stride;
    return (size_t)(PANEL_HEIGHT - BOOT_BAR_X - BOOT_BAR_WIDTH - 1) * stride;
}

/* Rebuild the bar's middle row (screen x) from that band. */
static void band_bar_row(uint16_t *row, const uint8_t *band, size_t stride)
{
    for (unsigned x = BOOT_BAR_X - 1; x <= BOOT_BAR_X + BOOT_BAR_WIDTH; x++) {
        const uint8_t *pixel = band + (size_t)(BOOT_BAR_X + BOOT_BAR_WIDTH - x) * stride +
            (BOOT_BAR_Y + 1) * 2;
        row[x] = (uint16_t)(pixel[0] | pixel[1] << 8);
    }
}

/* One bar column is four adjacent pixels in one native panel row. */
static void bar_column(uint8_t out[BAR_HEIGHT * 2], unsigned x, unsigned fill)
{
    for (unsigned y = 0; y < BAR_HEIGHT; y++) {
        uint16_t color = boot_bar_pixel(x, y, fill);
        out[y * 2] = (uint8_t)color;
        out[y * 2 + 1] = (uint8_t)(color >> 8);
    }
}

static size_t bar_column_offset(unsigned x, size_t stride)
{
    return (size_t)(PANEL_HEIGHT - 1 - BOOT_BAR_X - x) * stride + BOOT_BAR_Y * 2;
}

static int open_panel(const char *path, struct fb_fix_screeninfo *fix)
{
    struct fb_var_screeninfo var;
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, fix) < 0)
        goto failed;
    if (!valid_panel(&var, fix)) {
        errno = ENOTSUP;
        goto failed;
    }
    return fd;
failed:
    {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return -1;
}

/* Write a whole frame; the panel driver resends the full panel afterwards. */
static int write_panel(int fd, const uint8_t *pixels, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, pixels + done, size - done, (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/*
 * Bar updates go through a shared mapping: the panel driver then sends only the
 * touched pages, while write() always resends the whole panel over SPI.
 */
static uint8_t *map_panel(int fd, size_t stride)
{
    void *map = mmap(NULL, stride * PANEL_HEIGHT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return map == MAP_FAILED ? NULL : map;
}

/* Copy the rows of [offset, offset + size) that differ from the panel. */
static void copy_changed_rows(uint8_t *panel, const uint8_t *pixels, size_t offset, size_t size,
                              size_t stride)
{
    for (size_t at = offset; at < offset + size; at += stride)
        if (memcmp(panel + at, pixels + at, stride))
            memcpy(panel + at, pixels + at, stride);
}

/* Bar writers serialize their read-modify-write cycles on the framebuffer. */
static int lock_panel(int fd)
{
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    while (fcntl(fd, F_SETLKW, &lock) < 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

/* progress < 0 draws the whole frame; otherwise only an existing boot bar changes. */
static int show_panel(const char *path, uint16_t *frame, int progress)
{
    struct fb_fix_screeninfo fix;
    uint8_t *pixels;
    size_t size;
    int result = -1;
    int fd = open_panel(path, &fix);

    if (fd < 0)
        return -1;
    size = (size_t)fix.line_length * PANEL_HEIGHT;
    pixels = calloc(1, size);
    if (!pixels)
        goto close_panel;
    if (progress < 0) {
        rotate_panel(pixels, fix.line_length, frame);
        /* The lock keeps the animator from painting over this frame. */
        if (lock_panel(fd) < 0 || write_panel(fd, pixels, size) < 0)
            goto free_pixels;
        /* Leave the last frame visible; the normal screen service takes over. */
        (void)ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
    } else {
        size_t band, first = boot_bar_band(fix.line_length, &band);
        uint8_t *panel = map_panel(fd, fix.line_length);
        if (!panel)
            goto free_pixels;
        if (lock_panel(fd) < 0) {
            munmap(panel, size);
            goto free_pixels;
        }
        memcpy(pixels, panel, size);
        if (update_boot_bar(pixels, fix.line_length, frame, (unsigned)progress))
            copy_changed_rows(panel, pixels, first, band, fix.line_length);
        munmap(panel, size);
    }
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

static volatile sig_atomic_t stop_animation;

static void request_stop(int signal)
{
    (void)signal;
    stop_animation = 1;
}

static unsigned long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long)now.tv_sec * 1000 + (unsigned long)now.tv_nsec / 1000000;
}

/* Paint columns [from, fill) of a bar `fill` pixels wide on the native panel. */
static void paint_bar(uint8_t *panel, size_t stride, unsigned from, unsigned fill)
{
    for (unsigned x = from; x < fill; x++)
        bar_column(panel + bar_column_offset(x, stride), x, fill);
}

/* Keep lengthening the bar on `fd` (a native panel with `stride`) until told to stop. */
static int animate_fd(int fd, size_t stride, unsigned long lifetime_ms)
{
    enum { TICK_MS = 80 };
    uint16_t row[WIDTH];
    size_t band_size, band_offset = bar_edge_band(stride, &band_size);
    uint8_t *panel = map_panel(fd, stride);
    unsigned long started = monotonic_ms(), stage_started = started;
    int stage = -2;

    if (!panel)
        return 1;
    while (!stop_animation && monotonic_ms() - started < lifetime_ms) {
        struct timespec tick = { 0, TICK_MS * 1000000L };
        struct flock unlock = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
        unsigned fill, target;
        int current;

        if (lock_panel(fd) < 0)
            break;
        band_bar_row(row, panel + band_offset, stride);
        if (!boot_bar_row_valid(row))
            break;
        fill = boot_bar_row_fill(row);
        current = boot_stage(fill);
        if (current != stage) {
            stage = current;
            stage_started = monotonic_ms();
        }
        if (stage >= BOOT_STAGES - 1)
            break;
        target = stage < 0 ? fill : boot_stage_target(stage, monotonic_ms() - stage_started);
        /* Repaint the old rounded end too, then the newly filled columns. */
        if (target > fill)
            paint_bar(panel, stride, fill > 2 ? fill - 2 : 0, target);
        (void)fcntl(fd, F_SETLK, &unlock);
        nanosleep(&tick, NULL);
    }
    munmap(panel, stride * PANEL_HEIGHT);
    return 0;
}

/*
 * Started by the initramfs once storage is ready; it survives switch_root and
 * keeps the bar moving until `finish`, until the bar disappears, or for at most
 * two minutes. It only ever lengthens the fill.
 */
static int animate(const char *device)
{
    struct fb_fix_screeninfo fix;
    struct sigaction action = { .sa_handler = request_stop };
    int fd, result;

    (void)setsid();
    signal(SIGHUP, SIG_IGN);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    fd = open_panel(device, &fix);
    if (fd < 0)
        return 1;
    result = animate_fd(fd, fix.line_length, 120000);
    close(fd);
    return result;
}

static int animation_process(pid_t pid)
{
    char path[64], args[256];
    ssize_t length;
    int fd;
    const char *name;

    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    length = read(fd, args, sizeof(args) - 1);
    close(fd);
    if (length <= 0)
        return 0;
    args[length] = '\0';
    name = strrchr(args, '/') ? strrchr(args, '/') + 1 : args;
    if (strcmp(name, "onekvm-boot-splash"))
        return 0;
    for (ssize_t i = (ssize_t)strlen(args) + 1; i < length; i += (ssize_t)strlen(args + i) + 1)
        if (!strcmp(args + i, "animate"))
            return 1;
    return 0;
}

static int process_running(pid_t pid)
{
    char path[64], stat[128];
    ssize_t length;
    const char *state;
    int fd;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    length = read(fd, stat, sizeof(stat) - 1);
    close(fd);
    if (length <= 0)
        return 0;
    stat[length] = '\0';
    state = strrchr(stat, ')');
    return state && state[1] == ' ' && state[2] != 'Z' && state[2] != 'X';
}

/* Stop `animate` before the caller draws its own first frame. */
static int finish(void)
{
    enum { MAX = 8 };
    pid_t pids[MAX];
    unsigned count = 0;
    struct dirent *entry;
    DIR *proc = opendir("/proc");

    if (!proc)
        return 1;
    while ((entry = readdir(proc)) && count < MAX) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid == (long)getpid() || !animation_process((pid_t)pid))
            continue;
        if (!kill((pid_t)pid, SIGTERM))
            pids[count++] = (pid_t)pid;
    }
    closedir(proc);
    /* Wait up to half a second so no bar write can follow the caller's frame. */
    for (unsigned tries = 0; count && tries < 50; tries++) {
        struct timespec pause = { 0, 10 * 1000000L };
        unsigned running = 0;
        for (unsigned i = 0; i < count; i++)
            if (process_running(pids[i]))
                pids[running++] = pids[i];
        count = running;
        if (count)
            nanosleep(&pause, NULL);
    }
    return count ? 1 : 0;
}

/* A percentage, or the name of a boot checkpoint. */
static int parse_percent(const char *text)
{
    int value = 0;
    for (int i = 0; i < BOOT_STAGES; i++)
        if (!strcmp(text, boot_stages[i].name))
            return boot_stages[i].percent;
    if (!*text || strlen(text) > 3)
        return -1;
    for (; *text; text++) {
        if (*text < '0' || *text > '9')
            return -1;
        value = value * 10 + (*text - '0');
    }
    return value <= 100 ? value : -1;
}

int main(int argc, char **argv)
{
    uint16_t frame[WIDTH * HEIGHT];
    const char *preview = NULL, *device = "/dev/fb0", *message = NULL;
    unsigned stage = 0;
    int failed = 0, progress = 0, bar_only = 0;
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
        else if (!strcmp(argv[1], "--progress") && (progress = parse_percent(argv[2])) >= 0)
            ;
        else
            break;
        argc -= 2;
        argv += 2;
    }
    if (argc == 2 && !strcmp(argv[1], "animate") && !preview)
        return animate(device);
    if (argc == 2 && !strcmp(argv[1], "finish"))
        return finish();
    if (argc == 2 && !strcmp(argv[1], "boot")) {
        stage = 0;
    } else if (argc == 2 && !strcmp(argv[1], "error")) {
        failed = 1;
    } else if (argc == 2 && !strcmp(argv[1], "error-saved")) {
        failed = 2;
    } else if (argc == 3 && !strcmp(argv[1], "progress") && !preview &&
               (progress = parse_percent(argv[2])) >= 0) {
        bar_only = 1;
    } else if (argc == 3 && !strcmp(argv[1], "install") &&
               strlen(argv[2]) == 1 && argv[2][0] >= '1' && argv[2][0] <= '7') {
        stage = (unsigned)(argv[2][0] - '0');
    } else {
        fprintf(stderr, "Usage: onekvm-boot-splash [--language en|zh|zh_tw] [--message TEXT] [--progress PERCENT] "
                "[--output FILE.ppm | --framebuffer /dev/fb0] {boot | install STAGE(1-7) | error | error-saved}\n"
                "       onekvm-boot-splash [--framebuffer /dev/fb0] {progress PERCENT|CHECKPOINT | animate}\n"
                "       onekvm-boot-splash finish\n");
        return 2;
    }
    if (!bar_only)
        render(frame, stage, failed, (unsigned)progress, labels, message);
    if ((preview ? write_preview(preview, frame) : show_panel(device, frame, bar_only ? progress : -1)) < 0) {
        perror("onekvm-boot-splash");
        return 1;
    }
    return 0;
}
