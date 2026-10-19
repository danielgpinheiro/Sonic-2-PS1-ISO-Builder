# Changelog

Updates to the Sonic 2 PS1 ISO Builder, newest first. To get an update, rebuild your disc image with the new
version of the builder (same steps as in the [README](README.md)).

## Build stamp on the title screen (October 2026)

- **The title screen shows which build you are playing:** a small grey line in its bottom-left corner, e.g.
  `V 2E8A04CD` (in the menus' font, which has capital letters only). It is the commit of the RSDKv4-ps1 port the
  executable was built from, so a screenshot or a quick look tells which version runs in DuckStation, PCSX-Redux or on
  a console. It appears only on the title screen.
- The builder writes it to a small `VERSION.TXT` file at the root of the disc (from `bin/VERSION.TXT`, which comes with
  each update together with the executable). A disc without that file shows `V UNKNOWN`.
- The executable also carries the port's further special-stage speed work since the previous update (port commits
  `f307b1a` to `f6643bb`).
- Checked with the port's automated tests in PCSX-Redux and DuckStation (the special stage's memory check included).
  Not yet tested on a console.
- The image's SHA-256 changes (the new value is in the README's Output section).

## Faster special stages, steadier rotated sprites (October 2026)

- **Special stages run faster:** the code that sorts and draws the half-pipe and turns its 3D points into screen
  positions now runs as hand-written assembly. Special stage 1 needs about 10 % less CPU time a frame (335.6 -> 302.4
  of a frame's 262 scanlines in PCSX-Redux), stage 7 about 9 % less (356.0 -> 324.3). They still run a little below full
  speed; the work continues. The picture is the same: every change was checked frame by frame.
- **Steadier rotated sprites:** a rotated sprite's picture no longer bends slightly along its diagonal (the bend
  changed with every angle: Sonic in loops and on quarter pipes, rotating objects, the special stages' messages). Each rotated sprite is now
  drawn as an exact parallelogram.
- Rotated sprites flipped vertically are no longer drawn mirrored left-to-right; they follow the original game's rule.
- Checked for every frame of the player animations at every angle, facing left and right, with the port's automated
  tests. Not yet tested on a console.
- The image's SHA-256 changes (the new value is in the README's Output section).

## Memory safety fix: the stack has its own area (October 2026)

- **The main stack moved out of the heap** (port issue V4-102): the processor's working data (the stack) now lives in
  a separate, fixed 8 KB area. Before, nothing kept it apart from the memory the game loads into, so with little
  memory left a load could be placed on the stack and freeze the game. No such freeze was seen in normal play, but the
  special stage's load had only about 5 KB to spare, and the port's tests froze it there with a few KB less.
- When memory runs out, a request for more now fails cleanly instead of being handed the stack's memory.
- **More room in the special stages:** their two drawing buffers are 40 KB each instead of 64 KB (a stage fills at
  most about 24 KB of one), so the special stage's load now has about 13 KB to spare, after paying for the stack's own
  area. The zones keep about 80 KB free (8 KB less than before: the stack's area).
- Checked with the port's automated tests in PCSX-Redux and DuckStation. Not yet tested on a console.
- The image's SHA-256 changes (the new value is in the README's Output section).

## Faster special stages (October 2026)

- **Faster special stages:** stage 1 from about 44 to about 47 frames a second in emulators, stage 7 (the one with the
  most bombs) from about 38.5 to about 44. The bombs now run as native code instead of the game's script, the 3D pass
  does less work per face and vertex, and one layer of the star field that drew the same picture twice in the same
  place is drawn once (about 30 % less GPU work there).
- **The zones are a little faster** (up to about 2 % less CPU time a frame, Emerald Hill the most).
- **Only controller port 1 is read:** the game uses one controller; port 2 was polled too and, when empty, its time-out
  was waited out every frame.
- **Flipped level tiles as mirrored sprites:** where the console's GPU can mirror a sprite (every PSone has such a GPU),
  flipped tiles are drawn as smaller mirrored sprites instead of reversed quads. A self-test at boot checks it and keeps
  the previous way elsewhere, including emulators that don't mirror sprites.
- The game looks and plays the same: each change was checked frame by frame against the previous build.
- The image's SHA-256 changes (the new value is in the README's Output section).

## The disc's executable is named after the game (October 2026)

- The disc now boots **`SONIC2.EXE`** instead of `PSX.EXE` (the `BOOT` line of `SYSTEM.CNF`).
  Emulators and optical drive emulators that show a disc's executable name now show the game's. Nothing in the
  game itself changes.
- The image's SHA-256 changes with it (the new value is in the README's Output section).

## Earlier updates (October 2026)

- **No freeze if the CD drive stops answering:** at a 1-up, the pause menu or a load, the game used to wait for the
  drive forever; it now asks again after a few seconds and carries on.
- **Delete a save file:** on the save select, press Select on a used file and confirm "Delete Saved Game?" (as the
  mobile versions' Delete button). The other files, the Time Attack records and the settings stay.
- **Shields setting relabeled:** its four values now say what they do: Sonic 2, Sonic 3 + insta (the Sonic 3 shields
  in the monitors and the insta shield; the default), Random (every monitor random) and Random + insta (random with the
  Sonic 3 shields and the insta shield). The earlier labels assumed a different meaning.
- **Wing Fortress's boss sounds:** its hits, laser and explosions were silent; they play now. A few other sounds that
  could be skipped play too (Hill Top, Oil Ocean, Hidden Palace's waterfalls, the special stage's jump).
- **Fixed: the special stage could stay blank** (a plain screen with only its music) when reached from a zone in the
  previous update. It now has memory to spare again on every route in.
- **Sonic 3 shields and the insta shield:** the original 2013 release's hidden shield setting is now in Options
  (Shields: Sonic 2, Sonic 3 + insta, Random, Random + insta) and works with save files. It starts on Sonic 3 +
  insta shield: fire, bubble and lightning shields from the monitors, and Sonic's insta shield on a second jump press.
  Time Attack keeps the classic shields.
- **Faster special stages:** the half-pipe runs about 12 % faster. In emulators it now plays at about 74 % of full
  speed, up from about 66 %.
- **Wing Fortress at full speed:** it used to drop slightly below.
- **Smoother busy zones:** Hidden Palace runs about 11 % faster and Aquatic Ruin about 7 %, Chemical Plant a little,
  and every zone gains a little.
- **Quicker underwater:** being underwater (the air countdown, bubbles and splashes) costs much less time, which is where
  the water zones gain most. Hidden Palace's own stage logic is quicker too.
- **Faster game scripts:** the engine now reads the game's script variables directly instead of looking each one up
  every time, which speeds up every stage a little.
- **More free memory** for the game, so loading the special stages is safer.
- Apart from the new shield setting, nothing looks or plays differently: every change was checked frame by frame
  against the previous version.
