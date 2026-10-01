"""Derive an arm from an existing arm: copy its arm.json, re-point the run paths, set knobs.

    python scripts/clock_level/mkarm.py <src_tag> <dst_tag> [dotted.key=json_value ...]
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'


def setk(cfg, dotted, val):
    d = cfg
    ks = dotted.split('.')
    for k in ks[:-1]:
        d = d.setdefault(k, {})
    d[ks[-1]] = val


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s_dir, d_dir = f'{RUNS}/_rap_{src}', f'{RUNS}/_rap_{dst}'
    text = open(f'{s_dir}/arm.json', encoding='utf-8').read()
    text = text.replace(f'runs/_rap_{src}/', f'runs/_rap_{dst}/')
    cfg = json.loads(text)
    for kv in sys.argv[3:]:
        k, v = kv.split('=', 1)
        setk(cfg, k, json.loads(v))
    os.makedirs(d_dir + '/logs', exist_ok=True)
    json.dump(cfg, open(f'{d_dir}/arm.json', 'w', encoding='utf-8'), indent=4)
    left = [ln for ln in json.dumps(cfg).split('"') if f'_rap_{src}' in ln]
    print(dst, 'ok', 'LEFTOVER' if left else '')


if __name__ == '__main__':
    main()
