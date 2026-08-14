# onekvm-nanokvm-machine

[English](README.md) | 简体中文

用于在 NanoKVM 设备上运行 OneKVM 的机器标识、板型检测和固定硬件配置。

本仓库包含：

- I2C 探针和板型检测脚本；
- Alpha、Beta、PCIe 和 Lite 的 GPIO ATX 配置；
- OneKVM 机器能力描述；
- systemd、udev 和内核模块加载配置。
- 首次启动时迁移为 NanoKVM A/B rootfs 和 userdata 分区布局；
- initramfs 早期 OLED 状态模块；
- NanoKVM 存储创建和挂载服务；
- RAUC slot 健康确认和失败处理。

开机验证完成后，RAUC 槽位状态会缓存在 `/run/onekvm`，RAUC D-Bus 服务随即
停止。只读升级状态查询使用缓存；执行升级或切换槽位时会使缓存失效，并按需
启动 RAUC。

这些生命周期组件依赖 NanoKVM SD 分区布局、启动标记、GPIO/OLED 拓扑和机器
服务，因此和机器支持代码放在同一个仓库中维护。

## 编译

编译本机 I2C 探针：

```sh
make
```

完整目标软件包由 `meta-onekvm` layer 中的 `onekvm-machine-nanokvm` 配方构建。
Yocto 配方仍保留在 `onekvm-distro`，因为其中包含发行版专用依赖、安装路径和
升级处理逻辑。配方继续使用现有软件包名，以保持升级兼容性。

## 许可证

用户空间生命周期文件使用 MIT 许可证；机器检测程序和 early OLED 内核模块
使用 GPL-2.0-only。详见 [LICENSE](LICENSE) 和
[LICENSES/MIT.txt](LICENSES/MIT.txt)。
