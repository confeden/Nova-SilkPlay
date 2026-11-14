"""Generate small, deterministic CFR test videos for windowed_page.html."""
from __future__ import annotations

import argparse
import pathlib

import cv2
import numpy as np


HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "generated"
WIDTH, HEIGHT, SECONDS = 960, 400, 12
FPS_SET = (24, 30, 60)


def make(fps: int) -> pathlib.Path:
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"cfr_{fps}.webm"
    writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"VP80"), fps,
                             (WIDTH, HEIGHT))
    if not writer.isOpened():
        raise RuntimeError(f"OpenCV could not open VP8 WebM writer: {path}")
    try:
        for frame_no in range(fps * SECONDS):
            frame = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
            frame[:] = (48, 36, 32)
            x = int((frame_no * 240 / fps) % (WIDTH + 120)) - 60
            for i in range(12):
                color = tuple(int(v) for v in cv2.cvtColor(
                    np.uint8([[[i * 15, 179, 191]]]),
                    cv2.COLOR_HSV2BGR)[0, 0])
                y = 28 + i * 28
                cv2.rectangle(frame, (x + i * 3, y), (x + i * 3 + 68, y + 20), color, -1)
            cv2.putText(frame, f"CFR {fps} FPS SOURCE", (20, 382),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 1, cv2.LINE_AA)
            writer.write(frame)
    finally:
        writer.release()

    cap = cv2.VideoCapture(str(path))
    try:
        actual_fps = cap.get(cv2.CAP_PROP_FPS)
        frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    finally:
        cap.release()
    expected = fps * SECONDS
    if frame_count != expected or abs(actual_fps - fps) > 0.01:
        path.unlink(missing_ok=True)
        raise RuntimeError(f"Bad fixture {path}: fps={actual_fps}, frames={frame_count}; expected {fps}/{expected}")
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps", type=int, choices=FPS_SET, action="append")
    args = parser.parse_args()
    for fps in args.fps or FPS_SET:
        path = make(fps)
        print(f"{fps} fps: {path} ({path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
