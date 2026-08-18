#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s /dev/i2c-N address\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    errno = 0;
    long address = strtol(argv[2], &end, 0);
    if (errno != 0 || end == argv[2] || *end != '\0' || address < 0x03 || address > 0x77) {
        fprintf(stderr, "invalid I2C address: %s\n", argv[2]);
        return 2;
    }

    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 1;
    if (ioctl(fd, I2C_SLAVE, address) < 0) {
        close(fd);
        return 1;
    }

    /* Do not send 0xAE. C906L already owns the panel; turning it off here
       blanks the BOOTING canvas. Contrast is a harmless ACK probe. */
    const unsigned char contrast[] = {0x00, 0x81, 0xcf};
    ssize_t written = write(fd, contrast, sizeof(contrast));
    close(fd);
    return written == (ssize_t)sizeof(contrast) ? 0 : 1;
}
