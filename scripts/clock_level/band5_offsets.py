"""Card L5R P3 (python scripts/clock_level/band5_offsets.py <run dir> [...]): per station, mean code post-fit residual over hours 2-6 by band, with a standard error
from 10-min block means (residuals are time-correlated); count stations with |mean| > 3 SE."""
import sys, numpy as np, pandas as pd
for run in sys.argv[1:]:
    df = pd.read_csv(run + '/logs/network_pf_rows.csv', usecols=['tow', 'station', 'sat', 'signal', 'kind', 'resid_m', 'w'],
                     dtype={'station': 'category', 'sat': 'category', 'signal': 'category', 'kind': 'category'})
    df = df[(df['kind'] == 'code') & (df['sat'].astype(str).str[0] == 'G') & (df['w'] >= 1e-2)]
    t0 = df['tow'].min(); df = df[df['tow'] >= t0 + 3600]
    df['band'] = df['signal'].astype(str).str[1]
    df['blk'] = ((df['tow'] - t0) // 600).astype(int)
    print(f'== {run}')
    for b, g in df.groupby('band'):
        bm = g.groupby(['station', 'blk'], observed=True)['resid_m'].mean()
        st = bm.groupby(level=0, observed=True).agg(['mean', 'std', 'count'])
        st = st[st['count'] >= 6]
        se = st['std'] / np.sqrt(st['count'])
        z = st['mean'] / se
        n3 = int((z.abs() > 3).sum())
        print(f'  band {b}: {len(st)} stations, |mean| p50 {st["mean"].abs().median():.3f} m p90 {st["mean"].abs().quantile(.9):.3f} m, '
              f'stations |mean|>3SE {n3}, |mean|>0.10 m {int((st["mean"].abs() > 0.10).sum())}')
        if b == '5':
            top = st['mean'].abs().sort_values(ascending=False).head(6)
            print('    largest: ' + ', '.join(f'{s} {st.loc[s, "mean"]:+.3f}' for s in top.index))
