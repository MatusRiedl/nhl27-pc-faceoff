# What's new

Newest first. Download: [the release page](../../releases/latest).

## v0.20.1

- 🛠️ **DLSS no longer closes the game at start on some RTX cards.** Before the game reserved its memory, the DLSS
  start-up step let the NVIDIA driver place its own memory over an address the game needs, and the game closed right
  after the splash screen. The emulator now reserves the game's low memory first, so the driver goes elsewhere. If
  DLSS still closes the game on your card, please send the play log.
- 🖥️ **Ultrawide and 4:3 screens: no more stretching.** The game's 16:9 picture is now centred with black bars on a
  screen of another shape (21:9, 32:9, 4:3, 16:10). A window within about 9 % of 16:9 (a 1440p window that lost its
  title bar and taskbar height) is still filled edge to edge. Faceoff → Graphics → **Aspect ratio**: Keep 16:9 (the
  new default) or Stretch (the old look). Mouse and touch-pad positions follow the picture. This applies in windowed
  mode too: the window opens maximized, so on a 21:9 monitor it is 21:9 as well. Works with sharpening and with
  DLSS. `KYTY_PRESENT_STRETCH=1` brings the stretch back without Faceoff.
- 🎚️ **Resolution now changes what the game renders.**
  - Quality presets are a share of the resolution you pick. 1440p is unchanged. 1080p Quality now renders 1280×720
    (before, it rendered the same as 1440p), so lower resolutions really run lighter.
  - **4K Quality is now a real 4K Quality:** it renders 2560×1440 and the game upscales to 3840×2160. That's heavier
    than before (it rendered 1708×960), so expect lower fps and more video memory at 4K. In our test on an RTX 5080
    the GPU ran near its limit, play held about 59 fps, and the card's memory use peaked at about 11.4 GB (desktop
    included; measured in a 1440p window). If your card struggles, pick Balanced or Performance.
  - **Off** means "render at your resolution". The most the game renders is 2560×1440, and the caption says so.
  - The render-scale slider (Advanced) is now "% of your resolution". A custom scale you saved before converts once
    to the same render size.
  - The caption under Quality shows the real chain, e.g. "Renders 1280×720 · game upscales to 1920×1080 · shown at
    1920×1080".
- **Known:**
  - Some players see RC3 run faster than v0.20.0 with the same settings (about 60 against 48 fps at faceoffs). On
    our test PC the gap is under 1 fps at the default settings and up to 3 fps with the GPU at its limit, so the
    cause isn't found yet and v0.20.1 doesn't claim to fix it. If you see it, please send a play log from RC3 and one from v0.20.1 (Faceoff →
    Log), plus the Last session line.
  - Sharpening above 0 % costs more on v0.20.x than on RC3-RC7: below a 4K window the newer builds downscale the
    game's 4K picture and then sharpen it, where the older ones skipped sharpening. On a GPU that is already at its
    limit, set sharpening to 0 % to compare.
  - Rarely, play drops to a few fps for 10-30 s and then recovers. It is not new (v0.20.0 has it too). If you hit
    it, the play log helps.
- **Updating:** Faceoff updates itself to 0.20.1 and offers the new build (v0.20.1). Saves and the shader cache stay.

## v0.20.0

- ⚡ **Faster draw path.** The emulator's speed profile (PGO) had gone stale: v0.10.x threw it away on 41 of its
  hottest functions, which is most of why RC5 felt quicker. v0.20.0 is built with a fresh profile from a match and
  an Edit Player session.
- ⚡ **Lazy shared writes are back, at every frame rate** (`KYTY_LAZY_SHARED_WRITES=1`,
  `KYTY_LAZY_SHARED_WRITES_RATES=all` in `builds/rc8/cfgGlobal.json`). v0.10.1 turned them off for the Edit Player
  crash. The crash path is fixed now: a page the merge can't back is skipped instead of closing the emulator, and a
  GPU-written page goes to the CPU in one step. Tested for 18 minutes in Edit Player's equipment grid with no crash.
  In menus and editors it keeps about two CPU threads busy. If your PC struggles there, set
  `"KYTY_LAZY_SHARED_WRITES_RATES": "60"` (play only) or `"KYTY_LAZY_SHARED_WRITES": "0"` (the v0.10.1 behaviour).
- 🎬 **Fewer hitches after the puck drops.** The emulator now remembers which pipelines a session built and builds
  them at start-up next time. In our test matches this cut pipeline work after play start from 0.9-2.2 s to under
  0.1 s. The first session after updating builds the list, so it stutters as before; later sessions don't.
- 🖼️ **Graphics tab in Faceoff:** upscaler (Off, FSR 2, XeSS, DLSS), quality presets, a sharpening slider and an
  Advanced section.
  - **DLSS** (`nvngx_dlss.dll` is included) needs a GeForce RTX card and a recent driver. It upscales from the same
    render size as FSR 2 at the same preset, so expect a cleaner picture rather than more fps: in a 20-minute test
    match it ran every frame with no failures at about the same fps as FSR 2. With DLSS picked, start-up takes about
    0.2 s longer. If DLSS can't start, the game keeps its own FSR 2 and Faceoff's status line says so.
  - **XeSS** (`libxess.dll` is included) runs on most DX12 GPUs. Its quality label can read one step higher in the
    log than in Faceoff ("UltraQuality" for "Quality"), because the game's output target is smaller than the
    window. The image is right.
  - Sharpening below a 4K window now downscales first, then sharpens (RCAS).
- 📊 **Last session card in Faceoff** (Log view): average fps, 1% low, frames over 50 ms and play time from your last
  session, with a Copy button for Discord.
- 🎮 **Controls tab in Faceoff:** rebind the keyboard and the controller. Click a box, then press the key or the
  button; Escape cancels and × clears it. A key that is already taken swaps places, and the keys the emulator keeps
  for itself (Escape, F1, F2, F7, F8, F11) are refused. Reset puts the built-in layout back. Until you change
  something, Faceoff starts the game exactly as before.
- 🧭 **Faceoff's Play tab redone:** builds as tiles, larger settings, and the window scales up on big screens.
- **Fixes from KytyPS5**, each with its original author:
  - Read-only binds over CPU-clean pages skip the region lock (nmzik).
  - The detile scratch buffer is no longer zero-filled (nmzik).
  - `V_PERM_B32` is translated (nmzik).
  - Null textures reuse their lookups, image barriers stay inline, back-to-back barrier operations are dropped, and
    page protection changes run outside the tracking locks (petrmolek).
  - Upstream PRs 2193, 2224 and 2225 plus 22c02672 and 4f9d51dd: formatted reads cover the whole GPU-written image,
    shader buffers bind after the draw inputs that can join them, CP copies reach every image they overlap, the
    render area is bounded to the real image, and pages without images skip texture invalidation.
- **Stability:** an overlap the texture cache can't resolve now recreates the image instead of closing the emulator
  (the nhl24pc pattern).
- **Logs:** the `NHL27ENV` start-up line lists every switch in effect, so a log shows exactly what ran. Slow frames
  print `NHL27HITCHWHY`, which says where the time went.
- **Measured** (CPU vs CPU, first period of a match, same PC and same day, one run of v0.10.1 against two of v0.20.0): play 1% low 40.6 →
  48-50 fps, frames over 50 ms 2 → 7-9. Smoother on average, with a few more single long frames. Our test PC got
  slower after a restart halfway through testing; before it, v0.20 held a 1% low of 56 over 20-minute matches with
  0-2 frames over 50 ms.
- **Known:**
  - A few of our long test runs froze for good at a cutscene cut (a wait that never finished), on older v0.20 test
    builds as well. If it happens to you, send `play_log.txt`.
  - GPU hangs and stalls in our last test session. Of its nine test matches, three closed with a GPU hang
    (`masterSemaphore.cpp:159`): two with FSR 2, 20 s and 105 s into play, and one with DLSS, 55 s into play. Two
    more FSR 2 matches froze for 25-30 s and then carried on. The matches before that session were clean with
    FSR 2, DLSS and XeSS. The cause is not found. Our test PC was at its memory limit during these runs, which may
    be related: the emulator commits about 14 GB at start-up and 10-15 GB more in a match (RAM plus pagefile), so
    close other large apps before you play. If it happens to you, send `play_log.txt`.
  - Some GPU-written data can still reach the CPU late; it shows as rare wrong materials in a few scenes.
  - Faceoff can open with an empty window. On our test PC, RivaTuner Statistics Server (the overlay that comes
    with MSI Afterburner) crashed the window's browser part (WebView2) at start-up, about one start in six. Close
    Faceoff and start it again. Earlier Faceoff versions do the same.
- **Updating:**
  - From v0.10.x, Faceoff updates itself and the build.
  - From RC5, take `Faceoff.exe` out of the release zip once and put it over the old one.
  - Saves stay. The shader cache rebuilds once (the shader translation changed).

## v0.10.1

- 🩹 **Edit / Create Player no longer crashes** when you scroll through equipment. Thanks to the player who found it.
  The cause was `KYTY_LAZY_SHARED_WRITES`, a speed-up from duy64's tree that every build since RC3 had on: a CPU write
  next to memory the GPU had just written didn't wait for the GPU. It is now `"0"` in `builds/rc7/cfgGlobal.json`.
  (Removing the line is not enough: the emulator turns it on when the line is missing.)
- Same emulator as v0.10.0 otherwise. Faceoff 0.10.1 picks the new build and now checks the renamed repository
  for updates.
- **From v0.10.0:** Faceoff updates itself and the build. **From RC5:** Faceoff offers the new build; take
  `Faceoff.exe` from the release once by hand. Saves and shader cache stay.
- On an older build, set `"KYTY_LAZY_SHARED_WRITES": "0"` in that build's `cfgGlobal.json` to get the same fix.

## v0.10.0

The project is now called **Faceoff**, and releases are numbered from v0.10.0 (this one is the former RC6).

- **Smoother play.** Memory faults are handled in larger batches, unmapped GPU-written memory is dropped instead of
  copied back, and the page-tracking locks give up the CPU instead of spinning. Played with a controller before
  release; not yet measured against RC5 on the same match.
- **Fixes from KytyPS5**, each with its original author: texture views past the last mip end cleanly instead of
  closing the emulator, a depth image keeps its own stencil record, and freed memory is unmapped from the GPU one run
  at a time (nmzik); a depth target keeps its HTile clear state (TheCruZ).
- **Settings on by default** (in `builds/rc6/cfgGlobal.json`, remove a line to turn one off): `KYTY_FAULT_COALESCE_PAGES`,
  `KYTY_UNMAP_DISCARD`, `KYTY_STENCIL_OWN_PLANE`, `KYTY_HTILE_OWNER`, `KYTY_SPIN_YIELD`.
- **Diagnostics**, only with `KYTY_PERF_STATS=1`: draw-preparation timing (`NHL27DRAWPROF`, duy64), read-fault
  breakdown (`NHL27RDSPLIT`).
- **New Faceoff look**, shared with the other Puck Peak apps: app dock at the top left, Play at the top right, check
  marks for game, emulator and shader cache, and a status bar with download progress.
- **Updating from RC5:** the auto-update installs the new emulator build and keeps your saves and shader cache. The
  launcher itself is now numbered 0.10.0, which RC5's launcher reads as older, so take `Faceoff.exe` from the zip once
  by hand.

Known: shadows can look blocky in places (seen in earlier builds too), and a cut from one cutscene to the next can
stutter.

## RC5

- **Built on RC3 again.** Testers found RC4 slower than RC3, with the overlay's texture transfers far higher. Two of
  RC4's on-ice fixes re-upload whole textures much more often than needed. RC5 starts from RC3 and leaves them out.
  RC4 stays available in Faceoff for anyone who prefers its glitch fixes.
- **Newer fixes from KytyPS5 and duy64**, each with its original author:
  - four texture fixes by nmzik (cube textures, views past the last mip, storage-texture LOD, depth textures with
    metadata);
  - a faster internal lock (Artyom Zabroda) and fewer repeated GPU interrupts (superkeka);
  - a shader math fix by duy64 (clamped reciprocal), for stability.
- **Long-session crash fix.** When the GPU runs out of dedicated buffer allocations the game keeps running and the log
  says `Buffer: dedicated allocations exhausted`.
- **Better crash reports.** `play_log.txt` now names your GPU and driver, the shader being compiled, and the window
  setup steps.
- **Faceoff 1.2.0:**
  - **Pick your build:** RC3, RC4 or RC5 on the Play tab. A build you don't have is downloaded from GitHub when you
    pick it, and checked against its known SHA-256.
  - **Auto-update:** Faceoff looks for a new release when it starts and offers to update. Saves, shader cache and
    settings stay where they are.
  - All builds share one set of saves (`_SaveData`, `_DownloadData`, `_Trophies`) and one shader cache folder.
  - The trophy pop-up setting now works.
  - After the game closes or crashes, **Open log folder** shows `play_log.txt` in Explorer for your bug report.
  - 1440p is the default resolution.
- The RC5 speed-ups were played at 1440p before release; the multi-draw change and the out-of-video-memory recovery
  have only passed build checks: see `TESTING.md`.

## RC3

- **Be a Pro crash fixed.** Opening Weekly Growth or Traits crashed the game (`DEVICE_LOST`). 5 of 5 opens crashed
  before the fix, 0 of 13 after it. Cause: a depth target reused at the same address with a wider width kept the
  old, narrower image. It was not an NVIDIA driver bug.
- **Be a Pro images fixed.** The hub headline picture and the player, agent, coach and GM portraits came out as
  coloured noise. The game reads those pictures from memory right after the GPU draws them, and the emulator now
  copies them back in time.


## RC2: changes since duy64 a917829

- Pacing: the game's own flip rate at a 60 Hz vblank is the default, so menus and cut-scenes run at 30 fps and play at
  60 fps. `KYTY_FORCE_FLIP_RATE=0` with `KYTY_MENU_VBLANK_HZ=240` / `KYTY_PLAY_VBLANK_HZ=240` brings back the old
  behaviour.
- At the 30 Hz flip rate, a frame that misses its vblank shows on the next vblank (50 ms) instead of the next even one
  (66 ms). `KYTY_LATE_FLIP=0` restores the old behaviour.
- Built with PGO and AVX2 (clang-cl 19.1.5 from VS 2022 Build Tools). Build line: `tools/pgo/README.md`.
- Frame-pacing log lines: `NHL27FRAMETIME` every 5 s, `NHL27HITCH` for each frame from 40 ms.
- `cfgGlobal.json`: `KYTY_PERF_STATS` is `0`. `KYTY_READBACK_STILLS=0` turns the Be a Pro image fix off.
- Text and overlay keys in the scripted input file, used for automated tests.
- `tools/nhl27`: the start script and settings shipped in this folder.

### Measured on RC2

Ryzen 7 9800X3D, RTX 5080, scripted 15-minute CPU vs CPU Play Now match.

| | a917829 defaults (1 run) | pacing fix (2 runs) | pacing fix + PGO/AVX2 (3 runs) |
|---|---|---|---|
| Play fps (median / p10) | 67 / 54 (uncapped) | 60 / 59-60 | 60 / 60 |
| Play mean 1% low (fps) | - | 44.9-46.9 | 48.7-49.8 |
| Play 5 s windows with no missed frame | - | 58-71% | 75-87% |
| Play frames over 50 ms (15 min) | - | 28-31 | 9-23 |
| Menus / 30 Hz scenes fps (median / p10) | 40 / - (uncapped) | 30 / 29 | 30 / 28 |

These numbers are from RC2. The RC3 fixes were not measured separately. On the RC3 code, a 55-minute Be a Pro run (two
games) had no crash, with play at a median of 60 fps and a 1% low of 52.

