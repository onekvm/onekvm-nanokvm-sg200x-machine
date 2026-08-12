# onekvm-machine-nanokvm

English | [简体中文](README.zh-CN.md)

Machine identity, board-variant detection, and fixed hardware profiles for
running OneKVM on NanoKVM devices.

The repository provides:

- a small I2C probe and board-detection script;
- Alpha, Beta, PCIe, and Lite GPIO ATX profiles;
- the OneKVM machine capability description;
- systemd, udev, and module-loading integration files.

## Build

Build the native I2C probe with:

```sh
make
```

The complete target package is built by the `onekvm-machine-nanokvm` recipe in
the `meta-onekvm` layer. Yocto packaging remains in `onekvm-distro` because it
contains distribution-specific dependencies, paths, and upgrade handling.

## License

GPL-2.0-only. See [LICENSE](LICENSE).
