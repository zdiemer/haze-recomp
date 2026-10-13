"""Emit the SDK's empty PPU ABI header; no game input or translated functions."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
from ppu_lifter import PPULifter

output = Path(sys.argv[2])
output.mkdir(parents=True, exist_ok=True)
(output / "ppu_recomp.h").write_text(PPULifter().emit_header(), encoding="utf-8")
