<h1 align="center">
  <img src="assets/logo.png" width="48" height="48" alt="" valign="middle"> Nova SilkPlay
</h1>

<p align="center">Smoother video in Google Chrome, powered by GPU frame generation.</p>
<p align="center">
  <a href="https://github.com/confeden/Nova-SilkPlay/releases"><strong>Download for Windows</strong></a>
  · <a href="https://github.com/confeden/Nova-SilkPlay/issues">Report an issue</a>
  · <a href="LICENSE">GPL-3.0</a>
</p>

Nova SilkPlay adds intermediate frames to video playing in Chrome and automatically targets your monitor's refresh rate. It runs in the notification area, supports fullscreen and windowed video, and needs no browser extension.

## Requirements

- Windows 10 2004 or newer / Windows 11, 64-bit.
- Google Chrome and an NVIDIA GPU with hardware Optical Flow support. Tested on RTX 5060 Ti; other GPUs are unverified.

## Quick start

1. Open [Releases](https://github.com/confeden/Nova-SilkPlay/releases) and download `Nova.SilkPlay-0.9.0-setup.exe`.
2. Run the installer, then launch **Nova SilkPlay** from the Start menu.
3. Play a video in Chrome with sound enabled. Keep the video visible; fullscreen is the easiest way to start. Frame generation starts automatically when a suitable video is detected.
4. Click the **pink ribbon** in the notification area (check the **^** menu if hidden) to open Settings. Enable **Google Chrome** and, if desired, **Frame rate counter** or **Start with Windows**. Settings are saved automatically.

| Shortcut | Action |
| --- | --- |
| `Ctrl+Alt+Q` | Turn frame generation on or off |
| `Ctrl+Alt+X` | Quit Nova SilkPlay |

You can also right-click the tray icon to toggle generation or quit. To update, run the newer installer; to remove the app, use Windows **Settings → Apps**.

## If video is not smoother

Check that Chrome is enabled in Settings and generation is on. Start playback with sound, uncover the video, or try fullscreen. Hover over the tray icon to see the current status. Generation pauses when a window covers the video or a game is detected.

Version **0.9.0** supports **Chrome only**; PotPlayer is planned. DRM-protected video is unsupported. Some scenes may show interpolation artifacts, and sustained 165 Hz smoothness is still being validated.

The installer uses the author's self-signed certificate with an online timestamp. Windows may still show a publisher or SmartScreen warning.

## License

[GPL-3.0-only](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md) and [developer documentation](prototype/README.md).
