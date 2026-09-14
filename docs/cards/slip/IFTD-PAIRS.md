# IF-TD pairs — the IF-TD detector watched ONE lane per satellite — `if_phase_td_pairs`

**STATUS**

- **id:** IF-TD pairs
- **topic:** slip
- **status:** closed
- **summary:** the IF-TD detector watched ONE lane per satellite — `if_phase_td_pairs`
- **dates in the text:** 2026-09-11
- **knobs / posture:** `network.cycle_slip.if_phase_td_pairs`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_event_harness.md` — 14.10, 14.10.1, 14.10.2, 14.10.3, 14.10.4

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 14.10 The IF-TD detector watched ONE lane per satellite — `network.cycle_slip.if_phase_td_pairs` (2026-09-11)

### 14.10.1 The defect

The common-band scope's IF-TD VETO (§14.2) is what closes the EQUAL-COUNT trap:
two bands slipping by the same integer leave their Melbourne-Wübbena lane silent
while the iono-free combination moves 0.107 m per cycle, so a silent MW pair with
no IF-TD pair behind it exonerates two bands that both moved.  But
`IfPhaseSlipDetector` forms **exactly one pair per satellite** — the first two
distinct phase bands it finds (L1C+L2W on GPS, L1C+L5Q on Galileo) — so the veto
reaches one lane of three and the trap is open on every other one.

Measured on the storm bed, station **S37000SIM / G20 at 4.99 h**: truth L5Q −31
cycles (receiver-flagged) at 4.98 h and L1C −29 cycles at 4.99 h.  The (L1C+L5Q)
lane moved by 2 wide-lane cycles and stayed silent, and the verdict read

```
keep implicated=L2W; exonerated=L1C;L5Q;
basis=mw(L1C+L5Q)silent(n=99;thr=9.0WL);mw(L1C+L2W)flag;
      mw(L2W+L5Q)flag;iftd(L1C+L2W)=flagged
```

— it freed the one band that had **not** slipped, the epoch's misfit was metres,
and the user paid +9.5 cm for the following hour.

### 14.10.2 The knob

`network.cycle_slip.if_phase_td_pairs` = `first` (default, byte-identical) |
`all`.  Under `all` the detector evaluates **every distinct pair** of a
satellite's tracked phase bands (3 on a triple-band arc, 1 on a dual-band one).
Each pair rides its own GRADIENT CHAIN: `constellation_state_` is keyed by the
bare system string for a satellite's FIRST pair — so that pair's population,
double differences and log lines are the pre-knob ones byte for byte — and by
`"<system>|<sig1>_<sig2>"` for every additional pair.  The driver's pools
(centre / scale / shrinkage) were already per pair KIND, and the verdict's
read-only preview reads the same source, so the extra pairs are a pure addition
on both sides.

A flag resets **that chain only** (`resetSatelliteInState`); the whole-satellite
`resetSatellite` would discard the other pairs' clean gradient histories and make
one pair's decision depend on another's.  `T1_if_slip` rows carry a
`pair=L1C+L5Q` token under `all` (omitted under `first`, so those rows stay the
pre-knob text), reset keys are per pair (`G01_L1C_L5Q`), and the run-end clause
reports evaluated / flagged / refloated per pair kind, knob-independently.

### 14.10.3 ⚠ 14.10.4 first — ONE reference arm per (station, constellation)

Running the reference-satellite slip test (tier T1r) per CHAIN is **not** a pure
addition: it is three tests on three different iono-free combinations of the same
arcs, and the extra-wide ones are ill-conditioned.  (L2W,L5Q) has IF coefficients
12.26 / −11.26 — noise amplification 16.6 against 2.98 on (L1C,L2W) — while its
λ_IF is 0.1248 m, so the legacy candidate condition |median(non-ref ΔΔ)| >
0.5·λ_IF is met by NOISE.  Measured on a 70-epoch quiet 60-station null before
this clause:

| quantity | `first` | `all`, per-chain ref arm | `all`, shared ref arm |
|---|---|---|---|
| IF-TD ref-gate legacy candidates | 25 | **5 351** | **25** |
| …of which FIRED | 1 | **64** (52 marked incoherent STATION EVENTS) | **1** |
| pooled-arm flags | 0 | 0 | 0 |
| products / solution / ZWD / DD-frac vs `first` | — | all DIFFER | **all IDENTICAL** |

A false-alarm cost entirely invisible in the per-pair evaluated/flagged tally, and
the reason the first `all` arms were not byte-identical to `first` on a quiet bed.
Additional chains now ADOPT the first chain's reference (so every pair of a
satellite sits in one between-satellite frame) and skip its test; the fallback to
their own election covers only a satellite set that does not contain that
reference at all.

### 14.10.4 Harness legs (quiet seed ep0665, S21000SIM / G08, event at +10, 70 epochs)

Arms: **OFF** (`common_band_scope` false, `if_phase_td_pairs` first), **ON**
(true, first), **FIX** (true, all).  Arms of a leg differ in exactly those two
knobs (verified by config diff).  G08's arc at the seed is 367 epochs old, but the
MW detector's state is not in the snapshot, so the in-leg maturity reads n = 10 at
the event — exactly the `common_band_min_arc_epochs` bar, which is what makes
these legs reproduce the field shape at all.

| leg | event(s) | ON verdict | FIX verdict |
|---|---|---|---|
| **S1** storm shape | L5Q −31 **with LLI** @+10, L1C −29 @+11 | +1 `keep implicated=L2W` — **the field defect, verbatim** | +1 `keep implicated=L2W` — **all-pairs alone does NOT close it** |
| **S2** pure equal-count | L1C −29 and L5Q −29 both @+10, no flag | +0 `keep implicated=L2W` — wrong | +0 `all reason=no_exoneration`, `iftd(L1C+L5Q)=flagged` — **veto fires** |
| **L2** single-band control | L2W −33 @+10, no flag | +0 `keep implicated=L2W` | identical verdict; basis gains `iftd(L1C+L5Q)silent` |
| **S3** LLI only | L5Q −31 with LLI @+10 | +1 `keep implicated=L5Q` — the CORRECT late narrowing | identical verdict |

Per-band DD level residual of the injected arc (event − null, cycles; the
`sig` band has its injected integer removed, the others do not — read across
arms):

| leg / arm | L2_d@+1 | L5_d@+1 | L2_d@+45 | L5_d@+45 | oth_d rms +14..45 | ZWD_d peak | iono_d peak | T4 rows (ids) |
|---|---|---|---|---|---|---|---|---|
| S1 OFF | −2.214 | −2.280 | −0.367 | −0.378 | 0.0214 | 0.0026 m | −0.331 m | 0 |
| **S1 ON (pre-F2, the defect)** | **+13.822** | **+9.078** | +0.290 | +0.289 | **0.0668** | **0.729 m** | 0.523 m | **15 (15, all at S21000SIM)** |
| S1 FIX (pre-F2) | +5.935 | +4.434 | +0.236 | +0.251 | 0.0215 | 0.237 m | 0.470 m | **0** |
| S1 ON / FIX with F2 (§14.11) | −2.214 / −2.228 | −2.280 / −2.294 | −0.367 | −0.378 | 0.0214 / 0.0192 | 0.0026 m | −0.330 m | 0 |
| S2 OFF | −0.587 | −29.584 | −0.326 | −29.335 | 0.0193 | 0.0027 m | 0.143 m | 0 |
| **S2 ON (the defect)** | −2.428 | −31.484 | −0.256 | −29.264 | 0.0459 | 0.247 m | **3.846 m** | 0 |
| **S2 FIX** | −0.587 | −29.584 | −0.326 | −29.335 | 0.0193 | 0.0027 m | 0.143 m | 0 |
| L2 OFF | −0.587 | −0.584 | −0.326 | −0.335 | 0.0193 | 0.0027 m | 0.143 m | 0 |
| L2 ON | −0.031 | −0.002 | −0.013 | −0.008 | 0.00014 | 0.0003 m | −0.003 m | 0 |
| **L2 FIX** | −0.031 | −0.002 | −0.013 | −0.008 | 0.00014 | 0.0003 m | −0.003 m | 0 |

Readings: **S2 FIX is OFF digit for digit** — the veto returns the leg to today's
response.  **L2 FIX is ON digit for digit** on every channel, and products /
solution / ZWD / DD-fractional are byte-identical between them, so the widening is
exactly inert on a genuine single-band slip.  **S1 ON pre-F2 is what the defect
costs**: 13.8 and 9.1 cycles of level error at +1, ZWD 0.73 m, the station's other
arcs 3.1× worse, and the post-fit tier erasing **15 innocent arcs at that one
station**.  All-pairs alone removes all 15 erases and halves the level error (the
new pairs catch the L5Q slip at +0), but it does not remove the wrong narrowing:
a flag resets its own chain, so at +1 — the epoch of the second slip and of the
verdict — the (1,5) and (2,5) pairs have no previous gradient, produce no sample,
and cannot veto.  That is §14.11.

**⚠ The one measured COST of `all`: S3.**  With the widening the S3 leg's level
residual goes 0.005 → 0.335 cyc at +45 and its slant-iono peak 0.004 → 0.143 m,
i.e. from near-perfect to the §9 all-band floor, *with the +1 verdict unchanged at
`keep implicated=L5Q`*.  Mechanism: at the LLI epoch itself **no verdict exists** —
every MW sample is withheld (§14.11), so `flagged_pairs` is empty and the rule
returns FALLBACK 1, and the drains run UNSCOPED.  Under `first` that was harmless
because the only actor there is the LLI drain, which is already band-scoped by the
receiver's own bits (it freed L5Q alone).  Under `all` the (1,5) and (2,5) IF-TD
pairs also flag at +0 and, unscoped, free both of their bands each — all three.
**Named fix, not built here: form a verdict at an LLI epoch too** (the LLI bits are
themselves evidence: implicated = flagged bands, provided a silent mature
un-shadowed lane free of flagged bands exonerates the rest), so FALLBACK 1 stops
meaning "unscoped" on the one epoch class where a receiver flag has already named
the band.

