#!/usr/bin/env python3
"""Generate NTFS MFT record fixtures for unit tests (512-byte records)."""

import struct
from pathlib import Path

OUT = Path(__file__).resolve().parent / "mft"
OUT.mkdir(parents=True, exist_ok=True)


def u16(x):
    return struct.pack("<H", x)


def u32(x):
    return struct.pack("<I", x)


def u64(x):
    return struct.pack("<Q", x)


def build_fixup_valid():
    rec = bytearray(512)
    rec[0:4] = b"FILE"
    rec[4:6] = u16(0x30)  # usa offset
    rec[6:8] = u16(3)  # usa count (2 + sectors)
    rec[0x14:0x16] = u16(0x38)  # attr offset
    rec[0x18:0x1C] = u32(0x100)  # bytes used
    # USA at 0x30: update sequence 0x1234, then sector end words
    rec[0x30:0x32] = u16(0x1234)
    rec[0x32:0x34] = u16(0xAAAA)  # sector 1 tail (before fixup)
    rec[0x34:0x36] = u16(0xBBBB)  # sector 2 tail placeholder
    # After fixup, tails restored from USA
    return bytes(rec)


def build_fixup_invalid_usa():
    rec = bytearray(build_fixup_valid())
    rec[6:8] = u16(0)  # invalid usa count
    return bytes(rec)


def build_resident_filename():
    rec = bytearray(512)
    rec[0:4] = b"FILE"
    rec[4:6] = u16(0x30)
    rec[6:8] = u16(3)
    rec[0x30:0x32] = u16(0x5678)
    rec[0x32:0x34] = u16(0x1111)
    rec[0x34:0x36] = u16(0x2222)
    rec[0x14:0x16] = u16(0x38)
    rec[0x18:0x1C] = u32(0x120)

    off = 0x38
    # $FILE_NAME resident attribute
    rec[off : off + 4] = u32(0x30)
    alen = 0x68
    rec[off + 4 : off + 8] = u32(alen)
    rec[off + 8] = 0  # resident
    rec[off + 9] = 0  # unnamed
    rec[off + 0x10 : off + 0x14] = u32(0x4A)  # value length (>= 0x42 + 4 wchar)
    rec[off + 0x14 : off + 0x16] = u16(0x18)  # value offset

    val = off + 0x18
    rec[val : val + 8] = u64(5)  # parent mft ref
    rec[val + 0x30 : val + 0x38] = u64(1024)  # real size
    rec[val + 0x40] = 4  # name length (wchar)
    rec[val + 0x41] = 1  # namespace posix
    rec[val + 0x42 : val + 0x4A] = "test".encode("utf-16le")

    off2 = off + alen
    rec[off2 : off2 + 4] = u32(0xFFFFFFFF)
    rec[off2 + 4 : off2 + 8] = u32(0)
    return bytes(rec)


def main():
    (OUT / "record_fixup_valid.bin").write_bytes(build_fixup_valid())
    (OUT / "record_fixup_invalid_usa.bin").write_bytes(build_fixup_invalid_usa())
    (OUT / "record_resident_filename.bin").write_bytes(build_resident_filename())
    print("Wrote fixtures to", OUT)


if __name__ == "__main__":
    main()
