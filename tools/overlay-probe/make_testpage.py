"""Generate self-contained HTML test page for video compositing measurement.

Creates an HTML test page with autoplaying video, sub-pixel checkerboard overlay,
HUD, and JavaScript measurement probe hooks.
"""

import argparse
import os
from pathlib import Path
import sys
import urllib.parse

# Windows consoles default to cp1252; paths and report text are UTF-8.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Compositing Probe Testpage</title>
<style>
* {{
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}}

html, body {{
  width: 100vw;
  height: 100vh;
  background-color: #000000;
  overflow: hidden;
  display: flex;
  align-items: center;
  justify-content: center;
}}

.video-box {{
  position: fixed;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: min({width}px, 100vw);
  height: min({height}px, 100vh);
  display: flex;
  align-items: center;
  justify-content: center;
}}

.video-box video {{
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
}}

/* 1px magenta checkerboard border drawn INSIDE the video box edges */
.video-box::after {{
  content: "";
  position: absolute;
  inset: 0;
  box-sizing: border-box;
  padding: 1px;
  pointer-events: none;
  z-index: 1000;
  background-image: repeating-conic-gradient(#ff00ff 0% 25%, #000000 0% 50%);
  background-size: 2px 2px;
  -webkit-mask: linear-gradient(#fff 0 0) content-box, linear-gradient(#fff 0 0);
  -webkit-mask-composite: xor;
  mask: linear-gradient(#fff 0 0) content-box, linear-gradient(#fff 0 0);
  mask-composite: exclude;
}}

#hud {{
  position: fixed;
  top: 12px;
  left: 12px;
  z-index: 10000;
  pointer-events: none;
  font-family: Consolas, "Lucida Console", Monaco, monospace;
  font-size: 13px;
  line-height: 1.4;
  color: #00ff66;
  background: rgba(0, 0, 0, 0.8);
  padding: 6px 12px;
  border: 1px solid rgba(0, 255, 102, 0.35);
  border-radius: 4px;
  user-select: none;
}}
</style>
</head>
<body>
<div id="hud">Initializing HUD...</div>
<div class="video-box" id="video-box">
  <video
    id="probe-video"
    src="{video_url}"
    autoplay
    muted
    loop
    playsinline
  ></video>
</div>

<script>
(() => {{
  const video = document.getElementById("probe-video");
  const hud = document.getElementById("hud");

  // Install window.__probe
  window.__probe = {{
    raf: 0,
    rvfc: 0,
    startedAt: performance.now(),
    lastVfc: null
  }};

  // Rescheduling requestAnimationFrame loop
  function rafLoop() {{
    window.__probe.raf++;
    requestAnimationFrame(rafLoop);
  }}
  requestAnimationFrame(rafLoop);

  // Rescheduling requestVideoFrameCallback loop if available
  if (typeof HTMLVideoElement.prototype.requestVideoFrameCallback === "function") {{
    function rvfcLoop(now, metadata) {{
      window.__probe.rvfc++;
      window.__probe.lastVfc = metadata;
      video.requestVideoFrameCallback(rvfcLoop);
    }}
    video.requestVideoFrameCallback(rvfcLoop);
  }}

  // Probe snapshot function
  window.__probeSnapshot = () => {{
    const now = performance.now();
    const elapsed = now - window.__probe.startedAt;
    const quality = (typeof video.getVideoPlaybackQuality === "function")
      ? video.getVideoPlaybackQuality()
      : null;

    return {{
      raf: window.__probe.raf,
      rvfc: window.__probe.rvfc,
      elapsedMs: elapsed,
      elapsed_ms: elapsed,
      totalVideoFrames: quality ? quality.totalVideoFrames : 0,
      droppedVideoFrames: quality ? quality.droppedVideoFrames : 0,
      videoWidth: video.videoWidth,
      videoHeight: video.videoHeight,
      currentTime: video.currentTime,
      visibilityState: document.visibilityState,
      lastVfc: window.__probe.lastVfc
    }};
  }};

  // 1-second HUD updates
  let prevRaf = window.__probe.raf;
  let prevRvfc = window.__probe.rvfc;
  let prevTime = performance.now();

  function updateHud() {{
    const now = performance.now();
    const dt = (now - prevTime) / 1000.0;
    const curRaf = window.__probe.raf;
    const curRvfc = window.__probe.rvfc;

    const rafFps = dt > 0 ? ((curRaf - prevRaf) / dt).toFixed(1) : "0.0";
    const rvfcFps = dt > 0 ? ((curRvfc - prevRvfc) / dt).toFixed(1) : "0.0";

    prevRaf = curRaf;
    prevRvfc = curRvfc;
    prevTime = now;

    const quality = (typeof video.getVideoPlaybackQuality === "function")
      ? video.getVideoPlaybackQuality()
      : null;
    const dropped = quality ? quality.droppedVideoFrames : 0;

    hud.textContent = `RAF/s: ${{rafFps}} | RVFC/s: ${{rvfcFps}} | Dropped: ${{dropped}} | ${{video.videoWidth}}x${{video.videoHeight}}`;
  }}

  setInterval(updateHud, 1000);

  // Proactively start playback
  video.play().catch(() => {{}});
}})();
</script>
</body>
</html>
"""


def generate_testpage(
    video_path: str, out_path: str, width: int = 2560, height: int = 1440
) -> str:
    """Generates the testpage HTML file and returns its file:// URI."""
    abs_video = Path(video_path).resolve()
    if not abs_video.is_file():
        sys.stderr.write(f"Error: Video file not found: {abs_video}\n")
        sys.exit(1)

    # Convert to properly percent-encoded file:// URL
    video_url = abs_video.as_uri()

    html_content = HTML_TEMPLATE.format(
        width=width,
        height=height,
        video_url=video_url,
    )

    abs_out = Path(out_path).resolve()
    abs_out.parent.mkdir(parents=True, exist_ok=True)
    abs_out.write_text(html_content, encoding="utf-8")

    return abs_out.as_uri()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate self-contained HTML test page for compositing measurements."
    )
    parser.add_argument("--video", required=True, help="Path to local MP4 video file")
    parser.add_argument(
        "--out", default="testpage.html", help="Output HTML file path (default: testpage.html)"
    )
    parser.add_argument(
        "--width", type=int, default=2560, help="Video container width in CSS pixels (default: 2560)"
    )
    parser.add_argument(
        "--height",
        type=int,
        default=1440,
        help="Video container height in CSS pixels (default: 1440)",
    )

    args = parser.parse_args()

    out_uri = generate_testpage(
        video_path=args.video,
        out_path=args.out,
        width=args.width,
        height=args.height,
    )

    print(out_uri)


if __name__ == "__main__":
    main()
