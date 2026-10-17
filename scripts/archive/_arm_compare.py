#!/usr/bin/env python3
"""Extract the Card-1f/1g A/B metrics from a set of run logs."""
import re, sys, os
PATS = {
 'emitted'  : (r"producer complete: (\d+) epochs emitted, (\d+) skipped \((\d+) of those withheld by the observability gate", None),
 'gate'     : (r"rank gate evaluated on (\d+) epoch\(s\).*?(\d+) withheld UNDER-specified.*?(\d+) withheld OVER-specified", None),
 'ddi'      : (r"ratio measured on (\d+) epoch\(s\), (\d+) \(epoch x direction\) samples — R range \[([\d.e+-]+), ([\d.e+-]+)\].*?(\d+) epoch\(s\) breached R >= 1 across (\d+) edge", None),
 'cpl'      : (r"injected terms: (\d+) present, (\d+) departed, (\d+) kappa-shrunk", None),
 'gauge'    : (r"projection — (\d+) \(constellation,kind\) family sum\(s\) HELD.*?, (\d+) ORPHANED", None),
 'divert'   : (r"reinit sources — (\d+) coupling term\(s\) diverted", None),
 'nearnull' : (r"near-null count over the run: min (-?\d+), max (-?\d+)", None),
}
def scan(path):
    try: txt=open(path,encoding='utf-8',errors='replace').read()
    except OSError: return None
    txt=re.sub(r'\n\s*',' ',txt)   # run-end lines wrap
    out={}
    for k,(p,_) in PATS.items():
        m=re.search(p,txt,re.S)
        out[k]=m.groups() if m else None
    return out
for name in sys.argv[1:]:
    r=scan(f'runs/{name}/run.log')
    print(f'=== {name} ===')
    if not r: print('  (no log)'); continue
    if r['emitted']: print(f"  emitted {r['emitted'][0]}  skipped {r['emitted'][1]}  gate-withheld {r['emitted'][2]}")
    if r['gate']:    print(f"  gate: {r['gate'][0]} ep,  UNDER {r['gate'][1]},  OVER {r['gate'][2]}")
    if r['nearnull']:print(f"  observed near-null min..max: {r['nearnull'][0]}..{r['nearnull'][1]}")
    if r['ddi']:     print(f"  inv#7: {r['ddi'][1]} samples, R [{r['ddi'][2]}, {r['ddi'][3]}], breach ep {r['ddi'][4]}, edges {r['ddi'][5]}")
    if r['cpl']:     print(f"  coupling terms: present {r['cpl'][0]}, departed {r['cpl'][1]}")
    if r['gauge']:   print(f"  1f held/orphan: {r['gauge'][0]} / {r['gauge'][1]}")
    if r['divert']:  print(f"  1g diverted: {r['divert'][0]}")
