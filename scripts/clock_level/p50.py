"""Consumer 3D p50 by hour from logs/closed_loop_trace.csv (hour 0 at the first row; hours with
more than 30 rows).
    python scripts/clock_level/p50.py <run> [<run> ...]      (run = dir, or name under runs/)"""
import csv, sys, statistics as st, os
from collections import defaultdict
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
for a in sys.argv[1:]:
    d = a if os.path.isdir(a) else (ROOT + '/runs/' + a)
    by = defaultdict(list); t0 = None
    for r in csv.DictReader(open(d + '/logs/closed_loop_trace.csv')):
        try: t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, KeyError): continue
        if t0 is None: t0 = t
        by[int(round((t - t0) * 1000)) // 3600000].append(e)
    print(f'{os.path.basename(d):22s}', ' '.join(f'{st.median(by[h]):.3f}' for h in sorted(by) if len(by[h]) > 30))
