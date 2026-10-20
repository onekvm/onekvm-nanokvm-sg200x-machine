# Pro Desk early splash

`onekvm-boot-splash` draws the OneKVM logo to the Pro Desk's 172×320 RGB565
framebuffer, rotated 270° to a 320×172 landscape canvas. It draws once and
exits, leaving the image visible until Recovery or the normal screen service
renders its first frame. It does not modify SPL, U-Boot or kernel boot arguments.

The shared firstboot hook loads the matching panel modules before storage
initialization and shows `boot`. Fresh installation advances `install 1` through
`install 7` at actual storage checkpoints, then returns to the boot logo. An
initialization error shows `error` before the existing fatal handling. Display
failures never stop storage initialization or normal boot. Boards without the
Desk framebuffer simply skip the artwork; Cube/PCIe retain their OLED module.

The artwork uses the existing `onekvm-screen-display/src/init/logo.png`, copied
here as `logo.png`. English captions are rasterized using DejaVu Sans.
`generate-assets.py` requires Python/Pillow and regenerates the checked-in
`assets.h`; OE and the target need neither Pillow nor a font renderer. Artwork
is RGB565 run-length encoded in the binary.

Build target binaries and images through `onekvm-distro` OE/kas. For development,
`--output FILE.ppm` renders the same logical canvas to a portable preview without
opening a framebuffer. The CLI also accepts `--framebuffer PATH` for a specific
framebuffer; unsupported geometry or pixel formats are rejected.

Run `make test-boot-splash` on the development host to check pixel format
validation, the fbtft compatibility flag, panel orientation and padded strides.

The installer writes its selected language to the canonical firstboot settings.
Before installation progress begins, the hook reads only that language statement
from the boot volume; it never sources or evaluates firstboot.txt. English is the
fallback. `--language en|zh|zh_tw` (before other options) selects preview captions;
Chinese captions are generated with Noto Sans CJK at development time.

The OneKVM SD U-Boot uses the same English boot artwork. The Linux JD9853 driver
keeps display scanout off through both reset/init passes and enables it only after
its first full framebuffer transfer. This avoids briefly exposing stale GRAM
between U-Boot and the early splash. The original eMMC U-Boot is unaffected.
