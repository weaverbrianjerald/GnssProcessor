# field wander probe: the field wire wander is estimator-internal

**STATUS**

- **id:** field wander probe
- **topic:** clock
- **status:** closed
- **summary:** The field wire wander (1.3 m detrended) is NOT receiver-code-driven and NOT the a-priori clock product's drift; with slips, datum geometry and tropo also excluded, it is ESTIMATOR-INTERNAL.
- **dates in the text:** 2026-08-11
- **knobs / posture:** none (trace probes on the CB-Q 12 h control: FT63 lineage, W6+SH posture, datum MBAR00UGA)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/cbq_12h_ctl`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §9.9, §9.9.1

**Outcome.** The probe asked whether the field wire's common-level wander is driven by a receiver's code errors or by the a-priori clock product. Neither: every station's corr(dw, dmp), the datum station MBAR's included, sits at the noise floor (max |r| 0.053), and the COD CLK's −4.089 m/h common drift is not inherited (detrended slope −0.014). The wander is the LEVEL channel's clock, code-bias and ionosphere common-direction exchange. Caveat: the increment probe covers the 30 s channel only, so an hours-scale driver needs a level-domain instrument.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §9.9 The FIELD probe — the wire wander is NOT receiver-code-driven (2026-08-11)

`scripts/field_wire_datum_probe.py` on the retained CB-Q 12 h control run
(`runs/cbq_12h_ctl`: 61 stations, datum MBAR00UGA, the FT63 lineage, W6+SH posture).  Method:
wire common level w(t) = per-epoch median of the emitted C records (increments per-satellite
→ median; pairs across the run's single `datum_changed` epoch dropped); per-station iono-null
multipath proxy from `network_slip_raw.csv` (`l1c1 + 2/(γ−1)·gf` — increments track
−Δmp_code; LLI rows excluded); corr(dw, dmp_i) per station over the settled 10 h.  TOW keys
quantized to the second (the E rows carry float dust: 302370.00000190735 vs the trace's
%.3f — exact-key joins silently die).

**Result: the §9.8 receiver-side driver is ABSENT on the field.**  Field wire wander
**1.735 m** demeaned std over the longest settled segment (875 epochs) — the CB-Q "metres per
pass" scale, quantified.  corr(dw, dmp): **MBAR ranks 48 of 61** (r = +0.011); controls
median |r| = 0.018, max 0.053 — no station's code observables track the wire, the whole table
is at the noise floor.  Tropo at MBAR is excludable by magnitude (1.7 m of clock common would
need ~0.6–1 m of ZWD error), though the mp proxy is structurally blind to it (tropo cancels
in L1−P1).

**Interpretation.**  The sim's geometry-driven datum-receiver term exists but is SMALL on the
field (MBAR is equatorial; the sim bound for a good datum is ~0.02–0.03 m, two orders below
the 1.735 m observed).  The field wander is dominated by the SHARED/product common
directions — the CB-Q "unpinned inter-channel drift" (clock ↔ CB ↔ iono exchanges, plus any
a-priori product common drift, which enters w(t) directly as −(apriori common) and which no
station observable can track).  This probe eliminates the receiver-local hypothesis and
returns the hunt to the LEVEL information channel (`SPEC_level_information_budget`, the CB-Q
card).  **Caveat**: the increment probe covers the FAST channel (30 s); a driver acting only
at hours-scale with negligible 30 s increments would evade it — the level-domain candidates
need a level-domain instrument (the CB-Q structure-function attack, not this one).

### 9.9.1 The a-priori CLK common-drift probe — the PRODUCT FRAME EXONERATED

`scripts/field_apriori_clk_probe.py`: A(t) = c·median_s dt_apriori_s(t) from the cached COD
30 s CLK, against w(t) on the joint 874 settled datum-stable epochs (7.3 h).  The C records
are corrections about this product, so a product fleet-common drift enters w(t) at slope −1
and no station observable can track it — the exact §9.9 signature.

- A(t) is a nearly pure LINEAR timescale drift: −4.089 m/h, residual std **0.094 m** after
  detrending (range 29.8 m raw).
- The wire does NOT inherit it: w's own trend is −0.548 m/h (slope-−1 inheritance would read
  +4.089), detrended corr(w,A) = −0.19 at slope **−0.014**, increment corr +0.04.  The
  detrended product common (9 cm) could at most explain ~7 % of the wire's 1.298 m detrended
  wander even at slope −1; measured slope says it explains ~none.
- Linear timescale offsets are frame-conventional (a datum-invariant consumer never sees
  them).

**With §9.9 this corners the field wander: slips (§9.7.9), every station's receiver-code
errors (§9.9), datum-station geometry (§9.8, bounded 2–3 cm at an equatorial datum), tropo
(magnitude), and the a-priori frame (here) are all eliminated by measurement.  The 1.3 m
detrended wander is ESTIMATOR-INTERNAL — the clock ↔ CB ↔ iono common-direction exchanges,
i.e. the LEVEL information channel.**

