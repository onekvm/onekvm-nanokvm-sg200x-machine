# onekvm-machine-nanokvm

[English](README.md) | 简体中文

用于在 NanoKVM 设备上运行 OneKVM 的机器标识、板型检测和固定硬件配置。

本仓库包含：

- I2C 探针和板型检测脚本；
- Alpha、Beta、PCIe 和 Lite 的 GPIO ATX 配置；
- OneKVM 机器能力描述；
- systemd、udev 和内核模块加载配置。

## 编译

编译本机 I2C 探针：

```sh
make
```

完整目标软件包由 `meta-onekvm` layer 中的 `onekvm-machine-nanokvm` 配方构建。
Yocto 配方仍保留在 `onekvm-distro`，因为其中包含发行版专用依赖、安装路径和
升级处理逻辑。

## 许可证

GPL-2.0-only，详见 [LICENSE](LICENSE)。
