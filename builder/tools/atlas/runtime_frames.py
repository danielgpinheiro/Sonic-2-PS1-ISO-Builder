#!/usr/bin/env python3
"""Sheet regions of the frames Sonic 2's scripts change at runtime (docs/30 phase 3.1, user decision 2026-09-26:
per-object regions).

tools/rsdkmanifest lists the frames the object startups define. An `EditFrame` in an update / draw sub (or an
`EditFrame` / `SpriteFrame` there whose operands are variables) can make a frame read another part of its sheet;
this table holds, per object, the sheet rect that covers every value the script can produce (derived from the
bytecode by hand; the reasoning is next to each entry). build_atlas.py adds a region when the object is in the
stage block and its sheet is loaded, and drops the `drop` frames (startup frames always edited before drawing).

Objects whose runtime edits stay inside their own startup frames need no entry: Clucker (height <= 32 of frame 0),
Eggman Laser (Zone09: width <= 64 of frame 0; Zone11: frame 5's 168-line beam, height <= 167 from the bottom),
Eggman Platform (only the pivot changes), B08 Eggman (frames 13 / 14: width <= 80 of the same rects).
Hex No (LSelect) has variable SpriteFrames only in its startup loops, which rsdkmanifest runs. Names repeat across
stages with other scripts (Zone01's Waterfall has no runtime frames): a region applies only where its sheet is loaded.
Left out on purpose: VS mode strips (`vs.playerID` != 0: Title Card, LSelect, Special; phase 4 decides VS), the
mobile device type's frames (the PS1 is RETRO_STANDARD), ZoneM (Egg Gauntlet, not on the disc).
"""

# object name (StageConfig / GameConfig, spaces kept) -> {'add': [(sheet, x, y, w, h)], 'drop': [(sheet, x, y, w, h)]}
RUNTIME = {
    # GlobalCode: the end-of-act sign's face, frame 5 edited to one of three 48x32 faces (constant operands in the
    # update sub, picked by the player who passed it).
    'Sign Post': {'add': [('Global/Items2.gif', 34, 1, 48, 32), ('Global/Items2.gif', 34, 34, 48, 32),
                          ('Global/Items2.gif', 34, 67, 48, 32)]},
    # Zone03 (ARZ): frame 0 = (59, 42) 56 x value0; value0 starts at 32 (startup) and grows by 2 up to 80.
    'Breakable Pillar': {'add': [('ARZ/Objects.gif', 59, 42, 56, 80)]},
    # Zone04 (CNZ): three reels, frames at x 1, w 32, y = 256 + 32 * symbol (+ 1..32 for the next symbol), height
    # <= 32; symbols 0-5 from the reel tables, 0 -> 6 for Knuckles: y 256..511.
    'Slot Display': {'add': [('CNZ/Objects.gif', 1, 256, 32, 256)]},
    # Zone06 (MCZ): frame 0 = (232, 176 - d) 24 x (d + 80), d = the vine's pull 0..176 px: y 0..255.
    'Pull Vine': {'add': [('MCZ/Objects.gif', 232, 0, 24, 256)]},
    # Zone08 (HPZ): frame 0 = (484, 0) 28 x (value1 - y), the column up to 256 px (value2 = start - 256 px caps
    # it). The startup's frame 0 (484, 256) lies past the 256-line sheet and is always edited before drawing.
    'Water Geyser': {'add': [('HPZ/Objects.gif', 484, 0, 28, 256)], 'drop': [('HPZ/Objects.gif', 484, 256, 28, 256)]},
    # Zone08 (HPZ): the falling water, height = water level - y + 16 capped at 256.
    'Waterfall': {'add': [('HPZ/Objects.gif', 223, 0, 32, 256)]},
    # Zone11 (WFZ): frame 0 = (236, 176 - d) 24 x (d + 80), d = (y - value2) >> 16 in -32..145 (the chain's
    # start offsets and 2 px x 48 / 80 frames of travel): y 31..255.
    'Pull Chain': {'add': [('SCZ/Objects.gif', 236, 31, 24, 225)]},
    # Special: rings and bombs are TEXTURED_C faces in the half-pipe's Scene3D (centre +- extent 17 x 16: 34 x 32 texel
    # boxes, one wider than the 32 x 32 sprite frames): the ring frames (centres from the Ring's U / V tables: U 17..215,
    # V 17 / 50 -> x 1..231, y 1..66), the bomb (centre 83, 50) and the shadows (row V 50) (Ring / Bomb update subs) ->
    # the sheet's top-left 232 x 67 (134 wide missed the late spin frames: face misses at frame 900, docs/30 phase 7).
    'Ring': {'add': [('Special/Objects.gif', 0, 0, 232, 67)]},
    'Bomb': {'add': [('Special/Objects.gif', 0, 0, 232, 67)]},
    # Special: the checkpoint gates are TEXTURED_C faces too (Special function 11): U centres from table 16929 by frame
    # (165..330), V 256, extent 16 -> x 149..346, y 240..272 (missed at the first checkpoint: docs/30 phase 7).
    'Checkpoint': {'add': [('Special/Objects.gif', 149, 240, 198, 33)]},
    'Checkpoint 2PVS': {'add': [('Special/Objects.gif', 149, 240, 198, 33)]},
    # Special: the emerald line reads x 403 when specialStage.emeralds > 127 (the startup only saw 0).
    'Special Finish': {'add': [('Special/Objects.gif', 403, 311, 80, 16)]},
    # LSelect: the text menus (DrawMenu -> DrawTextMenu: 8 x 8 characters at ((c & 15) * 8, (c >> 4) * 8 + 0 | 128) of
    # the object's sheet, Text.gif: 128 x 256, the white and the highlighted grid). One atlas per stage: the other
    # DrawMenu objects (VS menus, results) read the same rect.
    'Menu Control': {'add': [('LevelSelect/Text.gif', 0, 0, 128, 256)]},
    # LSelect: the zone button's middle, frame 1 / 4 = (5, 151 | 200) (value1 - 8) x 48, up to the right cap at x 221.
    'Zone Button': {'add': [('LevelSelect/Icons.gif', 5, 151, 216, 48), ('LevelSelect/Icons.gif', 5, 200, 216, 48)]},
}


def regions(name):
    """(add, drop) rect lists for an object name (either spelling: engine typeNames drop the spaces)."""
    key = name.replace(' ', '').lower()
    for k, v in RUNTIME.items():
        if k.replace(' ', '').lower() == key:
            return v.get('add', []), v.get('drop', [])
    return [], []
