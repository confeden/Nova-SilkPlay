"""ISO-BMFF MP4 Video Track Probe.

Parses the ISO-BMFF box tree using only Python standard library modules
and outputs video track characteristics (resolution, fps, duration, codec, bit depth).
"""

import argparse
import os
import struct
import sys
from typing import BinaryIO, Optional

# Windows consoles default to cp1252; paths and report text are UTF-8.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def read_box_header(
    f: BinaryIO, max_end: int
) -> Optional[tuple[int, bytes, int, int, Optional[bytes]]]:
    """Reads an ISO-BMFF box header at current file position.

    Returns:
        (box_start, box_type, box_size, header_size, uuid_bytes) or None if EOF.
    """
    box_start = f.tell()
    if box_start + 8 > max_end:
        return None

    header = f.read(8)
    if len(header) < 8:
        return None

    size, box_type = struct.unpack(">I4s", header)
    header_size = 8

    if size == 1:
        large_header = f.read(8)
        if len(large_header) < 8:
            raise ValueError(f"Truncated 64-bit largesize for box {box_type!r} at offset {box_start}")
        size = struct.unpack(">Q", large_header)[0]
        header_size = 16
    elif size == 0:
        size = max_end - box_start

    uuid_bytes: Optional[bytes] = None
    if box_type == b"uuid":
        uuid_bytes = f.read(16)
        if len(uuid_bytes) < 16:
            raise ValueError(f"Truncated uuid header for box at offset {box_start}")
        header_size += 16

    if size < header_size:
        raise ValueError(f"Invalid box size {size} < header size {header_size} for box {box_type!r}")

    return box_start, box_type, size, header_size, uuid_bytes


def derive_bit_depth_from_sample_entry(
    f: BinaryIO, entry_start: int, entry_size: int, codec_fourcc: str
) -> Optional[int]:
    """Attempts to derive video bit depth from codec configuration boxes or visual sample entry fields."""
    fallback_depth: Optional[int] = None
    if entry_size >= 86:
        f.seek(entry_start + 82)
        depth_data = f.read(2)
        if len(depth_data) == 2:
            raw_depth = struct.unpack(">H", depth_data)[0]
            if raw_depth in (24, 0x0018):
                fallback_depth = 8
            elif raw_depth == 30:
                fallback_depth = 10
            elif raw_depth == 36:
                fallback_depth = 12
            elif raw_depth == 48:
                fallback_depth = 16

    sub_pos = entry_start + 86
    sub_end = entry_start + entry_size

    while sub_pos < sub_end:
        f.seek(sub_pos)
        header_info = read_box_header(f, sub_end)
        if header_info is None:
            break
        b_start, b_type, b_size, b_hdr_size, _ = header_info

        payload_len = b_size - b_hdr_size
        payload = f.read(payload_len)

        if b_type == b"av1C" and len(payload) >= 3:
            high_bitdepth = (payload[2] >> 6) & 1
            twelve_bit = (payload[2] >> 5) & 1
            if twelve_bit:
                return 12
            if high_bitdepth:
                return 10
            return 8

        if b_type == b"hvcC" and len(payload) >= 18:
            return (payload[17] & 0x07) + 8

        if b_type == b"vpcC" and len(payload) >= 3:
            return (payload[2] >> 4) & 0x0F

        if b_type == b"avcC" and len(payload) >= 6:
            profile = payload[1]
            if profile in (100, 110, 122, 244):
                num_sps = payload[5] & 0x1F
                idx = 6
                sps_valid = True
                for _ in range(num_sps):
                    if idx + 2 > len(payload):
                        sps_valid = False
                        break
                    sps_len = struct.unpack(">H", payload[idx : idx + 2])[0]
                    idx += 2 + sps_len

                if sps_valid and idx < len(payload):
                    num_pps = payload[idx]
                    idx += 1
                    for _ in range(num_pps):
                        if idx + 2 > len(payload):
                            sps_valid = False
                            break
                        pps_len = struct.unpack(">H", payload[idx : idx + 2])[0]
                        idx += 2 + pps_len

                if sps_valid and idx + 2 <= len(payload):
                    return (payload[idx + 1] & 0x07) + 8
                return 8
            return 8

        sub_pos = b_start + b_size

    return fallback_depth


def probe_mp4_file(file_path: str) -> None:
    """Parses an ISO-BMFF file and prints one line per video track."""
    if not os.path.isfile(file_path):
        sys.stderr.write(f"Error: File not found: {file_path}\n")
        sys.exit(1)

    file_size = os.path.getsize(file_path)
    if file_size < 8:
        sys.stderr.write(f"Error: '{file_path}' is not a valid MP4/ISO-BMFF file (too small).\n")
        sys.exit(1)

    with open(file_path, "rb") as f:
        first_box = read_box_header(f, file_size)
        if first_box is None:
            sys.stderr.write(f"Error: Failed to read initial box in '{file_path}'.\n")
            sys.exit(1)

        first_type = first_box[1]
        valid_initial_brands = (b"ftyp", b"moov", b"styp", b"free", b"pdin")
        if first_type not in valid_initial_brands:
            sys.stderr.write(
                f"Error: '{file_path}' is not a valid MP4/ISO-BMFF file (initial box is {first_type!r}).\n"
            )
            sys.exit(1)

        moov_start = -1
        moov_size = -1
        pos = 0
        while pos < file_size:
            f.seek(pos)
            info = read_box_header(f, file_size)
            if info is None:
                break
            b_start, b_type, b_size, _, _ = info
            if b_type == b"moov":
                moov_start = b_start
                moov_size = b_size
                break
            pos = b_start + b_size

        if moov_start < 0:
            sys.stderr.write(f"Error: No 'moov' box found in '{file_path}'.\n")
            sys.exit(1)

        moov_end = moov_start + moov_size
        f.seek(moov_start)
        moov_hdr = read_box_header(f, moov_end)
        if moov_hdr is None:
            sys.stderr.write(f"Error: Corrupt 'moov' box in '{file_path}'.\n")
            sys.exit(1)

        pos = moov_start + moov_hdr[3]
        while pos < moov_end:
            f.seek(pos)
            info = read_box_header(f, moov_end)
            if info is None:
                break
            b_start, b_type, b_size, b_hdr_size, _ = info

            if b_type == b"trak":
                parse_trak(f, b_start, b_size, b_hdr_size, file_path)

            pos = b_start + b_size


def parse_trak(
    f: BinaryIO, trak_start: int, trak_size: int, trak_hdr_size: int, file_path: str
) -> None:
    """Parses a trak box and prints its video info if it is a video track."""
    trak_end = trak_start + trak_size
    pos = trak_start + trak_hdr_size

    track_disp_w = 0.0
    track_disp_h = 0.0

    mdia_start = -1
    mdia_size = -1
    mdia_hdr_size = 0

    while pos < trak_end:
        f.seek(pos)
        info = read_box_header(f, trak_end)
        if info is None:
            break
        b_start, b_type, b_size, b_hdr_size, _ = info

        if b_type == b"tkhd":
            f.seek(b_start + b_hdr_size)
            tkhd_data = f.read(b_size - b_hdr_size)
            if len(tkhd_data) >= 84:
                version = tkhd_data[0]
                if version == 0:
                    w_raw, h_raw = struct.unpack(">II", tkhd_data[76:84])
                    track_disp_w = w_raw / 65536.0
                    track_disp_h = h_raw / 65536.0
                elif version == 1 and len(tkhd_data) >= 96:
                    w_raw, h_raw = struct.unpack(">II", tkhd_data[88:96])
                    track_disp_w = w_raw / 65536.0
                    track_disp_h = h_raw / 65536.0

        elif b_type == b"mdia":
            mdia_start = b_start
            mdia_size = b_size
            mdia_hdr_size = b_hdr_size

        pos = b_start + b_size

    if mdia_start < 0:
        return

    mdia_end = mdia_start + mdia_size
    pos = mdia_start + mdia_hdr_size

    handler_type: bytes = b""
    timescale = 0
    duration = 0

    minf_start = -1
    minf_size = -1
    minf_hdr_size = 0

    while pos < mdia_end:
        f.seek(pos)
        info = read_box_header(f, mdia_end)
        if info is None:
            break
        b_start, b_type, b_size, b_hdr_size, _ = info

        if b_type == b"hdlr":
            f.seek(b_start + b_hdr_size)
            hdlr_data = f.read(b_size - b_hdr_size)
            if len(hdlr_data) >= 12:
                handler_type = hdlr_data[8:12]

        elif b_type == b"mdhd":
            f.seek(b_start + b_hdr_size)
            mdhd_data = f.read(b_size - b_hdr_size)
            if len(mdhd_data) >= 20:
                version = mdhd_data[0]
                if version == 0:
                    timescale, duration = struct.unpack(">II", mdhd_data[12:20])
                elif version == 1 and len(mdhd_data) >= 28:
                    timescale = struct.unpack(">I", mdhd_data[20:24])[0]
                    duration = struct.unpack(">Q", mdhd_data[24:32])[0]

        elif b_type == b"minf":
            minf_start = b_start
            minf_size = b_size
            minf_hdr_size = b_hdr_size

        pos = b_start + b_size

    if handler_type != b"vide":
        return

    duration_s = (duration / timescale) if timescale > 0 else 0.0

    if minf_start < 0:
        return

    minf_end = minf_start + minf_size
    pos = minf_start + minf_hdr_size

    stbl_start = -1
    stbl_size = -1
    stbl_hdr_size = 0

    while pos < minf_end:
        f.seek(pos)
        info = read_box_header(f, minf_end)
        if info is None:
            break
        b_start, b_type, b_size, b_hdr_size, _ = info

        if b_type == b"stbl":
            stbl_start = b_start
            stbl_size = b_size
            stbl_hdr_size = b_hdr_size
            break

        pos = b_start + b_size

    if stbl_start < 0:
        return

    stbl_end = stbl_start + stbl_size
    pos = stbl_start + stbl_hdr_size

    codec_fourcc = "-"
    coded_w = 0
    coded_h = 0
    bit_depth: Optional[int] = None

    sample_count = 0
    stts_entry_count = 0
    stts_first_delta = 0

    while pos < stbl_end:
        f.seek(pos)
        info = read_box_header(f, stbl_end)
        if info is None:
            break
        b_start, b_type, b_size, b_hdr_size, _ = info

        if b_type == b"stsd":
            f.seek(b_start + b_hdr_size)
            stsd_header = f.read(8)
            if len(stsd_header) >= 8:
                entry_count = struct.unpack(">I", stsd_header[4:8])[0]
                if entry_count > 0:
                    entry_start = b_start + b_hdr_size + 8
                    f.seek(entry_start)
                    e_hdr = read_box_header(f, b_start + b_size)
                    if e_hdr is not None:
                        e_start, e_fourcc, e_size, e_hdr_size, _ = e_hdr
                        codec_fourcc = e_fourcc.decode("latin1", "replace")
                        f.seek(e_start + e_hdr_size)
                        sample_entry_payload = f.read(min(e_size - e_hdr_size, 128))
                        if len(sample_entry_payload) >= 28:
                            coded_w, coded_h = struct.unpack(">HH", sample_entry_payload[24:28])

                        bit_depth = derive_bit_depth_from_sample_entry(f, e_start, e_size, codec_fourcc)

        elif b_type == b"stsz":
            f.seek(b_start + b_hdr_size)
            stsz_data = f.read(8)
            if len(stsz_data) >= 8:
                count_data = f.read(4)
                if len(count_data) == 4:
                    sample_count = struct.unpack(">I", count_data)[0]

        elif b_type == b"stts":
            f.seek(b_start + b_hdr_size)
            stts_data = f.read(8)
            if len(stts_data) >= 8:
                stts_entry_count = struct.unpack(">I", stts_data[4:8])[0]
                if stts_entry_count >= 1:
                    first_entry = f.read(8)
                    if len(first_entry) == 8:
                        _, stts_first_delta = struct.unpack(">II", first_entry)

        pos = b_start + b_size

    final_w = coded_w if coded_w > 0 else int(round(track_disp_w))
    final_h = coded_h if coded_h > 0 else int(round(track_disp_h))
    res_str = f"{final_w}x{final_h}"

    fps_avg = (sample_count / duration_s) if duration_s > 0 else 0.0
    if stts_entry_count == 1 and stts_first_delta > 0:
        fps_nominal = timescale / stts_first_delta
        fps_str = f"fps_avg={fps_avg:.2f} fps_nominal={fps_nominal:.2f}"
    else:
        fps_str = f"fps_avg={fps_avg:.2f}"

    duration_str = f"{duration_s:.2f}"
    bit_depth_str = str(bit_depth) if bit_depth is not None else "-"

    print(f"{file_path} | {res_str} | {fps_str} | {duration_str} | {codec_fourcc} | {bit_depth_str}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Probe ISO-BMFF MP4 video track characteristics using Python standard library."
    )
    parser.add_argument("files", nargs="+", help="One or more MP4/ISO-BMFF video files to probe")
    args = parser.parse_args()

    for file_path in args.files:
        probe_mp4_file(file_path)


if __name__ == "__main__":
    main()
