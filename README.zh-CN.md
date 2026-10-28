# onekvm-nanokvm-machine

[English](README.md) | 简体中文

用于在 NanoKVM 设备上运行 OneKVM 的机器标识、板型检测和固定硬件配置。

本仓库包含：

- I2C 探针和板型检测脚本；
- Alpha、Beta、PCIe 和 Lite 的 GPIO ATX 配置；
- OneKVM 机器能力描述；
- NanoKVM 只读 EDID 预设；
- systemd、udev 和内核模块加载配置。
- 首次启动时迁移为 NanoKVM A/B rootfs 和 userdata 分区布局；
- initramfs 早期 OLED 状态模块；
- Pro Desk 的启动 Logo 和系统安装进度画面；
- NanoKVM 存储创建和挂载服务；
- RAUC slot 健康确认和失败处理。

开机验证完成后，RAUC 槽位状态会缓存在 `/run/onekvm`，RAUC D-Bus 服务随即
停止。只读升级状态查询使用缓存；执行升级或切换槽位时会使缓存失效，并按需
启动 RAUC。

正常启动及槽位验证完成后，`onekvm-boot-success.timer` 开始计时；两分钟后确认
OneKVM 仍在运行，再删除 `/boot/boot_retry_time.txt`。initramfs 计数及第十次
启动进入 Recovery 的逻辑由 `onekvm-nanokvm-recovery` 维护。

这些生命周期组件依赖 NanoKVM SD 分区布局、启动标记、GPIO/OLED 拓扑和机器
服务，因此和机器支持代码放在同一个仓库中维护。

## 编译

编译本机 I2C 探针：

```sh
make
```

完整目标软件包由 `meta-onekvm-bsp` layer 中的 `onekvm-machine-nanokvm` 配方构建。
Yocto 配方仍保留在 `onekvm-distro`，因为其中包含发行版专用依赖、安装路径和
升级处理逻辑。配方继续使用现有软件包名，以保持升级兼容性。

## 自动码率上限

机型配置的 `video_auto_bitrate_max_kbps` 定义自动自适应码率上限：Cube 为
10000 kbit/s（10 Mbps），Pro 的机型检测配置为 20000 kbit/s（20 Mbps）。
Core 取此值与视频后端上限的较小值；手动码率仍使用后端支持范围。

## EDID 预设

机型包将对应机型的 EDID 预设安装到 `/usr/share/onekvm/edid/presets/`。
Core 显示设置和 EDID 编辑器读取同一个目录，未安装编辑器时预设仍可使用。
恢复出厂 EDID 使用本机型的 `factory.edid.bin`，其他机型包可在同一路径提供
适合自身硬件的文件。

显示设置和编辑器按首选模式及标称刷新率，将已知预设统一显示为
`宽×高 @ 刷新率 Hz`，`factory` 显示为“出厂预设”。
显示名称随界面语言切换，预设 ID、文件名、EDID 内容和自定义文件名保持不变。

SG200x NanoKVM 预设：

- `factory.edid.bin` — 默认 1920×1080 @ 60 Hz
- `1080p60.edid.bin` — 1920×1080 @ 60 Hz
- `720p90.edid.bin` — 1280×720 @ 90 Hz
- `1440p30.edid.bin` — 2560×1440 @ 30 Hz
- `1620p30.edid.bin` — 2880×1620 @ 30 Hz（SG2002 VPSS 最大原生输出）

编辑器将修改副本保存在自己的数据目录，不能覆盖机型预设。

NanoKVM 与 Pro 还提供从 480p 到 1080p 的常用 60 Hz 档位，界面按
1:1、5:4、4:3、3:2、16:10、16:9、17:9、21:9、32:9、9:16 分组。
Pro 的竖屏最高为 1080×1920。新增预设 ID 使用 `宽x高p60`，已有 ID 保持不变。
显示设置、快捷面板和 EDID 编辑器支持按分辨率、比例、刷新率搜索。
“常用”组通过别名指向原预设：480p/VGA/FWVGA、720p/HD、1080p/FHD、
2K/QHD/1440p（2560×1440）、4K/UHD/2160p（3840×2160）。
支持搜索别名及 `4K30`、`2K 60 Hz` 等简写。
SG200x 预设限制为 2880×1620、165 MHz，Pro 为 3840×2160、297 MHz。
17:9、21:9 包含行业标称比例，例如 2048×1080、2560×1080。
已校验 EDID 校验和、时序及上限；非常规模式仍取决于源显卡、环出显示器与采集固件，
预设存在不代表所有模式均已完成实机采集验证。

Pro 机型包从 `share/edid/pro/` 安装的预设还包括：

- `factory.edid.bin` — 默认 1920×1080 @ 60 Hz，与 `1080p60.edid.bin` 相同
- `2160p30.edid.bin` — 首选 3840×2160 @ 30 Hz
- `1080p60.edid.bin` — 1920×1080 @ 60 Hz
- `1440p30.edid.bin` — 2560×1440 @ 30 Hz
- `1440p60.edid.bin` — 2560×1440 @ 59.95 Hz（标称 60 Hz）
- `720p60.edid.bin` — 1280×720 @ 60 Hz
- `1080p120.edid.bin` — 首选 1920×1080 @ 120 Hz，保留 1080p60 / 720p60 回退模式

独立分辨率预设声明对应首选时序及带宽更低的回退模式，并保留 BIOS 基础时序和双声道
LPCM 音频。Pro 采集最高支持 4K30，需要选择独立的 2160p30 预设；
默认 1080p60 预设不声明 4K 模式。
独立的 1080p120 预设用于高刷新率输入；编码仍服从目标帧率，Pro 已验证
120 Hz 输入、60 FPS 串流。连接环出显示器时，它也需要支持该输入模式。
HDMI 一分二向主机
提供所选采集端 EDID 与环出显示器共有的模式；环出显示器也支持 4K30 时，
主机才能选择该模式。仅停止 SoC 取帧不会关闭采集桥片，也不会将采集端
EDID 从协商中移除。使用
`python3 tools/generate-pro-edid.py` 可重新生成；独立目录避免将 Pro 模式装入
SG2002 的机型包。

Pro 机型包将普通 1080p60 预设安装为出厂 EDID。
共用的 `onekvm-capture-chip-init.service` 在使用新 OneKVM 数据的首次启动时、
Core 启动前应用默认值。它先将桥片原 EDID 备份到
`/var/lib/onekvm/edid/before-pro-default.bin`，回读校验通过后在同目录创建
`pro-default-initialized` 标记。后续启动和软件包升级保留用户的 EDID；
清除 OneKVM 数据也会清除标记，下次启动重新恢复首选 1080p60 的默认值。
此过程使用桥片驱动的 EDID 操作，不替换固件代码。

初始化直接单次读取 Flash 的 256 字节，不使用 `edid_snapshot`，也不在脚本中
操作 `power` 接口。读取失败或数据不完整时停止初始化，不写 Flash 或成功标记。
写入仍由驱动校验 Flash 回读。

Pro 与 Cube/PCIe 共用采集芯片初始化服务。它在 MMF 模块和 Core 启动前按文件名
顺序执行 `/usr/libexec/onekvm/capture-chip-init.d/` 下的可执行 hook：固件升级
使用 `10-` 前缀，默认 EDID 使用 `20-`。任一 hook 失败会停止初始化并阻止采集启动。

固件 hook 每次初始化都应检查实际芯片；EDID 标记不能阻止后续固件升级，固件已
匹配时跳过写入。升级校验后，Pro 和 PCIe 使用各自的桥片复位机制。Cube 没有
可控的桥片硬件复位线：EDID API 返回 `REBOOT`，固件升级也需标记为待整机重启，
重启后再确认生效。普通重启能否重新加载 Cube MCU、是否必须断电重启，仍需
Cube 固件升级实机验收。Cube 的差异升级由独立的 `onekvm-capture-firmware`
包提供；它使用 C 和 shell，按连续编号及源／目标程序哈希执行受审计增量。
该包不附带未验收的 Cube 优化候选。Pro 固件差异升级仍未实现。

## 早期 OLED

最早期的 Alpha Cube 使用 GPIOA27 作为 ATX 重置输出，OLED 则使用 SD1 引脚上的
硬件 I2C1。通用设备树还定义了 SDA 为 GPIOA27 的 GPIO-I2C OLED 总线；探测这条
未使用的总线可能驱动 ATX 重置线。内核必须在 Alpha 探测前禁用该 GPIO-I2C 节点，
并禁用与硬件 I2C1 引脚冲突的 Wi-Fi SDIO 节点。硬件 I2C1 控制器由 C906L 独占，
Linux 不应绑定该控制器，并须保留共享 I2C 功能时钟引用；否则 Linux 可能关闭时钟，
使开机动画冻结且 TX FIFO 留有未发送字节。PCIe 和后续 GPIO OLED 版本保持原有设备
与时钟配置。Alpha ATX 配置提供电源键、重置键、PWR LED 和 HDD LED。

initramfs 的安装与恢复画面优先交给 C906L，发送命令后必须在 1 秒内收到新的序号 ACK。
暖重启可能保留上次 OneKVM 的共享内存标记；原厂 FIP 未启动 C906L 时，该标记不能
证明显示服务可用。没有有效标记或新 ACK 时，早期模块探测 Cube / PCIe OLED 并直接绘制。
普通系统的 OLED 服务仍通过 C906L 工作，不使用这条 initramfs 硬件探测路径。

Pro Desk 使用单次绘制的 RGB565 [启动画面](boot-splash/README.md)：在存储初始化
之前显示 OneKVM Logo，新镜像首启时显示英文安装阶段，随后保留启动画面直到常规
屏幕服务接管。显示失败不会阻止安装或启动。

## 共用存储

`storage/storage-layout.sh`、`storage/build-image.py` 和 firstboot hook 由
Cube/PCIe、Pro SD、Pro eMMC 共用。profile 定义几何，root 参数选择介质和槽位，
设备不明确时拒绝写入。SD 安装镜像首启展开；Pro eMMC 使用 cmdline 分区，保留
厂商启动链前缀。三者均使用每槽 128 MiB 的 squashfs A/B、共享 upper/work；新 userdata 为 f2fs，
已有 ext4/f2fs 保留。Cube/PCIe、Pro SD 与 Pro eMMC 安装后统一使用 128 MiB boot FAT；
SD 发布载体的 FAT 按引导文件加少量余量生成，不填满最终分区容量。首启识别 `bootgrow`
标记，将 rootfs 和全部引导文件（含 firstboot 配置）备份到 RAM 并校验，再建立最终分区；
Cube/PCIe 载体另用内容为 `A` 的 `bootclone` 标记，只携带 `boot.sd`。首启在修改分区
之前，于受保护的 RAM 备份中复制并校验独立的 `boot-b.itb`，保留相同的 A/B build ID；
还原安装后的 FAT 并校验完成后删除两个安装标记。
eMMC 仍从前置 raw A/B 区加载启动载荷。旧版 Cube 的 32 MiB loader 仍可作为升级目标。
OE 配方仍在 `onekvm-distro`，Pro 启动限制见
`onekvm-distro/docs/nanokvm-pro-ab-recovery.md`。
生成的镜像尾部带 512 字节 profile 和 SHA-256 trailer；刷机程序必须先校验它，
再选择 SD 或 eMMC 写入路径。

## 许可证

用户空间生命周期文件使用 MIT 许可证；机器检测程序和 early OLED 内核模块
使用 GPL-2.0-only。详见 [LICENSE](LICENSE) 和
[LICENSES/MIT.txt](LICENSES/MIT.txt)。
