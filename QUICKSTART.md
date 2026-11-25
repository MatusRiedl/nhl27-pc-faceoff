# Quick start

## 1. Install

1. Download `Faceoff-v0.40.0.zip` from the [release page](../../releases/latest).
2. Extract it anywhere you can write to, with at least 2 GB free. Not inside `Program Files`.
3. Start `Faceoff.exe`. If Windows SmartScreen warns, choose **More info**, then **Run anyway**. If Faceoff asks for
   Microsoft's WebView2 Runtime, install it and start Faceoff again.

**Coming from RC3, RC4 or RC5:** extract the zip into your old folder and let it overwrite. Faceoff moves the old
build into `builds/` on the first start, and your saves, shader cache and game folder stay. To keep saves from another
Kyty install, copy `_SaveData`, `_DownloadData` and `_Trophies` into the Faceoff folder.

## 2. Point it at the game

Press **Browse** on the Play tab and pick your NHL 27 folder, the one that holds `eboot.bin` (PPSA34063, game version
01.003.001). Faceoff shows the title and version with a check mark when it's right.

## 3. Pick a build and settings

- **Emulator build:** v0.40.0 is the default, and every update selects the new build. The dropdown also lists the
  builds already on your PC, so you can switch back.
- **Graphics tab:** 1440p windowed is the default. The upscaler is the game's own FSR 2 at Quality (it renders 67%
  of your resolution). DLSS (GeForce RTX cards) and XeSS (most cards) are the other choices. A lower resolution or a
  lower quality is faster. 4K Quality renders 2560×1440 and needs a strong card; Balanced or Performance is lighter.
- **Frame generation (experimental):** on a GeForce RTX 40 or 50 series card, pick DLSS, then turn on **Frame
  generation**. About twice as many frames reach the screen; input lag goes up a little. It needs
  Hardware-accelerated GPU scheduling on in Windows (Settings → Display → Graphics). **Generated frames** picks
  2x, 3x or 4x (3x and 4x need an RTX 50 series card).
- **DLSS 5 (experimental):** on any GeForce RTX card, turn on **DLSS 5**. Faceoff downloads NVIDIA's DLSS 5 file
  (276 MB) the first time. It costs about 3 ms a frame and about 1.3 GB more RAM.
- **Ultrawide or 4:3 screen:** the picture keeps 16:9 with black bars. **Aspect ratio → Stretch** fills the screen
  instead.

## 4. Play

Plug in a controller first, then press **Play NHL 27**. The first arena load stutters for 30 s or longer while
shaders compile; after that the shader cache is warm. Close the game window when you're done.

Keyboard: J = Cross, L = Circle, I = Triangle, K = Square, arrow keys = D-pad, WASD = left stick, Enter = Options,
Q / E = L1 / R1. To change any key or controller button, open Faceoff's **Controls** tab: click a box, then press
the key or button you want.

## If something goes wrong

- **The game won't start and a window names `MSVCP140.dll` or `VCRUNTIME140.dll`:** install the latest Microsoft
  Visual C++ Redistributable (x64).
- **The game closes in a long match, or Faceoff's This PC card says the memory room is short:** set the paging
  file to "System managed" and close other large apps. A match needs about 28 GB of commit room.
- **Nothing happens on a CPU without AVX2:** the build needs AVX2 (Intel Haswell / AMD Zen or newer).
- **`could not reserve 13824 MB for guest direct memory`:** you started again too soon after closing. Wait a few
  seconds.
- **A crash:** Faceoff shows the play log. Press **Copy log**, or **Logs** to open its folder, and send it with your
  GPU and driver version on [Discord](https://gg.puckpeak.com). For a silent exit also send the Windows Event Viewer
  entry (Windows Logs > Application, `kyty_emulator.exe`).
