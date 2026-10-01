"""Card PAC section 2: the fleet-common clock across the clock-pivot handover, and by run hour.

    python scripts/clock_level/pac_slope.py <run> [<run> ...]

Two series per run: the fleet-common emitted clock (the per-epoch median of the C-record steps over
the satellites present at both epochs) and the frame-free common step of D (card FXA: the per-epoch
median of the L2W block clones' S_D steps), the handover epoch (TOW 276150 on DOY 129) left out of
both.  Their slopes over 30 and 60 minutes either side of the handover, the pivots' own steps, and
the frame-free and emitted slopes by run hour.
"""
import sys, collections, statistics as st, importlib.util
spec = importlib.util.spec_from_file_location('rw', __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'rev_wire.py')); rw = importlib.util.module_from_spec(spec); spec.loader.exec_module(rw)
H = 276150.0
def emitted(d):
    C = collections.defaultdict(dict); tow=None
    for ln in open(d+'/products.txt', encoding='utf-8'):
        if ln[0]=='E': tow=round(float(ln.split(',')[2]),1)
        elif ln[0]=='C' and tow is not None:
            f=ln.split(','); C[tow][f[1]]=float(f[2])
    return C
def fit(pts):
    n=len(pts); 
    if n<5: return float('nan')
    mx=sum(t for t,_ in pts)/n; my=sum(y for _,y in pts)/n
    return sum((t-mx)*(y-my) for t,y in pts)/sum((t-mx)**2 for t,_ in pts)*3600.0   # m per hour
for run in sys.argv[1:]:
    d=rw.rundir(run); C=emitted(d); tows=sorted(C)
    com=[]; piv={'G01':[], 'G02':[]}; acc=0.0; accp={'G01':0.0,'G02':0.0}
    for a,b in zip(tows,tows[1:]):
        if abs(b-a-30)>0.5 or abs(b-H)<1: continue        # skip gaps and the declared handover epoch
        steps={s:C[b][s]-C[a][s] for s in C[b] if s in C[a]}
        if len(steps)<4: continue
        m=st.median(steps.values()); acc+=m; com.append((b,acc))
        for p in piv:
            if p in steps: accp[p]+=steps[p]; piv[p].append((b,accp[p]))
    # frame-free common step of D from the clone trace (L2W keys)
    rows=rw.load(d); inc=collections.defaultdict(list)
    for (sat,sig),lst in rows.items():
        if sig!='L2W': continue
        for cl in rw.clones(lst):
            for x,y in zip(cl,cl[1:]):
                if abs(y['tow']-x['tow']-30)<0.5 and abs(y['tow']-H)>1: inc[y['tow']].append(y['sd']-x['sd'])
    dd=[]; a2=0.0
    for t in sorted(inc):
        if len(inc[t])>=4: a2+=st.median(inc[t]); dd.append((t,a2))
    print(run)
    for win in (1800.0, 3600.0):
        pre=lambda s: [p for p in s if H-win<=p[0]<H]; post=lambda s: [p for p in s if H<p[0]<=H+win]
        print(f'  window {win/60:.0f} min: fleet-common emitted clock slope {fit(pre(com)):+.3f} -> {fit(post(com)):+.3f} m/h | '
              f'frame-free common D slope {fit(pre(dd)):+.3f} -> {fit(post(dd)):+.3f} m/h | '
              f'G01 own emitted slope {fit(pre(piv["G01"])):+.3f} -> {fit(post(piv["G01"])):+.3f} | G02 {fit(pre(piv["G02"])):+.3f} -> {fit(post(piv["G02"])):+.3f}')
    # hourly slopes of the frame-free common D over the run, for context
    t0=dd[0][0]; hrs=[]
    for h in range(7):
        seg=[p for p in dd if t0+h*3600<=p[0]<t0+(h+1)*3600]
        if len(seg)>20: hrs.append(f'{fit(seg):+.3f}')
    print('  frame-free common D slope by run hour (m/h):', ' '.join(hrs))

def hourly(run):
    d=rw.rundir(run); C=emitted(d); tows=sorted(C); t0=rw.run_start(d)
    acc=0.0; com=[]
    for a,b in zip(tows,tows[1:]):
        if abs(b-a-30)>0.5 or abs(b-H)<1: continue
        steps={s:C[b][s]-C[a][s] for s in C[b] if s in C[a]}
        if len(steps)>=4: acc+=st.median(steps.values()); com.append((b,acc))
    out=[]
    for h in range(7):
        seg=[p for p in com if t0+h*3600<=p[0]<t0+(h+1)*3600]
        if len(seg)>20: out.append(f'{fit(seg):+.3f}')
    print(run, 'fleet-common emitted clock slope by run hour (m/h):', ' '.join(out))
for r in sys.argv[1:]: hourly(r)
