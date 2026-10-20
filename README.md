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

The machine package installs `share/edid/*.edid.bin` under
`/usr/share/onekvm/edid/presets/`. Core Display settings and the EDID editor
read this same directory. Presets remain available without the editor, and
factory reset uses this machine's `factory.edid.bin`. Other machine packages
can supply their own files at the same path.

NanoKVM presets:

- `factory.edid.bin` — default 1920×1080 @ 60 Hz
- `1080p60.edid.bin` — 1920×1080 @ 60 Hz
- `720p90.edid.bin` — 1280×720 @ 90 Hz
- `1440p30.edid.bin` — 2560×1440 @ 30 Hz
- `1620p30.edid.bin` — 2880×1620 @ 30 Hz (maximum native SG2002 VPSS output)

The editor saves modified copies in its own data directory; it cannot
overwrite the machine presets.

The Pro machine package installs the 1080p60 preset as its factory EDID.
The shared `onekvm-capture-chip-init.service` applies it before Core starts on
the first boot with new OneKVM data. It saves the previous bridge EDID in
`/var/lib/onekvm/edid/before-pro-default.bin`, verifies the driver's readback,
and then creates `pro-default-initialized` in the same directory. Later boots
and package upgrades preserve the user's EDID. Clearing OneKVM data also
clears this marker, so factory reset restores the 1080p default on next boot.
This uses the bridge driver's EDID operation; firmware code is not replaced.

Capture chip initialization is shared by Pro and Cube/PCIe. Executable hooks
in `/usr/libexec/onekvm/capture-chip-init.d/` run in filename order before
MMF modules and Core. Firmware updates use the `10-` prefix and default EDID
uses `20-`; a failed hook stops initialization and prevents capture startup.

Firmware hooks must check the actual chip on each initialization; the EDID
marker must not suppress later firmware updates. An already matching image
needs no programming. After a verified update, Pro and PCIe use their own
bridge reset mechanisms. Cube has no controllable bridge reset line: its EDID
API reports `REBOOT`, and firmware updates must remain pending until a machine
restart and verification. Whether a warm reboot reloads the Cube MCU, or a
power cycle is required, still needs a Cube firmware-update hardware test.
Cube firmware deltas are handled by `onekvm-capture-firmware`, using C and
shell with a numbered chain of source/target program hashes. Unvalidated Cube
timing candidates are not installed. Pro firmware deltas remain unimplemented.

## Early OLED

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
eMMC also retains its vendor raw A/B boot payload ranges. Legacy Cube cards
with the former 32 MiB loader remain valid upgrade targets. OE recipes remain in `onekvm-distro`; Pro boot entry limits
are documented in `onekvm-distro/docs/nanokvm-pro-ab-recovery.md`.
Generated images end with a 512-byte profile and SHA-256 trailer; callers must
validate it before selecting an SD or eMMC writer.

## License

Userspace lifecycle files are MIT licensed. The machine detector and early
OLED kernel module are GPL-2.0-only. See [LICENSE](LICENSE) and
[LICENSES/MIT.txt](LICENSES/MIT.txt).
