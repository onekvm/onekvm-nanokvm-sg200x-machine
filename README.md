# onekvm-nanokvm-machine

English | [简体中文](README.zh-CN.md)

Machine identity, board-variant detection, and fixed hardware profiles for
running OneKVM on NanoKVM devices.

The repository provides:

- a small I2C probe and board-detection script;
- Alpha, Beta, PCIe, and Lite GPIO ATX profiles;
- the OneKVM machine capability description;
- read-only NanoKVM EDID presets;
- systemd, udev, and module-loading integration files.
- first-boot migration to the NanoKVM A/B rootfs and userdata layout;
- an early initramfs OLED status module;
- the Pro Desk startup logo and installation progress splash;
- NanoKVM storage provisioning and mount services;
- RAUC slot health confirmation and failure handling.

After boot validation, the RAUC slot state is cached under `/run/onekvm` and
the RAUC D-Bus service is stopped. Read-only update status requests use the
cache; update and slot-switch operations invalidate it and activate RAUC on
demand.

The `onekvm-boot-success.timer` starts after normal startup and slot validation,
then clears `/boot/boot_retry_time.txt` two minutes later while OneKVM is active.
The initramfs retry counter and tenth-attempt Recovery entry belong to
`onekvm-nanokvm-recovery`.

These lifecycle components live here because they depend on the NanoKVM SD
partition layout, boot markers, GPIO/OLED topology, and machine services.

## Build

Build the native I2C probe with:

```sh
make
```

The complete target package is built by the `onekvm-machine-nanokvm` recipe in
the `meta-onekvm-bsp` layer. Yocto packaging remains in `onekvm-distro` because it
contains distribution-specific dependencies, paths, and upgrade handling.
The recipe keeps its existing package name for upgrade compatibility.

## EDID presets

The machine package installs its model-specific presets under
`/usr/share/onekvm/edid/presets/`. Core Display settings and the EDID editor
read this same directory. Presets remain available without the editor, and
factory reset uses this machine's `factory.edid.bin`. Other machine packages
can supply their own files at the same path.

Display settings and the editor label known presets as `width×height @ refresh Hz`
using the preferred mode and nominal refresh rate. `factory` is labelled
`Factory preset`. Labels are localized, while preset IDs,
file names, EDID bytes and custom file names remain unchanged.

SG200x NanoKVM presets:

- `factory.edid.bin` — default 1920×1080 @ 60 Hz
- `1080p60.edid.bin` — 1920×1080 @ 60 Hz
- `720p90.edid.bin` — 1280×720 @ 90 Hz
- `1440p30.edid.bin` — 2560×1440 @ 30 Hz
- `1620p30.edid.bin` — 2880×1620 @ 30 Hz (maximum native SG2002 VPSS output)

The editor saves modified copies in its own data directory; it cannot
overwrite the machine presets.

Both NanoKVM and Pro also provide common 60 Hz presets from 480p to 1080p,
grouped in the UI as 1:1, 5:4, 4:3, 3:2, 16:10, 16:9, 17:9, 21:9, 32:9 and 9:16.
The portrait set extends to 1080×1920 on Pro. New IDs use `WIDTHxHEIGHTpFPS`;
existing preset IDs stay stable. Resolution, aspect ratio and refresh rate can
be searched in Display settings, the quick panel and the EDID editor.
A Common group links to the same files by aliases: 480p/VGA/FWVGA, 720p/HD,
1080p/FHD, 2K/QHD/1440p (2560×1440), and 4K/UHD/2160p (3840×2160).
Aliases and compact queries such as `4K30` and `2K 60 Hz` are searchable.
Ordinary SG200x presets are limited to 2880×1620 and 165 MHz; Pro to 3840×2160 and 297 MHz.
Both sets also include `1920x1080p90.edid.bin`: preferred 1920×1080 @ 90 Hz,
222.75 MHz, with 1080p60 / 720p60 / VGA fallbacks. This high-refresh preset
exceeds the ordinary SG200x limit and targets LT6911UXC / Pro receivers;
LT6911C/D compatibility has not been verified. Factory defaults remain 1080p60.
17:9 and 21:9 include nominal DCI/ultrawide modes such as 2048×1080 and 2560×1080.
The HDMI TMDS clock ceiling covers every advertised timing, including VGA
fallbacks, rounded up in 5 MHz units. The preset regression checks this ceiling
as well as the 1080p60 factory default and the optional 1080p90 timing.
EDID checksums and timing bounds are checked, but unusual modes still depend
on the source GPU, loopout display and capture firmware; their presence does
not mean every mode has been verified on hardware.

Pro presets, installed from `share/edid/pro/`, also include:

- `factory.edid.bin` — default 1920×1080 @ 60 Hz, identical to `1080p60.edid.bin`
- `2160p30.edid.bin` — preferred 3840×2160 @ 30 Hz
- `1080p60.edid.bin` — 1920×1080 @ 60 Hz
- `1440p30.edid.bin` — 2560×1440 @ 30 Hz
- `1440p60.edid.bin` — 2560×1440 @ 59.95 Hz (nominal 60 Hz)
- `720p60.edid.bin` — 1280×720 @ 60 Hz
- `1080p120.edid.bin` — preferred 1920×1080 @ 120 Hz, with 1080p60 / 720p60 fallbacks

Dedicated resolution presets advertise their preferred timing and lower-bandwidth fallback modes,
including BIOS-safe established timings and stereo LPCM audio. Pro capture
supports up to 4K30; select the separate 2160p30 preset to advertise 4K30.
The default 1080p60 preset does not advertise 4K modes. The HDMI splitter presents
the host with modes shared by the selected capture EDID and the loopout display.
The separate 1080p120 preset supports high refresh input; encoding still follows
the configured stream frame rate (120 Hz input to 60 FPS output is verified on Pro).
The loopout display must also support 4K30 for that mode to be advertised.
Stopping SoC capture does not disable the capture bridge or remove its EDID
from this negotiation. Regenerate the Pro files with
`python3 tools/generate-pro-edid.py`; the separate directory keeps these modes
out of the SG2002 machine package.

The Pro machine package ships EDID presets, including 1080p60, for the Web
EDID settings. Boot never programs the bridge EDID: the bridge keeps its
existing EDID until the user changes it. The Pro bridge can be LT6911UXC,
LT6911D or LT6911C; the family driver detects the register ID rather than
assuming UXC from the board name. Pro's `20-pro-chip-info` hook only links
`/run/onekvm/lt6911_info` to the driver's `/proc/lt6911_info`, the same shell
lookup path used by the SG2002 MMF diagnostic cache. A missing driver is not
an initialization failure.

UXC and D permanent EDID settings are written to the bridge Flash; D uses its
own Flash sequence. C supports guarded manual EDID management; its Pro erase
extent still needs hardware validation. UXC temporary RAM application belongs
to the backend's optional mode and does not change Flash. Unavailable Shadow
support only disables temporary mode, not permanent Flash management. Pro
Shadow hardware acceptance is still pending.

Capture chip initialization is shared by Pro and Cube/PCIe. Executable hooks
in `/usr/libexec/onekvm/capture-chip-init.d/` run in filename order before
MMF modules and Core. Firmware updates use the `10-` prefix and chip
information uses `20-`; a failed hook stops the remaining hooks and leaves
capture unavailable. Core still starts, so Web, SSH and the screen remain
available. The PCIe firmware hook leaves C, D and other UXC revisions
unchanged for the capture backend; only a failed read or unknown chip fails.

Firmware hooks must check the actual chip on each initialization. An already
matching image needs no programming. After a verified update, Pro and PCIe use their own
bridge reset mechanisms. Cube has no controllable bridge reset line: its EDID
API reports `REBOOT`, and firmware updates must remain pending until a machine
restart and verification. Whether a warm reboot reloads the Cube MCU, or a
power cycle is required, still needs a Cube firmware-update hardware test.
Cube firmware deltas are handled by `onekvm-capture-firmware`, using C and
shell with a numbered chain of source/target program hashes. Unvalidated Cube
timing candidates are not installed. Pro firmware deltas remain unimplemented.

## Early OLED

The earliest Alpha Cube uses GPIOA27 for ATX reset. Its OLED uses hardware
I2C1 on the SD1 pads. The common device tree also defines a GPIO-I2C OLED bus
whose SDA is GPIOA27; probing that unused bus can drive the ATX reset line.
The kernel must disable this GPIO-I2C node before probing on Alpha, along
with the Wi-Fi SDIO node that overlaps its hardware I2C1 pads. C906L owns
the hardware I2C1 controller, so Linux must also leave that controller
unbound and retain its shared I2C functional clock. Otherwise Linux can
gate the clock and freeze the boot animation with bytes left in the TX FIFO.
PCIe and later GPIO OLED variants keep their normal device and clock setup.
The Alpha ATX profile provides power, reset, PWR LED, and HDD LED.

The initramfs installation and recovery displays prefer C906L and require a
fresh sequence ACK within one second of posting a command. A warm reset can
retain shared-memory magic from an earlier OneKVM boot even when the factory
FIP starts no C906L image. Missing magic or a fresh ACK makes the early module
probe and draw to the Cube / PCIe OLED directly. The normal system OLED service
continues to use C906L; this hardware-probing path is specific to initramfs.

Pro Desk uses a single-draw RGB565 [boot splash](boot-splash/README.md) instead.
It shows the OneKVM logo before storage initialization, English installation
stages on fresh images, and the boot logo until the normal screen service takes
over. Display failures do not prevent installation or boot.

## Shared storage

Before the first boot-FAT mount, firstboot validates the selected storage
layout and checks the unmounted FAT with `fsck.fat -n`. A clean filesystem
needs no repair or writes. Detected inconsistencies get one automatic repair
and a read-only verification; errors and timeouts never select Recovery.
RAM status and checker output are in `/run/onekvm-boot-fat/`. Each checker
invocation has a 15-second userspace timeout and 2-second kill grace period;
kernel I/O stalls can still outlive a userspace timeout. Mounted partitions
and whole disks are rejected by device number, including mount aliases.

`storage/storage-layout.sh`, `storage/build-image.py`, and the firstboot hook
are shared by Cube/PCIe, Pro SD, and Pro eMMC. Profiles define geometry;
root parameters select the medium and slot, and ambiguous devices are rejected.
SD installers are compact carriers expanded on first boot. Pro eMMC uses
cmdline partitions and preserves the vendor bootloader prefix. All profiles
use 128 MiB SquashFS A/B slots, a shared upper/work, and f2fs for new userdata while retaining
existing ext4/f2fs. Installed Cube/PCIe and both Pro media use a 128 MiB boot FAT;
SD release carriers size their FAT to the boot files plus headroom. The `bootgrow`
marker requests a 128 MiB installed FAT: firstboot protects rootfs and all boot
files (including firstboot configuration) in RAM before recreating partitions;
Cube/PCIe carriers also use `bootclone` containing `A` to ship only `boot.sd`.
Before changing partitions, firstboot creates and checks an independent
`boot-b.itb` in the protected RAM copy, retaining matching A/B build IDs.
Both installation markers are removed after restoring the installed FAT.
eMMC also retains its vendor raw A/B boot payload ranges. Legacy Cube cards
with the former 32 MiB loader remain valid upgrade targets. OE recipes remain in `onekvm-distro`; Pro boot entry limits
are documented in `onekvm-distro/docs/nanokvm-pro-ab-recovery.md`.
Generated images end with a 512-byte profile and SHA-256 trailer; callers must
validate it before selecting an SD or eMMC writer.

## License

Userspace lifecycle files are MIT licensed. The machine detector and early
OLED kernel module are GPL-2.0-only. See [LICENSE](LICENSE) and
[LICENSES/MIT.txt](LICENSES/MIT.txt).

## Optional SDIO Wi-Fi

The [official model matrix](https://wiki.sipeed.com/hardware/en/kvm/NanoKVM_PCIe/introduction.html)
lists Wi-Fi as optional on PCIe and absent on Cube Lite/Full. Both SDIO and
its `mmc-pwrseq-simple` node default to disabled in the shared DT. Before
unflattening it, the kernel enables both only for a valid, READY C906L PCIe
marker. Alpha retains its OLED I2C1 clock and ATX reset pin protection.
Beta, Lite, missing markers and unknown variants leave SDIO disabled.

The DT owns the six SD1 pad muxes, GPIOA26 mux/reset, power delays and the
SD1 clock. Two exact pinctrl-single register windows expose only these seven pads,
including to debugfs; reserved MMIO must never be mapped as additional pins. PCIe has
no card-detect line and the module can be absent, so use `broken-cd` rather
than `non-removable`. An empty mmc1 host is valid; mmc0 remains the storage
card. The machine script only inspects physical devices under this SDIO
host. It neither exports/drives GPIOA26 nor assumes a single SDIO vendor ID.
Detecting a module does not establish driver or firmware support.

## Installer network policy capability

The storage builder's `--network-policy` flag sets metadata v1 feature bit 0
(offset 22). Set it only when the rootfs contains policy-aware Core/CLI,
startup units, firewall tools and extension host. It is included in the full
image digest and recorded as `network_policy=1` in the manifest. The default
remains zero for existing images. Installers without feature-bit support reject
these images rather than ignoring installed opt-outs.
