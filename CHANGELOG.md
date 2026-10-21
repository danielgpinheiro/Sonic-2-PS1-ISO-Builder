# Changelog

Updates to the Sonic 2 PS1 ISO Builder, newest first. To get an update, rebuild your disc image with the new
version of the builder (same steps as in the [README](README.md)).

## Aquatic Ruin at full speed, the title card's text in the clear (October 2026)

- **Aquatic Ruin runs at 60 frames a second**, as Sonic and as Sonic & Tails, also where leaves fly, a Grounder bursts
  out of a wall in a shower of bricks and rings scatter. Its leaves, bushes, bricks, Grounders and springboards now run
  as native code instead of through the game's script interpreter, as do the water's surface and the zone's palette
  split; the water zones keep both of their palettes ready instead of converting them every frame, and sprites near the
  water line are drawn faster. Across the game about 160 objects that only draw one sprite skip the interpreter too.
  Everything behaves and looks exactly as before (checked frame by frame against the previous version in every zone).
- **The zone title card:** "SONIC THE HEDGEHOG" was half hidden behind the red stripe (the PS1's screen is narrower than
  the PC version's). The red stripe is now drawn a little narrower, so the text sits wholly on the yellow stripe as on
  the PC; the card's timing is unchanged.
- **Metropolis' giant gears stay at full speed** on every emulator: its spinning tubes (cylinders) run as native code
  too, which gives that spot more room than before.
- **The loading icon waits while a level load is copying graphics to video memory.** Two copies at the same time stop
  the game; in the Sonic 1 port this froze the end credits in the PCSX-ReARMed emulator. Sonic 2 had the same weak spot.
- The title screen shows the new build, `V 379BB44F` (the RSDKv4-ps1 port's commit `379bb44`).
- Checked with the port's automated tests in PCSX-Redux and DuckStation. The image's SHA-256 changes (the new value is
  in the README's Output section).

## Hidden Palace under water and Metropolis' giant gears at full speed (October 2026)

- **Hidden Palace's flooded section runs at 60 frames a second**, as Sonic and as Sonic & Tails. Under water the
  background and the level ripple, which made the console redraw the screen as many thin strips and slowed the game to
  less than half speed (some tiles also went missing). The background's rippling rows are now prepared while the disc
  is built, the level's front and back layers are drawn in one pass instead of two, and the Jellygnites run as native
  code.
- **Metropolis' giant gears (Acts 2 and 3) no longer slow the game down.** The gears, the conveyor belts' platforms, the
  wheel platforms, the moving and swinging platforms and the one-way doors (the same door in Chemical Plant, Hill Top and
  Death Egg) now run as native code instead of through the game's script interpreter; they behave exactly as before.
- **Emerald Hill's Coconuts** (the monkey) no longer climbs into the treetop or into the ground after coming back on
  screen: he restarts his climb where he should. The Hill Top seesaw was checked and works as in the original game:
  you are launched from whichever end the ball lands opposite you.
- Every change was checked to draw exactly the same pictures and play exactly the same as before, with the port's
  automated tests in PCSX-Redux and DuckStation (new tests for each place).
- The title screen shows the new build, `V CB40CC8F` (the RSDKv4-ps1 port's commit `cb40cc8`). The image's SHA-256
  changes (the new value is in the README's Output section).

## Tester fixes: Death Egg gate, coloured emeralds, sprite edges (October 2026)

- **Death Egg Zone: a spin dash after beating Mecha Sonic no longer takes you past the gate early.** On the PS1's
  narrower screen the camera, frozen for a moment by the spin dash, left Eggman outside the area where objects are
  updated, so he never held the door shut and you rolled past before his scene. Eggman is now always updated (a small
  change to the level's layout file made while building the disc); the door stays shut until he runs, as when you walk
  up to it.
- **The save select shows collected Chaos Emeralds in their colours** (as on mobile), so it is easy to see which ones
  are missing; missing ones stay grey.
- **Mirrored player sprites no longer show a stretched column** when the sprite happened to sit at the left edge of a
  texture page in video memory (one column was drawn twice, so part of the sprite shifted by a pixel between animation
  frames). The disc builder now keeps the player's sprites off that edge where there is room; a few still sit there in
  crowded stages.
- The title screen shows the new build, `V 5EA4BF02` (the RSDKv4-ps1 port's commit `5ea4bf0`).
- Checked with the port's automated tests in PCSX-Redux and DuckStation (new tests for each fix). The image's SHA-256
  changes (the new value is in the README's Output section).

## Flip-bit sprites on newer consoles (October 2026)

- **On consoles with the newer GPU (every PSone, most later PlayStations), mirrored sprites are now drawn with the
  GPU's own flip feature**, as mirrored level tiles already were, instead of as textured four-corner shapes: less GPU
  work for every sprite facing left. It applies to sprites drawn at their own size and not rotated (Sonic 2's player
  standing or running on flat ground included); rotated and scaled sprites are drawn as before.
- The start-up check is stricter: the game first asks the GPU its version (nocash's GPU detection; the earliest
  consoles' GPU lacks the flip feature and never uses it), then keeps the existing test drawing read back from video
  memory, so an emulator that reports a newer GPU but doesn't draw the flips (some versions of PCSX ReARMed) still
  gets the old drawing. The previous check could, in a rare case, misread an old GPU's answer.
- The picture is the same either way: checked frame by frame in the DuckStation preview build, which draws the flip
  feature, against the old way of drawing. Not yet tested on a console.
- The title screen shows the new build, `V 857E6CE9` (the RSDKv4-ps1 port's commit `857e6ce`).
- Checked with the port's automated tests in PCSX-Redux and DuckStation (the special stages keep their speed; the
  zones' and special stage's memory checks included).
- The image's SHA-256 changes (the new value is in the README's Output section).

## Much faster special stages (October 2026)

- **The special stages now run at or very near full speed (60 frames a second).** In the port's test runs with Sonic &
  Tails (the hardest case: two players), special stages 1, 3, 7 and 8 keep a full 60 frames a second, and stages 2, 4,
  5 and 6 drop at most 3 frames out of every 600 (10 seconds): the drops come during jumps, when the camera gets close
  to the half-pipe. With Sonic alone, stage 1 keeps 60 frames a second too. With the previous update's executable the
  special stages still ran visibly below full speed, more so with Sonic & Tails.
- How: the half-pipe's drawing and 3D maths do less work per frame (more of it in hand-written assembly; faces and
  sprites that can't be seen are dropped earlier), and more of the special stage's scripts run as native code instead
  of the script interpreter: the HUD, the players' controls (Tails's computer-controlled following among them) and the
  rings that scatter when you hit a bomb (dozens at once, which used to slow the game down right after a hit).
- The picture and the gameplay are the same: every change was checked frame by frame against the previous version
  (pictures, positions and the drawing commands), for every special stage.
- The title screen shows the new build, `V 250CB682` (the RSDKv4-ps1 port's commit `250cb68`).
- Checked with the port's automated tests in PCSX-Redux and DuckStation, the special stage's and the zones' memory
  checks included. Not yet tested on a console.
- The image's SHA-256 changes (the new value is in the README's Output section).

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
