# Workbook module 3 · What a user cannot see

**Goal:** why a user of the network's corrections cannot see the network's reference choices,
which quantities may therefore be graded, and the traps that catch a grade built on a
reference-dependent quantity.
**Report steps it bounds:** step 3 ("a constant satellite phase bias simply joins the ambiguity,
so case D matches case C"), the step 5 clock comparison (two means removed) and every grade of
steps 6 and 7.
**Time:** about 50 minutes. Module 2 comes first.

## The idea

A user positions itself with the same equations as the network (module 1), with the network's
clock C, code bias B and phase bias P in place of the satellite terms, and with its own
receiver clock, slant ionosphere and float ambiguities. Three parts of the corrections drop out
of the user's position:

1. a change common to all satellites of one system, absorbed by the user's receiver clock (and
   inter-system bias);
2. a constant per-satellite offset on a phase bias, absorbed by the float ambiguity of that
   satellite;
3. a geometry-free offset of one satellite's code biases, absorbed by the user's slant
   ionosphere of that satellite (the code biases and the ionosphere act as a pair).

These are exactly the directions a reference choice moves (module 2). The S-system theory states
the same for the correction covariance: its rank-one clock and ionosphere parts "lie in the
user's own design range and are absorbed" (`SBASIS-THEORY-E.md`, §E4). Hence standing invariant 5
of `CLAUDE.md`: **grade only datum-invariant or single-difference quantities.** A raw step of a
clock record, a single satellite's clock against another product without the two means removed,
or a double difference built on one reference satellite mixes the reference choice into the
grade.

What a user does see is declared: the header line of reference choices (the S line of
`products.txt`) names the reference station, the clock pivots, the code-bias pin pairs, the
geometry-free datum and the hold in force, and an epoch record (`E`, `datum_changed=1`) marks
every change of pivot.

The grading traps of `CLAUDE.md` ("Grading traps that still apply") follow from this: the double
difference trace has one reference satellite in every chord; subtracting a group's own circular
median shrinks wrapped noise by about 42 %; the network's ionosphere trace is graded on
`iono_total_m`; and a shift of a carried column's linearization point (`x_lin`) is an exact no-op
of the solve.

The figure of module 5 (the observation graph) shows the same idea for ambiguities: the tree arcs
are fixed by the reference choice, only the chord carries a double difference that the data
measure.

Sources: `CLAUDE.md` ("Standing invariants" item 5, "Grading traps that still apply"),
`docs/cards/network-vehicle/SBASIS-THEORY-E.md`, `docs/cards/network-vehicle/SBASIS-CONTRACT.md`,
`docs/cards/sim-bed/DD-TRACE-REF-SAT.md`, `docs/cards/slip/LANE-SNAP-RUNG3.md`,
`include/products/NetworkProductEmitter.hpp`, `src/core/products/NetworkProductIO.cpp`.

## Questions

### Q3.1 (derive) Three things a float user absorbs

Take the user's code and phase rows for satellite s of system g, with receiver clock `dt_r`,
inter-system bias, slant ionosphere `I_s` and float ambiguity `N_s` as unknowns, and the
corrections C, B, P applied.

1. Show that adding Δ to the clock C of every satellite of system g changes no position.
2. Show that adding a constant d (metres) to one satellite's phase bias P changes no position,
   and say what changes instead.
3. Show that adding the geometry-free offset (δ, μ₂·δ) to the two code biases of one satellite
   is absorbed by `I_s`, and say what that does to the user's ionosphere estimate.
4. The step 5 clock comparison removes two means before computing a standard deviation. Which
   two, and which of the three cases above does each stand for?

### Q3.2 (read) The header line of reference choices

Line 2 of `runs/_rf0hv15_sim6/products.txt` is:

```
S,S30000SIM,1,nominal-wu93,0,0,1,0,0.05,1,COD0MGXFIN_20241290000_01D_05M_ORB.SP3+COD0MGXFIN_20241290000_01D_30S_CLK.CLK,E:E02,G:G01,cbjump=1,atx=igs20.atx,oref=CoM,discfrac=0.25,hsig=pc,cbpin=E:C1C-C5Q,cbpin=G:C1C-C2W,sat_gf_datum=broadcast_tgd,pbhold=2
```

With `src/core/products/NetworkProductIO.cpp` (the writer, lines 465 to 602) and
`include/products/NetworkProductEmitter.hpp`, what does each field declare? Which fields name a
reference choice of module 2? Which token does a current run add that this one lacks, and which
datum of the real-data run's S line is not declared?

### Q3.3 (measure) A declared datum step, seen and absorbed

Find the epochs at which the run declares a change of reference
(`awk -F, '/^E,/ && $7=="1" {print NR": "$0}' runs/_rf0hv15_sim6/products.txt`), then measure how
every GPS clock record moves across the GPS one (tow 276150):

```
C:/Python313/python.exe - <<'EOF'
import statistics as st
d, t = {}, None
for line in open('runs/_rf0hv15_sim6/products.txt'):
    f = line.rstrip('\n').split(',')
    if f[0] == 'E':
        t = round(float(f[2]) / 30) * 30
    elif f[0] == 'C' and f[1].startswith('G') and t in (276120, 276150):
        d.setdefault(f[1], {})[t] = float(f[2])
s = [v[276150] - v[276120] for v in d.values() if len(v) == 2]
print(len(s), 'satellites, mean step %.4f m, sd after the mean %.4f m' % (st.mean(s), st.pstdev(s)))
EOF
```

How large is the step of the GPS clock records, how much of it is left after the per-system
mean is removed, and what absorbs the rest at a user? Why would a grade of raw clock steps call
this epoch a 1.6 m defect?

### Q3.4 (measure) The reference satellite in every chord

The network's double-difference trace (`network_dd_fractional.csv`) differences every satellite
against one reference satellite. Run (about 12 s):

```
C:/Python313/python.exe scripts/simnet_amb_integer.py runs/simnet60_quiet_col_24h --run runs/_rf0hv15_sim6 --ref-free
```

Compare the L1C double-difference error as traced and reference-free (median |e| in cycles and
the share that rounds to the right integer), over all epochs and over the last quarter. Then
derive: if every satellite's error has variance σ² and the reference satellite's σ_ref², what is
the variance of one chord and the correlation between two chords? Why does a grade on the traced
chords report the reference satellite's error?

### Q3.5 (claim) May the presentation say that a user cannot see the reference choices?

What may it claim, with which evidence, and what may it not? Consider in particular the case
where the network's a priori clock is wrong (module 4) and a user that fixes integers.
