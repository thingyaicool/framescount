# Frame Counter (Geode, Android)

Top-left HUD that counts the frame gap between your inputs:

```
10-15: 3
7-9:   1
5-6:   0
4 / 3 / 2 / 1
<1 cbs   (sub-frame gaps, only possible with click-between-steps)
16+      (anything bigger)
```

- Counts presses AND releases (ship, wave, robot, everything) on the jump button.
- Works on live play, imported macros and bots (hooks the game's own input funnel, `GJBaseGameLayer::handleButton`).
- Counts reset every attempt. Optional session totals in brackets.
- Sub-frame (CBS) gaps (0.83, 0.92, 0.55 ...) all go in ONE bucket and flash "(with cbs)" at the top for 1 second.
- v1.1: fast-wave fix (gaps now measured on the game's level clock + click timestamps, built for GD 2.2081 native Click Between Steps), **FC button in the pause menu** (opens settings: size, opacity, colors, sound...), **sound alerts** (pick a trigger like "3 or tighter" or "Sub-frame (CBS)", a built-in sound or your own file, volume, cooldown).
- Extras: best gap, shortest hold, detected TPS, color-coded rows, compact mode, completion summary popup, per-level stats file, draggable HUD (pause, then drag it).
- Not active in the editor's playtest (PlayLayer only).

## Build the .geode (no PC toolchain needed)

1. Make a new GitHub repo, push this whole folder (the `.github` folder MUST be included):
   ```
   git init && git add -A && git commit -m "frame counter"
   git branch -M main
   git remote add origin https://github.com/<you>/frame-counter.git
   git push -u origin main
   ```
2. GitHub -> Actions tab -> "Build Frame Counter (Android)" (it also runs on push). Wait for green.
3. Open the run -> Artifacts -> **Build Output** -> download, unzip -> `hman.frame-counter.geode` (contains both 32-bit and 64-bit Android builds).
4. Copy it to your phone's Geode mods folder:
   `Android/media/com.geode.launcher/game/geode/mods/`
   (or open the file with the Geode launcher). Restart GD.

## Settings
Geode -> Frame Counter -> cog. Move the HUD: pause a level and drag it. Reset it with "Reset HUD Position".

## Stats files
`Android/media/com.geode.launcher/game/geode/mods/hman.frame-counter/stats/` (or wherever your Geode save dir is).

## Tests
`g++ -std=c++17 -o t test/test.cpp && ./t` tests the gap/bucket logic (`src/logic.hpp`).
