#!/usr/bin/env python3
"""Generate the SG200x WIDTHxHEIGHTp60 and 1080p90 EDID presets in share/edid.

Run from any directory. The NanoKVM Pro presets live in onekvm-nanokvm-pro-machine.
"""

from pathlib import Path
from edid_modes import generate_common


if __name__ == "__main__":
    generate_common(Path(__file__).resolve().parents[1] / "share/edid", "sg200x")
