#!/usr/bin/env python3
"""Dump the raw structure of the first SBF RawNav block of each requested
block number, to ground the container-layout spec against real bytes.

Usage:  python scripts/sbf_rawnav_dump.py [capture.sbf] [blocknum ...]
Default: sbf_capture.sbf 4017 4023 4047 4026
"""
import struct
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else "sbf_capture.sbf"
WANT = [int(x) for x in sys.argv[2:]] or [4017, 4023, 4047, 4026]


def crc16(d: bytes) -> int:
    c = 0
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


data = open(PATH, "rb").read()
seen = {}
i, n = 0, len(data)
while i + 8 <= n:
    if data[i] != 0x24 or data[i + 1] != 0x40:
        i += 1
        continue
    length = data[i + 6] | (data[i + 7] << 8)
    if length < 8 or length % 4 or i + length > n:
        i += 1
        continue
    block = data[i:i + length]
    if crc16(block[4:]) != (block[2] | (block[3] << 8)):
        i += 1
        continue
    bid = (block[4] | (block[5] << 8)) & 0x1FFF
    if bid in WANT and bid not in seen:
        seen[bid] = block
    i += length

for bid in WANT:
    if bid not in seen:
        print(f"\nblock {bid}: NOT FOUND in {PATH}")
        continue
    b = seen[bid]
    tow = struct.unpack_from("<I", b, 8)[0]
    wnc = struct.unpack_from("<H", b, 12)[0]
    body = b[14:]   # bytes after the standard TOW/WNc time stamp
    print(f"\n=== block {bid}  total_len={len(b)}  TOW={tow} WNc={wnc}  body_len={len(body)} ===")
    print("  body u8 [0:12]:", " ".join(f"{x:02x}" for x in body[:12]))
    print("  body hex:", body.hex())
    # Try interpreting the navigation words (u32) at a couple of plausible offsets.
    for off in (6, 8):
        rem = len(body) - off
        if rem >= 4:
            nwords = rem // 4
            words = [struct.unpack_from("<I", body, off + 4 * k)[0] for k in range(nwords)]
            print(f"  NAVBits @body+{off}  ({nwords} u32): " + " ".join(f"{w:08x}" for w in words))
