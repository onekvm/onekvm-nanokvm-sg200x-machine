// SPDX-License-Identifier: MIT
/* Small initramfs feeder, bounded boot grace and explicit hardware reset. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t move_root, handoff;
static void signal_handler(int sig)
{
    if (sig == SIGUSR1) move_root = 1;
    if (sig == SIGUSR2) handoff = 1;
}

int main(int argc, char **argv)
{
    int reset = argc == 2 && !strcmp(argv[1], "--reset");
    if (argc != 2 || (!reset && strcmp(argv[1], "--boot"))) {
        fprintf(stderr, "usage: %s --boot|--reset\n", argv[0]);
        return 2;
    }
    int fd = open("/dev/watchdog0", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { perror("watchdog open"); return 1; }
    int seconds = reset ? 5 : 60;
    if (ioctl(fd, WDIOC_SETTIMEOUT, &seconds) ||
        ioctl(fd, WDIOC_KEEPALIVE, 0)) {
        perror("watchdog arm");
        return 1;
    }
    fprintf(stderr, "OneKVM Go: hardware watchdog armed (%d s)\n", seconds);
    if (reset) {
        /* Keep the descriptor open and never feed it again. No reboot syscall. */
        /* Flush writable SD/NFS data after arming; a hung sync is bounded by WDT. */
        sync();
        for (;;) pause();
    }
    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    FILE *pid = fopen("/run/onekvm/boot-watchdog.pid", "w");
    if (!pid) { perror("watchdog pid"); return 1; }
    fprintf(pid, "%ld\n", (long)getpid());
    fclose(pid);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (move_root) {
            move_root = 0;
            if (chdir("/newroot") || chroot(".") || chdir("/")) {
                perror("watchdog chroot"); return 1;
            }
            int ready = open("/run/onekvm/boot-watchdog.ready", O_WRONLY | O_CREAT, 0600);
            if (ready < 0) { perror("watchdog ready"); return 1; }
            close(ready);
        }
        if (handoff) {
            /* No 'V': keep hardware armed while the runtime owner opens it. */
            close(fd);
            unlink("/run/onekvm/boot-watchdog.pid");
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 240) {
            fprintf(stderr, "OneKVM Go: boot grace expired; hardware reset pending\n");
            close(fd);
            return 1;
        }
        if (ioctl(fd, WDIOC_KEEPALIVE, 0)) { perror("watchdog feed"); return 1; }
        sleep(2);
    }
}
