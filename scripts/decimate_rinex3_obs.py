#!/usr/bin/env python3
"""Decimate a RINEX 3 observation file to a coarser epoch interval.

Why this exists: `processing.gnss_rate_s` is applied only in `Application::run`
(src/core/app/Application.cpp:1723), the PPP / network path. The baseline
modes dispatch earlier, through `Application::runBaseline`, and neither that
file nor `MultiRinexSync` references the knob at all. So a baseline config
carrying `gnss_rate_s: 30.0` is silently inert and the solver sees every
native-rate epoch. For a 1 Hz 12 h session that is 30x the intended work, and,
more seriously, the reported formal sigmas come out far too optimistic, because
consecutive 1 s epochs of a static baseline are strongly time-correlated while
the filter treats them as independent.

Decimating the input is the fix that needs no code change.

Epoch records are kept when the seconds-of-day value is an exact multiple of
--interval (within --tol, to absorb the fractional-second jitter real
receivers write).

Decimating on absolute seconds-of-day, rather than by index from each file's
own start, is load-bearing when the output feeds a multi-station sync. Two
stations that began logging at different times (here 15:04:53 and 15:12:46,
473 s apart) would, under index decimation, land on seconds :23/:53 and
:16/:46 respectively: grids that never intersect, so the baseline would match
zero epochs rather than the expected 1424. Keying on seconds-of-day puts every
station on the same :00/:30 grid by construction.

The header is copied verbatim (every SYS / # / OBS TYPES block, GLONASS slot
record and PRN / # OF OBS line survives) with exactly one exception: INTERVAL
is rewritten to the new rate.

TIME OF FIRST OBS / TIME OF LAST OBS are deliberately not rewritten, so they
may precede or follow the first/last retained epoch by up to one interval
(for example, header 15:04:53 against a first kept epoch of 15:05:00).
Rewriting them would need a second pass, since the header is emitted before
the body is scanned. This is harmless for the consumers here, since the parser
uses them as a date hint rather than an epoch bound, but these two records are
not exact after decimation. The stale PRN / # OF OBS counts are likewise
pre-decimation values.

Usage:
  python scripts/decimate_rinex3_obs.py in.25o out.25o --interval 30
"""
from __future__ import annotations
import argparse
import io
import sys


def parse_epoch_line(line):
    """RINEX 3 epoch line: '> YYYY MM DD HH MM SS.sssssss  F  n ...'. None if not one."""
    if not line.startswith('>'):
        return None
    f = line[1:].split()
    if len(f) < 6:
        return None
    try:
        return (int(f[0]), int(f[1]), int(f[2]), int(f[3]), int(f[4]), float(f[5]))
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--interval', type=float, required=True, help='target epoch interval (s)')
    ap.add_argument('--tol', type=float, default=0.01, help='seconds-of-day match tolerance')
    a = ap.parse_args()

    kept = dropped = 0
    first_epoch = last_epoch = None
    wrote_interval = False

    with io.open(a.src, encoding='utf-8', errors='replace') as fi, \
         io.open(a.dst, 'w', encoding='utf-8', newline='\n') as fo:
        # ---- header: verbatim, except INTERVAL ----
        for line in fi:
            line = line.rstrip('\n').rstrip('\r')
            label = line[60:].strip() if len(line) > 60 else ''
            if label == 'INTERVAL':
                fo.write('%10.3f%50s%s\n' % (a.interval, '', 'INTERVAL'))
                wrote_interval = True
                continue
            fo.write(line + '\n')
            if label == 'END OF HEADER':
                break

        # ---- body: keep whole epoch blocks on the grid ----
        keep = False
        for line in fi:
            line = line.rstrip('\n').rstrip('\r')
            ep = parse_epoch_line(line)
            if ep is not None:
                _, _, _, hh, mm, ss = ep
                sod = hh * 3600 + mm * 60 + ss
                r = sod % a.interval
                keep = (r < a.tol) or (a.interval - r < a.tol)
                if keep:
                    kept += 1
                    if first_epoch is None:
                        first_epoch = ep
                    last_epoch = ep
                else:
                    dropped += 1
            if keep:
                fo.write(line + '\n')

    if not wrote_interval:
        print('WARNING: source had no INTERVAL record; none written', file=sys.stderr)
    print('kept %d epoch(s), dropped %d  (%.1f%% retained)'
          % (kept, dropped, 100.0 * kept / max(1, kept + dropped)))
    if first_epoch:
        print('first kept %04d-%02d-%02d %02d:%02d:%06.3f' % first_epoch)
        print('last  kept %04d-%02d-%02d %02d:%02d:%06.3f' % last_epoch)


if __name__ == '__main__':
    main()
