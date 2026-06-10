#!/usr/bin/env python3
"""Connect to a Septentrio SBF TCP output, capture a few seconds, and report
which SBF block numbers are present (CRC-validated).  Also saves the raw
bytes for deterministic off-receiver replay through GnssProcessor.

Usage:  python scripts/sbf_probe.py [host] [port] [seconds]
Default: 10.214.154.211 28785 4
"""
import collections
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "10.214.154.211"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 28785
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
OUT = "sbf_capture.sbf"

# A few well-known SBF block numbers for a friendly label in the histogram.
NAMES = {
    4027: "MeasEpoch",        # classic measurements (decoder supports this)
    4109: "Meas3Ranges",      # compact 'meas3' measurements (NOT yet supported)
    4110: "Meas3Doppler",
    4111: "Meas3PP",
    4113: "Meas3CN0HiRes",
    5891: "GPSNav", 4002: "GALNav", 4004: "GLONav",
    4081: "BDSNav", 4095: "QZSNav", 5896: "GEONav",
    4007: "PVTGeodetic", 4006: "PVTCartesian", 5914: "ReceiverTime",
    5902: "GPSRawCA", 4017: "GPSRawL2C", 4018: "GPSRawL5",
    4023: "GALRawINAV", 4024: "GALRawFNAV", 4047: "BDSRaw",
}


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def main() -> int:
    print(f"connecting to {HOST}:{PORT} for {DURATION:.0f}s ...")
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
    except OSError as e:
        print(f"CONNECT FAILED: {e}")
        return 1
    s.settimeout(2.0)

    buf = bytearray()
    start = time.time()
    try:
        while time.time() - start < DURATION:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
    finally:
        s.close()

    with open(OUT, "wb") as f:
        f.write(buf)
    print(f"captured {len(buf)} bytes -> {OUT}")
    if not buf:
        print("NO DATA received (port may be send-only-on-trigger, blocked, or wrong port).")
        return 1

    hist = collections.Counter()
    revs = {}
    crc_ok = crc_bad = 0
    i, n = 0, len(buf)
    while i + 8 <= n:
        if buf[i] != 0x24 or buf[i + 1] != 0x40:
            i += 1
            continue
        length = buf[i + 6] | (buf[i + 7] << 8)
        if length < 8 or length % 4 != 0 or i + length > n:
            i += 1
            continue
        block = buf[i:i + length]
        if crc16_xmodem(block[4:]) != (block[2] | (block[3] << 8)):
            crc_bad += 1
            i += 1
            continue
        ident = block[4] | (block[5] << 8)
        bid = ident & 0x1FFF
        hist[bid] += 1
        revs[bid] = ident >> 13
        crc_ok += 1
        i += length

    print(f"\nvalid SBF blocks: {crc_ok}   (crc-mismatch resyncs: {crc_bad})")
    print("block-number histogram:")
    for bid, c in sorted(hist.items(), key=lambda kv: -kv[1]):
        label = NAMES.get(bid, "?")
        print(f"  {bid:>5} rev{revs[bid]}  x{c:<4} {label}")

    have_meas = 4027 in hist
    have_meas3 = any(b in hist for b in (4109, 4110, 4111, 4113))
    print()
    if have_meas:
        print("OK: MeasEpoch (4027) present -> the current decoder can produce observations.")
    elif have_meas3:
        print("NOTE: only Meas3 blocks present (4109/...) -> need a Meas3 decoder, OR enable "
              "'MeasEpoch'/classic 'Rinex' SBF output on the receiver (sso/setSBFOutput).")
    else:
        print("WARNING: no measurement block seen in this window.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
