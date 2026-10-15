# Changelog

Updates to the Sonic 2 PS1 ISO Builder, newest first. To get an update, rebuild your disc image with the new
version of the builder (same steps as in the [README](README.md)).

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
