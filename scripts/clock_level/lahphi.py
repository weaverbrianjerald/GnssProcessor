"""Preview of the frame-consistent smoothed member: LAH's wire with P = P_LAH(t) - Phi(t) on every
(epoch, key) where the SMH arm's clone is live (its trace), everything else LAH's.
    python scripts/clock_level/lahphi.py <lah_run_dir> <smh_run_dir> <tpl_rpl_dir> <out_dir>"""
import csv, json, os, sys
lah, smh, tpl, out = sys.argv[1:5]
phi = {}
for r in csv.DictReader(open(smh + '/logs/network_pb_hold_clone.csv')):
    if r['phi_m']:
        phi[(round(float(r['tow']), 1), r['sat'], r['sig'])] = float(r['phi_m'])
os.makedirs(out + '/logs', exist_ok=True)
n = 0; tow = None
with open(out + '/products.txt', 'w', encoding='utf-8', newline='\n') as fo:
    for ln in open(lah + '/products.txt', encoding='utf-8'):
        if ln[0] == 'E': tow = round(float(ln.split(',')[2]), 1)
        if ln[0] == 'P' and tow is not None:
            f = ln.rstrip('\n').split(',')
            k = (tow, f[1], f[2])
            if k in phi:
                f[3] = repr(float(f[3]) - phi[k]); n += 1
                ln = ','.join(f) + '\n'
        fo.write(ln)
arm = json.load(open(tpl + '/arm.json'))
nw = arm['network']
nw['closed_loop']['replay_products_path'] = out + '/products.txt'
nw['product_output_path'] = out + '/products_replay_out.txt'
json.dump(arm, open(out + '/arm.json', 'w'), indent=1)
print(n, 'P records moved by -Phi')
