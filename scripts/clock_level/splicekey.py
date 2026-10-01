"""Replace the P records of the given satellites in run A's wire with run B's (same epochs), and
write a replay arm from A's _rpl_ template.  python scripts/clock_level/splicekey.py <A> <B> <tpl_rpl_dir> <out_dir> <sat,...>"""
import json, sys, os
A, B, tpl, out, sats = sys.argv[1:6]
sats = set(sats.split(','))
def load(p):
    d = {}; tow = None
    for ln in open(p, encoding='utf-8'):
        if ln[0] == 'E': tow = round(float(ln.split(',')[2]), 1)
        elif ln[0] == 'P' and tow is not None:
            f = ln.split(','); d[(tow, f[1], f[2])] = f[3]
    return d
pb = load(B)
os.makedirs(out + '/logs', exist_ok=True)
n = 0; tow = None
with open(out + '/products.txt', 'w', encoding='utf-8', newline='\n') as fo:
    for ln in open(A, encoding='utf-8'):
        if ln[0] == 'E': tow = round(float(ln.split(',')[2]), 1)
        if ln[0] == 'P' and tow is not None:
            f = ln.rstrip('\n').split(',')
            if f[1] in sats and (tow, f[1], f[2]) in pb:
                f[3] = pb[(tow, f[1], f[2])]; n += 1
                ln = ','.join(f) + '\n'
        fo.write(ln)
arm = json.load(open(tpl + '/arm.json'))
nw = arm['network']
nw['closed_loop']['replay_products_path'] = out + '/products.txt'
nw['product_output_path'] = out + '/products_replay_out.txt'
json.dump(arm, open(out + '/arm.json', 'w'), indent=1)
print(n, 'P records replaced')
