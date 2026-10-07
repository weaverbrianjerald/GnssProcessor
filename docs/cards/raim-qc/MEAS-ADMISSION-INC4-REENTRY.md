# INC-4: the admission screen's arc re-entry vet

**STATUS**

- **id:** INC-4
- **topic:** raim-qc
- **status:** closed
- **summary:** 16 of 17 mid-arc onsets were re-entries inheriting a stale verdict; the re-entry vet at its shipped vet bound CLOSES both classes on two legs, and stays default OFF.
- **dates in the text:** 2026-08-02
- **knobs / posture:** `network.adm_revet` (default false, requires `adm_screen`), `network.adm_revet_hold`, `network.adm_revet_bound_scale` (shipped 0.2, a 200 m vet bound), `network.adm_revet_gap_ep`, `network.adm_revet_ep`, `network.adm_revet_census`; none is set in `config/golden/golden_field_refound.json` or `config/golden/golden_sim_refound.json`
- **hashes named in the text** (commit / exe / byte-cert): gate-1 md5 `15629c86`, `948ffbab`, `ff54584f`, `63ce6174`, `076aaeb2`; exe `gnss_processor_revet.exe`
- **run dirs named in the text:** `runs/ft63_24h_conv`, `runs/ft63_adm_bz14` (and `bz04`); arms `rv04{off,lbl,on}`, `rv14{off,lbl,hold,on}`, smoke `rvsmoke{,lbl,off}`
- **source:** `docs/archive/SPEC_measurement_admission.md` section 10 (10.1 to 10.11); the spec is [meas admission](../raim-qc/MEAS-ADMISSION.md)

**Outcome.** The question: should a channel returning after a gap inherit its pre-gap admission verdict. Twelve re-entries were mislabels, relabelled at zero cost. Label and prefit census alone pre-empted G13 and G22 but tied on G16 (its +19.2 TECU excursion kept); `adm_revet_hold` erased it, and the 200 m vet bound refused G16 on its return epoch while admitting 70 of 72 re-entries. The healthy maximum (61.9 m) and the lowest refusal (218.5 m) sit only 3.5× apart, so `adm_revet` stays default OFF and unpromoted, with no ledger row; the 24 h arm owed in section 10.9 is not recorded as run.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_measurement_admission.md -->
## 10. INC-4 — the arc RE-ENTRY vet (2026-08-02, Opus session)

Revives the §8 deferral "the retracted card's per-arc re-vet increment — revive only if a class
survives THIS screen".  One does.  It is not the class the retraction imagined, and it was hiding
inside this card's own onset counter.

### 10.1 The finding — there were no mid-arc onsets

`MeasurementAdmissionArc` records are kept for `kAdmArcForgetEp` = 2880 epochs (one day at 30 s), far
longer than an arc.  So a channel that leaves the model set and returns is still carrying
`admitted = true` from **before** the gap, and the seam's `else if (a.admitted)` branch dispatched it
down the ESTABLISHED path (INC-2 onset) rather than the birth path (INC-1).  The verdict being
inherited was made about a channel that is not necessarily the one that came back.

Measured on `runs/ft63_24h_conv` (63 stations, 24 h, 2880/2880 emitted, this card armed): **16 of the
17 `adm_onset` events are preceded by a `T0_gap arc_resume` on the same cell within five minutes.**
There were no genuine mid-arc onsets on the day at all.  The 17th (G07@BSHM, TOW 329640, band 2) is
the second band of a channel whose band 1 had already been refused.

| | n | shape | cost (a00) |
|---|---|---|---|
| **A — mislabel only** | 12 | screen fires at the SAME epoch as the resume, after gaps of 3–2025 epochs | none — the response was already right |
| **B — real exposure** | 4 | channel re-admitted, crosses the bound 2–8 epochs later | the day's two largest iono excursions |

**Class A is the more embarrassing half.**  One of its twelve is G16@BSHM at TOW 327210 with
`gap_ep=568` — gone 4.7 hours.  That is the *exact* channel §1 is written around, and §9.13's gate-2
leg replay calls it `adm_reject_birth` for the same cell at the same epoch with the same z = 97 537,
purely because a leg's record map starts empty (G07@BSHM at 327030, `gap_ep=2025`, is the same story).
**A classification that depends on how long the run has been going is not a classification.**

**Class B is where the cost is**, and it is in the exposure window rather than at the onset epoch —
the opposite of where one would look:

```
G16@BSHM   arc_resume 307740 (gap 3)   →   adm_onset 307800
  a00_tecu   307710: 39.679   ← flat 39.7-39.9 before
             307740: 52.730   ← the re-entry epoch, +13.1 the instant it is admitted
             307770: 59.401   ← peak, +19.7 over baseline; PREFIT names it here
             307800: 51.766   ← the onset withhold lands; recovery starts
             307980: 42.889

G13@BSHM   arc_resume 273240 (gap 1)   →   adm_onset 273390
  a00_tecu   273210: 30.838 → 273330: 40.639 (PREFIT names it) → 273360: 42.978
             (peak, +12.1) → 273390: 39.374 (withhold) → 273540: 34.713
```

Against that, the day's **eight `adm_reject_birth` events move a00 by a median of −0.11 TECU (max
+0.32)**.  That asymmetry is the whole argument for the increment and it is exactly standing invariant
#7's two edges: refusing at ADMISSION is free, because nothing was ever coupled; amputating an
ESTABLISHED arc discharges whatever tension it accumulated while it was trusted.

**And the census already knew.**  All four class-B cells carry a `prefit_exceedance` before this
card's bound reaches them — at return + 1, +3, +4 and +1 epochs respectively.  The G16 row, verbatim:

```
2313,307770.000,PREFIT,prefit_exceedance,G16,BSHM00ISR,1439.600121,1000.000000,
  flag,class=obs_code row=12 y=975.544 m=16.5467 w=2.25346
```

Raw `y` = 975.5 m — *under* this card's 1000 m bound — but the census statistic is standardized
(`|y − m|·√w`), so it reads s = 1439.6 against its own threshold of 1000.  The two instruments
genuinely see different things, which is what makes one useful to the other.  §1 always called the
census "the detection net"; INC-4 is the wire from it to the response.

### 10.2 Design

`network.adm_revet` (default false, requires `adm_screen`).  A channel absent for at least
`adm_revet_gap_ep` **screened** epochs is re-screened as a BIRTH instead of inheriting its pre-gap
verdict, and then carries an open VET WINDOW for `adm_revet_ep` epochs during which a PREFIT-census
`obs_code` exceedance on the line of sight (previous epoch) refuses as surely as the |z| bound does.

Four properties are load-bearing:

1. **Class A costs nothing to fix.**  A birth screen admits a clean channel immediately, so a healthy
   re-entry gets no withhold, no refloat and no solve change.  The fix for the twelve is a relabel
   with an identical response — which is why it can ship armed without a cost argument.
2. **The gap is measured in epochs THIS SEAM SCREENED.**  A station under `adm_min_sats` or a
   satellite the two-station observability gate erased both read as absent.  That is the conservative
   direction (it re-screens), and it keeps the clock the same one `last_ep` and `kAdmArcForgetEp`
   already use.
3. **A heal opens a vet window too.**  A heal is a re-entry in all but name.  Without this, the one
   path that puts a previously-REJECTED channel back into the solve would be the only one that puts it
   back unvetted.
4. **Refusing re-arms the window**, so a channel that fails inside its vet serves a full window again
   rather than the remainder of an old one.

`network.adm_revet_hold` (default **false**) is the strict posture: withhold a clean re-entry until
the ordinary heal path readmits it.  It is the only posture that closes class B's exposure window
outright — see §10.4, where that is stated as a limitation rather than glossed.  Default-OFF on the
prefer-fallback directive and on the flicker card's INC-3 deviation 9: it would withhold every
re-entry (~4650 on a 63-station day, 1.6/epoch into a funnel the Iono-2 v3k erase policy is tuned on),
and the population it delays is dominated by legitimately RISING satellites.  A producer that admits
every rising satellite 2.5 minutes late across 63 stations is paying an observability cost to catch a
class the relabel already refuses on arrival whenever the fault is present on arrival.

### 10.3 Deviations and things worth knowing

**The census flag is per LINE OF SIGHT, not per band.**  The census's worst is taken over a
satellite's code rows and there is no row → band map to resolve it further, so a flagged satellite
refuses whichever of its bands is currently vetting.  The flicker card's demotion map made the
identical trade for the identical reason (its deviation 6).  Conservative, bounded by the vet window,
and `revet_census_refusals` is counted apart so the trade is auditable.

**The flag is written from the EXCEEDANCE, upstream of `census_on`, the event-class mask and the
per-cell holdoff.**  The census card's own principle is that a detection must not depend on whether
anyone is listening; a trigger throttled by a diagnostic rate limiter would go quiet exactly when a
cell is persistently bad.  It is written whenever the prefit WALK runs (`trace_prefit_census` ∪
`seed_birth_check_sigma` ∪ `trace_topology`), not only under the census knob.

**CODE rows only**, matching this card's own CODE-only-by-principle rule.  A phase row's prefit
innovation is arc-specific and its station median is not a common mode — the census card measured 608
of 665 phase events on one leg as non-findings, and a phase-driven trigger wired to an admission
verdict would be a false-positive generator.

**`revet_census_reads` exists because `revet_window_ep` is not its denominator.**  The return epoch is
inside the window but takes the birth-equivalent path, which asks the census nothing — and could not
usefully, because a re-entering arc's ambiguity is fresh, so the census defers it as a BIRTH on
exactly that epoch.  So `reads = window_ep − reentries` identically, and the dead-wiring warn keys on
`absent == reads` rather than on `absent == window_ep`, which would never be true.  Measured, not
asserted: the §10.5 smoke reports 67 window epochs, 12 re-entries and 55 consultations.

**How much the census can be worth, and why it is bounded.**  Its flags are read one loop epoch late
(the walk runs *after* this seam) and it cannot flag the return epoch at all (birth deferral), so the
earliest readable flag is **return + 2**.  On the four class-B cells the flags land at return + 1, +3,
+4, +1 — readable at +2, +4, +5, +2 — against bound crossings at return + 2, +5, +8, +2: it pre-empts
G13 and G22 and **ties on G16 and G22-first**.  Confirmed in §10.7.  A real gain on half the class and
no loss on the rest, **but not the mechanism that CLOSES the exposure**: a cell whose ramp crosses the
bound within two epochs of its return can never be pre-empted by an instrument that is structurally
two epochs behind.  That is `adm_revet_hold`.

**`adm_revet_ep` default 8, derived not picked.**  Max measured census-flag lag is return + 4, +1 for
the read latency = return + 5, and coverage runs to return + (`adm_revet_ep` − 1).  8 gives three
epochs of margin.  A window that is too LONG costs nothing on a healthy channel (the census simply
never flags it), so the asymmetry is deliberate.

**Not fingerprinted** — the SIXTH instance of the exemption, recorded in place at
`networkSnapshotFingerprint`.  Worth stating for INC-4 specifically because it reads a cross-epoch
record and that looks fingerprint-relevant at a glance.  It is not: `vet_left` and `last_ep` live in
the same unserialized record map as `admitted`, so a resumed leg opens no vet windows and every
channel reads as a first sighting — the RESTORE rule's case, already handled by §9.5.  This is what
makes the INC-4 OFF/ON pair on one shared snapshot legal, which is how §10.6 is run.

### 10.4 What this increment does NOT do — stated, not glossed

**It does not fully close class B's exposure window at the label-only posture.**  With
`adm_revet_hold` off and the vet bound at 1.0×, a corrupt channel *inside the bound on arrival* is
still admitted and still seeds: the census pre-empts on G13 and G22 but **ties on G16**, where the
whole +19.2 TECU excursion survives (§10.7 / §10.7b).  The full save needs `adm_revet_hold` (§10.8) —
or, as shipped, the 200 m vet bound (§10.11), which reaches the same outcome far more cheaply.

**A tighter vet bound was not shipped in this increment**, because at the time it would have been a
threshold invented from two extrapolated samples — what `feedback_mw_thresholds_empirical` says not to
do, with a failure mode (discarding freshly-risen low-elevation satellites) that
`feedback_prefer_fallback_over_drop` says not to accept.  **That measurement has since been made:
§10.10**, and the bound shipped at §10.11.

**The band-loss policy interaction of §3 is unchanged** — INC-4 withholds through the same path
INC-1/INC-2 do.

### 10.5 Gate 1 — smoke + OFF byte-identity, AS RUN: **PASS**

Same-build Release, renamed exe (`gnss_processor_revet.exe`).  Cold start, 10 epochs, 63 stations,
DOY-129 00:00, from `python scripts/adm_make_arms.py`.  The control is `rvsmokeoff`, **not** `screen`:
`screen` lacks `trace_prefit_census`, whose own epoch-status columns would land in the diff and be
misread as an INC-4 effect.  Both arms carry the census; the arms differ in `adm_revet` and nothing
else.

| artifact | `rvsmokelbl` vs `rvsmokeoff` | md5 |
|---|---|---|
| `network_solution.csv` | **IDENTICAL** | `15629c86…` |
| `network_trace.csv` | **IDENTICAL** | `948ffbab…` |
| `network_epoch_status.csv` | **IDENTICAL** | `ff54584f…` |
| `network_events.csv` | **IDENTICAL** | `63ce6174…` |
| `network_vtec_sh_health.csv` | **IDENTICAL** | `076aaeb2…` |

`network_solution.csv` is also md5-identical to the §9.5 / §9.15 record's `15629c86…`, i.e. to the
pre-INC-4 binary — so the increment is byte-inert on this window against two independent baselines.
**`rvsmoke` — the arm carrying the SHIPPED defaults, 200 m vet bound included — is byte-identical to
both** (`network_solution.csv` and `network_events.csv` both match `rvsmokelbl` exactly).  All twelve
of the smoke's re-entries sit under 200 m, so the tighter bound refuses nothing there either.  That is
a third window in which the vet bound does not touch a healthy re-entry, and it is why gate 1 passes
for the default configuration and not merely for the label-only one.

Counters, verbatim: **12 re-entry screenings → 12 admitted, 0 refused; 67 channel-epochs inside a vet
window, 4 windows run to completion; census trigger on → 0 of 55 consultations refused, 0 found no
fresh flag to read.**  Every part of that is a denominator rather than an absence: the vet ran 12
times, the cross-seam census wire was consulted 55 times and was FRESH on all 55 (proving the
one-epoch stamp handshake works in the field, not just in the rig), and it refused nothing because
nothing deserved refusing.  `67 − 12 = 55` confirms the `reads`/`window_ep` relationship of §10.3 on
real data.

### 10.6 Gate 2 — the class-B known-answer legs

ON/OFF pairs on ONE shared snapshot (legal: the family is out of the fingerprint), each window chosen
to CONTAIN measured class-B re-entries:

- `rv04lbl` / `rv04off` — `snap_04` + offset 3.1125 (TOW 270405), 4500 s: contains **G13@BSHM**
  (resume 273240 gap 1 → onset 273390, prefit flag 273330) and **G22@BSHM** (resume 273450 gap 9 →
  onset 273690, prefit flag 273570).
- `rv14lbl` / `rv14off` — `snap_14` + offset 13.1125 (TOW 306405), 3600 s: contains **G16@BSHM**
  (resume 307740 gap 3 → onset 307800, prefit flag 307770).

Both arms carry `trace_prefit_census`.  **The OFF arms reproduce the control's class-B events to the
digit**, which is what makes these legs known-answer gates rather than new measurements:

| leg | event, verbatim from the OFF arm | matches `ft63_24h_conv` |
|---|---|---|
| `rv14off` | `307800 adm_onset G16 BSHM00ISR 1233.810182 … d=-39459.265;med=-40693.075;n_sats=8` | **exact, every field** |
| `rv04off` | `273390 adm_onset G13 BSHM00ISR 1198.329938 … d=-39510.103;med=-40708.433;n_sats=9` | **exact** |
| `rv04off` | `273690 adm_onset G22 BSHM00ISR 1102.758610 … d=-39602.092;med=-40704.851;n_sats=9` | **exact** |

and each carries its PREFIT precursor in the leg too — G16 s=1458.8 at 307770 (y=982.8), G13 s=1351.6
at 273330 (y=761.1), G22 s=1009.5 at 273570 (y=693.4).

### 10.7 Gate 2 result — the census trigger fires, and it fires UNDER the bound

**`rv04lbl` vs `rv04off`: two `adm_revet_refuse` events, both on evidence the |z| bound could not have
produced.**

```
2313,273360.000,ADMISSION,adm_revet_refuse,G13,BSHM00ISR, 975.277688,1000.000000,
  withhold,band=1;d=-39730.416;med=-40705.694;n_sats=9;trigger=prefit_census;vet_left=8
2313,273600.000,ADMISSION,adm_revet_refuse,G22,BSHM00ISR, 782.240601,1000.000000,
  withhold,band=1;d=-39922.234;med=-40704.475;n_sats=9;trigger=prefit_census;vet_left=8
```

|z| = **975.3** and **782.2 m** against a 1000 m bound.  Attribution is unambiguous — the bound was not
reached at either epoch, and the OFF arm's own `adm_onset` for the same cells lands at 273390
(z = 1198.3) and 273690 (z = 1102.8).  **One epoch early on G13, three on G22**, precisely the §10.3
prediction from the flag lags.  And it is worth what it costs:

```
G13   (resume 273240, OFF onset 273390)
TOW        273180   273240   273270   273300   273330   273360   273390   273420   273480
rv04off     32.88    33.78    36.29    39.14    41.94    43.56    40.52    38.27    36.34
rv04lbl      32.88    33.78    36.29    39.14    41.94    39.23    37.34    36.12    34.94
                                                          ▲ refusal lands here
```

Identical through 273330 — the arms cannot differ before the refusal — then the peak epoch drops
**43.56 → 39.23, a 4.33 TECU cut**, and the leg stays ~1.4 TECU lower for its remainder (G22's whole
window inherits it: 34.99 → 34.11 at the refusal epoch, 34.44 → 33.77 by 273840).  Peak excursion over
the 32.88 pre-event level: **+10.68 → +9.06**.  `rv04lbl` also carries a second class-A relabel —
G15@BSHM at 274140, `adm_onset` → `adm_reject_reentry` with `gap_ep=3`, response byte-identical.

Run-end: **141 re-entries → 140 admitted, 1 refused on the return epoch; 786 vet-window
channel-epochs; 2 of 624 census consultations refused, 0 absent.**  Two refusals out of 624
consultations is the shape a good trigger should have — not a second robustness tier, a tripwire that
fires on the class the bound is structurally one epoch behind.

### 10.7b …but it does NOT close the class where the bound ties

**THE CONSOLIDATED `rv14` a00 LADDER** — all four postures on one TOW grid.  This is the single copy;
§10.8 and §10.11 refer to it rather than reprint it.

```
TOW        307650   307680   307710   307740   307770   307800   307830   307920
rv14off     39.71    39.67    39.56    56.62    58.77    49.12    47.05    46.71   OFF control
rv14lbl     39.71    39.67    39.56    56.62    58.77    49.12    47.05    46.71   label+census only — IDENTICAL
rv14hold    39.72    39.48    39.70    39.51    39.35    39.30    39.21    39.23   strict hold (§10.8)
rv14on        —        —      39.56    39.46    39.30    39.34    39.25    39.31   200 m vet bound (§10.11)
```

**`rv14lbl` vs `rv14off`: a00 is identical at every epoch of the exposure window.**  The two arms
differ by exactly ONE line in `network_events.csv`, and it is a class-A gain, not a class-B one:

```
2313,307590.000,ADMISSION,adm_reject_reentry,G21,BSHM00ISR,63411.873777,
  1000.000000,withhold,band=1;d=22719.880;med=-40691.993;n_sats=8;gap_ep=12
```

G21@BSHM had already been refused at birth (306480), went away for 12 epochs, and came back still
corrupt.  In the OFF arm that epoch fell down the `rejects_held` path and emitted **nothing**; INC-4
names it.  Same response, new visibility — the same shape as the topology ledger's 13 silent
re-coupling edges under invariant #7.

**G16 stays an `adm_onset` at 307800, and §10.3 predicted exactly this.**  Its re-entry at 307740 was
*inside the bound*, so a birth screen admits it; the census flag written at 307770 (s = 1458.8,
y = 982.8 — present in the leg, verified) is readable at 307800, the same epoch the bound itself
fires.  A tie.  `revet_census_refusals` = 0 of 369 consultations, 0 absent: the wire was live and
fresh on every one, it simply had nothing to add that the bound had not already reached.

So the default posture's class-B record across the three measured cells is **2 pre-empted (G13 by 1
epoch, G22 by 3), 1 tied (G16)**.  A partial fix, and which half a cell lands in is decided by whether
its ramp puts the census flag more than one epoch ahead of the bound crossing — a property of the
fault, not of the configuration.

### 10.8 The decisive arm — `adm_revet_hold` erases the event: **PASS**

`rv14hold` = `rv14lbl` + `adm_revet_hold`.  Same snapshot, same build.  Ladder: the `rv14hold` row of
§10.7b.

**Peak excursion +19.2 TECU → 0.0.**  The armed arm never leaves the 39.2–39.7 band — it does not
notice that an event happened, the same shape §9.13b's break gate produced.  G16 at 307800 is no
longer an onset at all but an `adm_census … held` row: it was refused on arrival at 307740 and never
entered the model set.

**And the control's "recovery" was never a recovery.**  OFF decays 58.77 → 46.71 and stays there — a
permanent **+7.1 TECU scar** against a 39.6 pre-event level.  `rv14hold` ends at 39.23.  The withhold
that lands at 307800 in the control arms stops the excursion growing; it does not undo what was
already absorbed.  That is standing invariant #7's decoupling edge — the amputation bounds the future,
not the past — and it is the strongest argument in this card for refusing at admission rather than
responding later.

| | `rv14off` / `rv14lbl` | `rv14hold` |
|---|---|---|
| CODE channel-epochs screened | 169 663 | 169 663 |
| channel-epochs withheld | 184 (0.108 %) | 474 (0.279 %) |
| …of which clean re-entries held | — | 71 |
| epochs emitted | 121 / 121 | **121 / 121** |
| vet-window channel-epochs | 458 | 654 |
| census consultations / absent | 369 / **0** | 299 / **0** |

So the posture that closes the class costs **+0.17 percentage points** of the screened channel
population and withholds no epoch — a far smaller price than the §10.2 argument assumed, and the
number the promotion decision should be made on.

**It stays default-OFF anyway, and the reason is what this leg cannot show.**  121 epochs measures the
EVENT cost; it cannot measure the convergence cost of admitting every rising satellite `adm_heal_ep`
epochs late across a full day and 305 stations, which is the actual objection.  `σ̂₀` would be the QC
arbiter and it is **not gradeable here** — the `sigma0` column reads 0.0000 on all three arms, the
same gap §9.14 records against gates 6/7.  Promotion needs the 24 h arm and a σ̂₀ source, not another
leg.

### 10.11 Gate 3 — the tighter vet bound, AS RUN: **PASS**

`rv14on` / `rv04on` = the gate-2 legs at the SHIPPED defaults (`adm_revet_bound_scale` 0.2, a 200 m
vet bound).  The `rv*lbl` arms pin the scale to 1.0 so they keep testing the relabel + census only.

**Arm-naming note, because it is a trap this repo has paid for before.**  These arms were originally
`rv*vb` with `rv*on` pinned to 1.0 — an arm named `on` that did NOT carry the shipped configuration.
That is the same shape as `pcarry` being silently forced off and `innov_refold` parsing inert: the
next session grades the wrong thing and never learns it did.  Renamed so `on` always means "what a
user gets", with `lbl` for the label-only variant; the run directories were migrated with the tags so
the evidence and the names cannot drift apart.

**G16@BSHM is refused on its return epoch, at the |z| §10.10 predicted:**

```
2313,307740.000,ADMISSION,adm_reject_reentry,G16,BSHM00ISR,670.281206,1000.000000,
  withhold,band=1;d=-40022.233;med=-40692.515;n_sats=8;gap_ep=2;trigger=vet_bound
```

670.281206 m against §10.10's measured 670.3.  The 307800 onset never happens — that epoch is now an
`adm_census … held` row on a channel that never entered the model set.  Ladder: the `rv14on` row of
§10.7b.  Peak excursion **+19.2 → 0.0 TECU**, and the +7.1 TECU permanent scar with it — the
`rv14hold` outcome of §10.8, but with **70 of 72 re-entries still admitted** instead of 0 of 72.

**And on the other leg, the whole class-B window flattens:**

```
TOW        273180  273270  273330  273360  273450  273600  273690  273840  274200
rv04off     32.88   36.29   41.94   43.56   36.68   34.99   34.64   34.44   33.81
rv04lbl      32.88   36.29   41.94   39.23   35.02   34.11   33.87   33.77   33.42
rv04on      32.89   33.37   32.79   32.76   32.60   32.89   32.83   32.95   32.94
```

`rv04on` never leaves 32.6–33.4 against a 32.88 pre-event level.  Both the +10.7 excursion and the
residual level shift are gone; the OFF arm is still 0.9 TECU above baseline at 274200, the vet-bound
arm is at baseline.

| TOW | cell | \|z\| | lag | trigger |
|---|---|---|---|---|
| 272340 | G14@BSHM | 218.5 | `gap_ep=10` | `vet_bound` |
| 273270 | G13@BSHM | 308.5 | resume+1 | `vet_bound` |
| 273450 | G22@BSHM | 347.6 | resume+0 | `vet_bound` |
| 307740 | G16@BSHM | 670.3 | resume+0 | `vet_bound` |

All four refusals attributed to the vet bound and none to the outer one.  G14 is a cell no earlier
pass had catalogued.  Cost: 153 vs 140 channel-epochs withheld (**+9.3 %**), 121/121 epochs emitted.

**The census contributed nothing once the bound was armed: 0 refusals of 616 consultations (rv04on),
0 of 367 (rv14on).**  §10.10's disjoint-coverage claim is retracted — see the correction there.  The
bound reaches G13 at resume+1 where the census reached it at resume+4, and G22 at resume+0 where the
census reached it at resume+5.

#### The defect this gate found, and the fix

The first `rv04on` pass produced a **flicker** the unit suite had no case for:

```
273450  adm_reject_reentry  G22  z=347.6  trigger=vet_bound
273600  adm_heal            G22  z=782.2  readmit            <- readmitted at 782 m
273630  adm_revet_refuse    G22  z=888.5  trigger=vet_bound
```

The heal path tested `fail` — the **absolute** 1000 m bound — so a channel held out at 200 m
accumulated "clean" epochs at anything under 1000 and was readmitted *above the bound that had just
refused it*.  The bad readmission was visible in a00 (32.60 → **37.54** → 34.23).  An admission bound
that lets a channel back in above itself is not a bound.  Two-part fix: (1) the re-test uses the same
bound that made the refusal — `fail || (vet_left > 0 && fail_vet)`; (2) **a withheld epoch no longer
spends vet budget** — `vet_left` ticks down only on epochs the channel was ADMITTED, otherwise a
channel being held out serves its window in absentia and returns unvetted, the state INC-4 exists to
abolish.

After the fix G22 is refused at 273450 and stays out; no heal, no re-refusal, and a00 across 273600
reads 32.89 instead of 37.54.  Counters move as predicted: vet-bound refusals 4 → 3 (the spurious
re-refusal gone), `revet_window_ep` 784 → 753 (held epochs no longer counted), windows completed
74 → 71.  Pinned by `VetBoundRefusalHealsAgainstTheVetBoundNotTheOuterBound`; suite now **25 cases**.

#### The separation regime is NOT the parent card's

Stated plainly because §2's rhetoric does not transfer.  `adm_bound_m` enjoys four empty decades
(§9.13: nothing between 54.3 m and 194 621 m).  The **vet** bound does not: healthy maximum 61.9 m,
lowest refusal G14 at 218.5 m — a **3.5×** separation, with the corrupt population extending down
toward the threshold rather than sitting four decades above it.  G14 clears 200 m by only 1.09×.  That
is a materially different risk posture and it is the main reason this knob should not be promoted on
two legs.

### 10.9 Status at hand-off

**Built and gated**: knobs + loader (with the INERT-guard and the prefit-walk-absent warn), the
cross-seam census flag, the seam verdict, eight counters, two events (`adm_reject_reentry`,
`adm_revet_refuse`), run-end block with three invariant-#4 warns, five manifest fields (conditional on
`adm_revet` alone, so an unarmed-vet run's manifest is unchanged), and ten unit cases taking the suite
to 22 — including `DisarmedVetStillInheritsTheStaleVerdictAsAnOnset`, which pins the DEFECT so the OFF
path stays honest.  Unit: **1098/1099** Debug (the one pre-existing fixture skip), 172 suites, the
one-file-per-suite invariant intact.

**Verdict by class**, at the shipped defaults (`adm_revet_bound_scale` 0.2):

| | label-only (scale 1.0) | **shipped (200 m vet bound)** | `adm_revet_hold` |
|---|---|---|---|
| Class A (12 mislabels) | CLOSED — relabelled, response byte-identical, zero cost | **CLOSED** | closed |
| Class B (4 exposed) | PARTIAL — 2 of 3 pre-empted by 1 and 3 epochs; G16 ties | **CLOSED** — every cell refused at `resume+0`/`+1`; a00 flat on both legs | CLOSED |
| cost (channel-epochs withheld, rv04) | 144 (+2.9 %) | **153 (+9.3 %)** | — |
| cost (rv14, re-entries admitted) | 72 / 72 | **70 / 72** | 0 / 72 |

The shipped posture reaches the `adm_revet_hold` outcome at a small fraction of its withholding, which
retires `hold` as the recommended answer and leaves it as a backstop for a run with no census AND a
fault below the vet bound.

**Owed, in priority order:**

1. ~~The re-entry |z| measurement~~ — **DONE, §10.10**; ~~build `adm_revet_bound_scale`~~ — **DONE and
   gated, §10.11.**
2. **A 24 h arm at the shipped defaults against `runs/ft63_24h_conv`.**  The single highest-value owed
   run, and the reason is §10.11's separation result rather than the a00 result: the vet bound operates
   at **3.5×** over the healthy maximum (61.9 m vs G14@BSHM's 218.5 m), not the four empty decades
   `adm_bound_m` enjoys.  Two legs of 272 epochs cannot bound a false-positive rate in that regime.
   The headline to read is the run-end split line — the control scored 0 re-entry refusals vs 17
   onsets, of which 16 were re-entries in all but the label.  A 24 h `adm_revet_hold` arm is NO LONGER
   worth its own run: §10.11 reaches the same a00 outcome at a fraction of the withholding.
3. **A 24 h measure-only arm at `adm_bound_m` 50** (solve-inert, so it can ride along with anything) to
   extend §10.10's `n = 0` healthy-re-entry result from 272 epochs to a full day.  It is the direct
   evidence for the bound the previous item is really testing.
4. **σ̂₀ for gate 6.**  The `sigma0` epoch-status column reads 0.0000 on every arm here, the same gap
   §9.14 records against gates 6/7.  Until it has a source, no INC-4 arm can be QC-graded, and no
   promotion decision should be made on a00 alone.
5. **The 17th onset.**  G07@BSHM at TOW 329640 band 2 is the one mid-arc onset with no `arc_resume`
   within five minutes — the second band of a channel whose band 1 had already been refused.  Benign on
   this reading, but the only member of the population this increment does not explain; confirm rather
   than assume.

No POSTURE row and no ledger entry were added, per §5's no-promotion rule.

### 10.10 The re-entry |z| measurement (§10.4's owed run): **DONE**

`runs/ft63_adm_bz14` / `bz04` — MEASURE-ONLY (`adm_screen` off) with `adm_bound_m` walked down to
**50 m**, on the same two snapshots as the gate-2 legs.  272 leg-epochs, 63 stations, 49 `arc_resume`
cells.  Re-entry attribution is done offline against the `T0_gap arc_resume` stream — the same evidence
§10.1 used — so the measurement stays independent of the mechanism it parameterizes.

**Reading a measure-only arm as though it were a screened one is legal, and it is demonstrated rather
than argued.**  The statistic is state-free (`d = observed − modeled`, and `modeled` carries range,
product satellite clock and the deterministic corrections with no filter state in it), so the same cell
must read the same z under any solve.  G21@BSHM at TOW 306480:

| arm | event | z |
|---|---|---|
| `rv14off` (screened) | `adm_reject_birth` | **75334.778708** |
| `rv14lbl` (screened + vet) | `adm_reject_birth` | **75334.778708** |
| `rv14hold` (screened + strict hold — a materially different solve) | `adm_reject_birth` | **75334.778708** |
| `bz14` (measure-only) | `adm_census` | **75334.778708** |

Nine significant digits across four arms, one of which withholds 2.6× as many channels.  That is the
state-freedom claim of §9.1 measured end-to-end, and it is what makes this a cheap measurement.

#### The result: the healthy re-entry population does not reach 50 m

**Not one healthy station's re-entry cell cleared 50 m in either leg.**  Every >50 m re-entry cell is
BSHM00ISR, and the picture is sharper than expected — **three of the five are corrupt at `resume+0`,
i.e. on arrival**:

| cell | |z| at re-entry | lag | reached by |
|---|---|---|---|
| G16 band 1 (TOW 307740) | **670.3 m** | resume+0 | nothing today — this is the +19.2 TECU cell |
| G22 band 1 (TOW 273450) | **347.6 m** | resume+0 | census, at resume+5 |
| G13 band 1 (TOW 273240) | **81.5 m** | resume+0 | census, at resume+4 |
| G15 band 1 (TOW 274140) | 300 649.8 m | resume+0 | the 1000 m bound already |
| G21 band 1 (TOW 307680) | 62 496.4 m | resume+3 | the 1000 m bound already |

G16's 670.3 m is the number §10.4 extrapolated at "~715 m".  The extrapolation was right, and it is
now a measurement.  Against that, the healthy tail — every non-BSHM cell over 50 m in either leg, all
steady-state, none at a re-entry:

```
61.9  COCO00AUS G24     55.2  NIUM00NIU G03     50.5  COCO00AUS G23
58.1  COCO00AUS G24     54.1  DARW00AUS G23     50.4  COCO00AUS G24
57.1  COCO00AUS G24     50.9  COCO00AUS G24     50.3  NIUM00NIU G03
55.7  COCO00AUS G24     50.7  NIUM00NIU G03     50.1  STHL00GBR G08
```

A coherent population — tropical / low-elevation stations, repeat offenders, **topping out at 61.9 m**.
The per-epoch `adm_sta_worst_z` column agrees: p50 13.6 / 11.7 m, p99 17.1 / 13.8 m across all 272
epochs.

#### Verdict

| vet bound | margin over healthy max (61.9 m) | measured false positives | class-B cells refused at `resume+0` |
|---|---|---|---|
| 100 m | 1.6× | **0** | G16, G22 |
| **200 m** | **3.2×** | **0** | **G16, G22** |
| 300 m | 4.8× | **0** | G16, G22 |
| 1000 m (today) | 16× | 0 | none |

**~200 m is the supported choice** and it closes the case that mattered: G16 at 670.3 m is refused *on
its return epoch*, so the +19.2 TECU excursion and the +7.1 TECU scar never start — the
`adm_revet_hold` result of §10.8, without withholding a single healthy re-entry.

**⚠ A "disjoint coverage" conclusion was drawn here (census and vet bound cover disjoint failure
shapes, neither subsuming the other) and it is RETRACTED — §10.11.**  The error was reading |z| **at
`resume+0` only**: G13 is at 81.5 m on arrival and at **308.5 m one epoch later**, so a vet bound that
stays live across the window — which is how it was built — catches it at `resume+1`, three epochs
*before* the census could.  Measured in §10.11: with the vet bound armed the census made **0 refusals
out of 616 consultations**.  **On this data the bound strictly dominates.**  What survives: the census
remains the sole cover for a cell that stays under the vet bound for its entire window, which nothing
in this sample does.

#### Caveats, stated

- The 50 m floor means the healthy re-entry distribution **below** 50 m is not observed.  Fine for
  setting a bound above 62 m and useless for setting one below it.
- The per-cell 20-epoch event holdoff can suppress a repeat exceedance (it hid G16's own 307800 row, 2
  epochs after its 307740 one).  A cell that was quiet before its gap and loud on return is always
  emitted, which is the case this measurement is about — but a *chronically* over-50 m cell that also
  re-enters could be undercounted.  Those are not clean re-entries by construction.
- Two legs, 272 epochs, one day, one network.  `n = 0` healthy re-entries over 50 m is a strong result
  on this sample; it is not "never".  A 24 h measure-only arm at the same 50 m bound would cost one run
  and settle it — and unlike the screened arms it is solve-inert, so it can ride along with anything.
