#!/usr/bin/env python3
"""Census attribution between two keys and their pair (card ZTL section 9).

usage: ztl_census_attrib.py <ref key> <link key> <colour key> <pair key> [--day D] [--obs-root R]

Reads the 16-site census replays of scripts/clock_level/gcensus.py (runs `_rap_<key>_<site4>` under
RAPID_RUNS_ROOT) and prints, against <ref key>:
  - the median site p50 by hour of every key (moves with the site ranks);
  - per key, the paired median of the site changes by hour, the hours 2 to 6 mean change, its worst site
    and the count of sites beyond 1 cm either way;
  - per site, the hour 2, hour 3 and hours 2 to 6 changes of the three keys and the interaction
    pair - link - colour over hours 2 to 6 (zero when the two keys add).
Hours count from 00:00 of the day (gcensus.py --day / --obs-root, the same options).
"""
import os
import statistics as st
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clock_level'))
import gcensus as g  # noqa: E402  (owner of the census replays and their hourly p50)


def main(argv):
    argv = g.take_options(argv)
    if len(argv) != 4:
        sys.exit(__doc__)
    ref, link, col, pair = argv
    keys = {'link': link, 'colour': col, 'pair': pair}
    R = {n: g.hours(f'{ref}_{n}') for n in g.NAMES}
    K = {lab: {n: g.hours(f'{k}_{n}') for n in g.NAMES} for lab, k in keys.items()}
    sites = [n for n in g.NAMES[1:] if R[n] and all(K[lab][n] for lab in K)]
    nh = len(R[sites[0]])
    print(f'{len(sites)} census sites with every key; day {g.DAY}, hours from {g.START_H:g} h')
    print('median site p50 by hour (m)  ref    ' + ' '.join(f'{st.median([R[n][h] for n in sites]):.4f}'
                                                       for h in range(nh)))
    for lab in K:
        print(f'median site p50 by hour (m)  {lab:6s} '
              + ' '.join(f'{st.median([K[lab][n][h] for n in sites]):.4f}' for h in range(nh)))
    print('\npaired median change by hour (mm); hours 2 to 6 mean: median, worst site, sites beyond 1 cm')
    for lab in K:
        dh = [st.median([K[lab][n][h] - R[n][h] for n in sites]) * 1e3 for h in range(nh)]
        d26 = {n: (st.mean(K[lab][n][1:]) - st.mean(R[n][1:])) * 1e3 for n in sites}
        worst = max(d26, key=d26.get)
        print(f'  {lab:6s} ' + ' '.join(f'{x:+5.1f}' for x in dh)
              + f' | median {st.median(d26.values()):+.1f}, worst {worst.upper()} {d26[worst]:+.1f},'
              + f' over +10 {sum(1 for v in d26.values() if v > 10)}, under -10 {sum(1 for v in d26.values() if v < -10)}')
    print('\nsite   hour 2 change (mm)     hour 3 change (mm)     hours 2-6 change (mm)  interaction')
    print('       link colour  pair    link colour  pair    link colour  pair    2-6 (mm)')
    for n in sites:
        def d(lab, h):
            return (K[lab][n][h] - R[n][h]) * 1e3

        def m(lab):
            return (st.mean(K[lab][n][1:]) - st.mean(R[n][1:])) * 1e3
        print(f'{n.upper():5s} ' + ' '.join(f'{d(lab, 1):+6.1f}' for lab in K) + '  '
              + ' '.join(f'{d(lab, 2):+6.1f}' for lab in K) + '  '
              + ' '.join(f'{m(lab):+6.1f}' for lab in K) + f'  {m("pair") - m("link") - m("colour"):+6.1f}')


if __name__ == '__main__':
    main(sys.argv[1:])
