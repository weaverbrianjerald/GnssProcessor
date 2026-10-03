"""Hybrid wires from two runs of one bed (same epochs, keys): code side (C) from one, phase side
(C - P) from the other.  python scripts/clock_level/scl_hybrid_wire.py <code_run> <phase_run> <out_products>"""
import sys
def load(path):
    lines=open(path,errors='replace').read().splitlines(True)
    C={}; tow=None
    for ln in lines:
        if ln[0]=='E': tow=ln.split(',')[2]
        elif ln[0]=='C': p=ln.split(','); C[(tow,p[1])]=float(p[2])
    return lines,C
code_lines,Cc=load(sys.argv[1]); ph_lines,Cp=load(sys.argv[2])
# start from the phase run's lines; C := code run's C; P := P_ph + (C_code - C_ph) so C - P = phase run's
out=[]; tow=None; n=0
for ln in ph_lines:
    if ln[0]=='E': tow=ln.split(',')[2]; out.append(ln); continue
    if ln[0]=='C':
        p=ln.rstrip('\r\n').split(','); k=(tow,p[1])
        if k in Cc: p[2]=repr(Cc[k]); n+=1
        out.append(','.join(p)+'\n'); continue
    if ln[0]=='P':
        p=ln.rstrip('\r\n').split(','); k=(tow,p[1])
        if k in Cc and k in Cp: p[3]=repr(float(p[3])+Cc[k]-Cp[k])
        out.append(','.join(p)+'\n'); continue
    out.append(ln)
open(sys.argv[3],'w',newline='').write(''.join(out)); print('C replaced',n)
