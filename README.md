# onekvm-nanokvm-machine

English | [简体中文](README.zh-CN.md)

Machine identity, board-variant detection, and fixed hardware profiles for
running OneKVM on NanoKVM devices.

The repository provides:

- a small I2C probe and board-detection script;
- Alpha, Beta, PCIe, and Lite GPIO ATX profiles;
- the OneKVM machine capability description;
- systemd, udev, and module-loading integration files.
- first-boot migration to the NanoKVM A/B rootfs and userdata layout;
- an early initramfs OLED status module;
- NanoKVM storage provisioning and mount services;
- RAUC slot health confirmation and failure handling.

These lifecycle components live here because they depend on the NanoKVM SD
partition layout, boot markers, GPIO/OLED topology, and machine services.

## Build

Build the native I2C probe with:

```sh
make
```

The complete target package is built by the `onekvm-machine-nanokvm` recipe in
the `meta-onekvm` layer. Yocto packaging remains in `onekvm-distro` because it
contains distribution-specific dependencies, paths, and upgrade handling.
The recipe keeps its existing package name for upgrade compatibility.

## License

Userspace lifecycle files are MIT licensed. The machine detector and early
OLED kernel module are GPL-2.0-only. See [LICENSE](LICENSE) and
[LICENSES/MIT.txt](LICENSES/MIT.txt).
