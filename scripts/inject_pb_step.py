#!/usr/bin/env python3
"""Copy a network product file with an undeclared phase-bias step on one key.

usage: inject_pb_step.py <in.txt> <out.txt> <sat> <signal> <step_m> <tow_from>
Every P record of <sat>/<signal> in an epoch (E record) at or after <tow_from> gets <step_m> added to
bias_m. The discontinuity counter and the hold generation are left alone, so the step is undeclared.
"""
import sys
src, dst, sat, sig, step, tow_from = sys.argv[1:7]
step = float(step); tow_from = float(tow_from)
tow = -1.0; n = 0
with open(src, 'r', encoding='utf-8', errors='replace', newline='') as f, open(dst, 'w', encoding='utf-8', newline='') as g:
    for line in f:
        if line.startswith('E,'):
            tow = float(line.split(',')[2])
        elif line.startswith('P,') and tow >= tow_from:
            parts = line.rstrip('\r\n').split(',')
            if parts[1] == sat and parts[2] == sig:
                parts[3] = repr(float(parts[3]) + step)
                line = ','.join(parts) + '\n'
                n += 1
        g.write(line)
print('P records stepped:', n)
