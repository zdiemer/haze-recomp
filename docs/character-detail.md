# Character detail

Haze reduces character geometry a short distance from the camera. The Video
setting **Character detail** (`character_detail` in `config/graphics.ini`,
override `HAZE_CHARACTER_DETAIL`) controls this:

- `full` (default) keeps soldiers and other characters on their closest-range,
  highest-detail model at every distance.
- `original` keeps the game's distance-based selection.

The setting applies live from the Video page or an F6 reload.

## How the game selects character detail

BLUS30094 01.00 gives every character a LOD component (type name
`chr lod component`, component id `CLOD`). Its update, guest `00245CE8`, stores
the character's camera distance at `+20` and selects one of four 128-bit
part-visibility masks into `+38`:

| Distance (shipped thresholds) | Mask | Parts enabled |
| --- | --- | --- |
| below `+10` (7.015) | `+3C` | 77-84 |
| `+10` to `+14` (25.0) | `+4C` | 34-40 |
| `+14` to `+18` (999999) | `+5C` | 34-40 |
| `+18` and above (unmeasured or special) | `+6C` | 1 |

Each set bit enables one mesh part, so a character at more than about 7 units
loses over half of its parts. This is the close-range detail loss in
[#26](https://github.com/zdiemer/haze-recomp/issues/26). Props and level
geometry use a separate distance LOD selector (`0069B400`) and are unchanged.

`scripts/patches/patch_character_lod.py` changes only the two stores that select
`+4C` and `+5C`. When Full is selected, they store `+3C`. The distance
measurement, thresholds, the `+3C` and `+6C` paths, and the separate detail
predicates (`00243CF8`, `00243DD8`, `00243EC0`, `00243F20`, `00247100`) stay
original. The patch is hash-guarded and fails the build if the translation
changes.

The retail binary also contains a stubbed "character LOD disabled" query
(`00243CE8`, always 0). Forcing that query on is not a usable high-detail mode.
It selects the 1-part `+6C` mask, and characters lose their armor and helmets in
game.

## Verification

In the later jungle checkpoint, a temporary probe logged each component's
distance and selected mask (local evidence only, not shipped):

- `original`: components below 7.015 used `+3C` (84 parts). Components at 30-48
  units used `+5C` (34 parts), and others at mid range used `+4C` (34-40 parts).
- `full`: every component the game had reduced used `+3C` (77-84 parts). `+6C`
  selections were unchanged.

Captured frames showed armored allies and enemies rendered normally at mid
range. This is a gameplay check of one checkpoint, not a full campaign pass.

## Texture streaming at checkpoint load

Texture pop-in after a checkpoint load is separate. Dense frame captures show
some surfaces using a low-resolution mip for several seconds after the fade-in
before their sharp textures arrive. The streamer tick (`006A6BE8`) advances
per-texture state using timers derived from the frame time. Its request states
are not yet traced well enough to change safely, so this setting does not
change texture streaming.
