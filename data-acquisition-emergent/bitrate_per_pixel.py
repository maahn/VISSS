#!/usr/bin/env python3
"""
Compute bits-per-pixel-per-frame for a list of video files, so a target bitrate can be
scaled to a new camera resolution/framerate while preserving the old encode's visual density.

    bits_per_pixel_per_frame = total_bits / (num_frames * width * height)

Usage:
    python3 bitrate_per_pixel.py file1.mkv file2.mkv ...
    python3 bitrate_per_pixel.py /path/to/recordings/*.mkv

To go the other direction (pick a target bitrate for the new camera):
    target_bitrate_bps = bits_per_pixel_per_frame * new_width * new_height * new_fps
"""

import json
import subprocess
import sys
from fractions import Fraction


def probe(path):
    cmd = [
        "ffprobe", "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=width,height,r_frame_rate,nb_frames",
        "-show_entries", "format=duration,size",
        "-of", "json",
        path,
    ]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    data = json.loads(out)
    stream = data["streams"][0]
    fmt = data["format"]

    width = int(stream["width"])
    height = int(stream["height"])
    fps = float(Fraction(stream["r_frame_rate"]))
    duration = float(fmt["duration"])
    size_bytes = int(fmt["size"])

    nb_frames = stream.get("nb_frames")
    if nb_frames in (None, "N/A"):
        # container didn't store a frame count (common for mkv); estimate from duration*fps
        nb_frames = duration * fps
    else:
        nb_frames = int(nb_frames)

    return {
        "path": path,
        "width": width,
        "height": height,
        "fps": fps,
        "duration_s": duration,
        "nb_frames": nb_frames,
        "size_bytes": size_bytes,
    }


def main(paths):
    rows = []
    for path in paths:
        try:
            info = probe(path)
        except (subprocess.CalledProcessError, KeyError, ValueError, ZeroDivisionError) as e:
            print(f"skip {path}: {e}", file=sys.stderr)
            continue

        total_bits = info["size_bytes"] * 8
        pixels_per_frame = info["width"] * info["height"]
        bpppf = total_bits / (info["nb_frames"] * pixels_per_frame)
        bitrate_bps = total_bits / info["duration_s"]

        rows.append({**info, "bitrate_bps": bitrate_bps, "bits_per_pixel_per_frame": bpppf})

    if not rows:
        print("no files could be probed", file=sys.stderr)
        return 1

    header = f"{'file':40s} {'WxH':>10s} {'fps':>7s} {'bitrate_kbps':>13s} {'bits/px/frame':>14s}"
    print(header)
    print("-" * len(header))
    for r in rows:
        print(
            f"{r['path'][-40:]:40s} "
            f"{r['width']}x{r['height']:>4d} "
            f"{r['fps']:7.2f} "
            f"{r['bitrate_bps'] / 1000:13.1f} "
            f"{r['bits_per_pixel_per_frame']:14.5f}"
        )

    avg_bpppf = sum(r["bits_per_pixel_per_frame"] for r in rows) / len(rows)
    print("-" * len(header))
    print(f"average bits_per_pixel_per_frame across {len(rows)} file(s): {avg_bpppf:.5f}")
    print("\nTo size a bitrate for the new camera:")
    print("  target_bitrate_bps = bits_per_pixel_per_frame * new_width * new_height * new_fps")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    sys.exit(main(sys.argv[1:]))
