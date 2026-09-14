# Cards 1f + 1g — the TRUNCATED-ROW gauge defect and the reinit-source widening — the positive control, the validation ladder, and the 2×2 isolation

**STATUS**

- **id:** Cards 1f + 1g
- **topic:** network-vehicle
- **status:** closed
- **summary:** the TRUNCATED-ROW gauge defect and the reinit-source widening — the positive control, the validation ladder, and the 2×2 isolation
- **dates in the text:** 2026-08-15 → 2026-08-19
- **knobs / posture:** `network.carry_coupling_gauge_project`, `network.carry_coupling_reinit_sources`, `network.mw_wl.bridge_sample_debias`, `network.carry_conditional_coupling`, `network.carry_invariant`, `network.shared_carry_gauge_free`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/s24cpl_`, `runs/fld50_`
- **source:** `docs/archive/SPEC_card1_coupling_carry.md` — 13, 13.1, 13.2, 13.3, 13.4, 13.5, 13.6, 13.7, 13.8, 13.8.1, 13.9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_card1_coupling_carry.md -->
## 13. Card 1f / 1g — the TRUNCATED-ROW gauge defect, and the reinit-source widening

**Landed 2026-08-15, both default-OFF.** `network.carry_coupling_gauge_project` (1f) and
`network.carry_coupling_reinit_sources` (1g).  Origin: the carry↔datum handover theory
campaign (memory `project_carry_datum_handover_theory`), fix-program items A and B.

### 13.1 The defect (1f)

A harvested coupling row is gauge-invariant by the regression identity
`Mᵀg_s = −g_r(c)`: it observes each datum gauge **exactly as much as its own `A_r = +1`
local requires**, and no more.  §1's measured null directions record the concrete value —
a Galileo ambiguity's row has **`Σ_E(shared) = −1` exactly**, because the E gauge moves E
ambiguities by +1.

TRUNCATION breaks the identity.  A DEPARTED column is marginalized by **deleting** its
coefficient (`NetworkCoupling.cpp`, the `c < 0` branches), which leaves
`Σ_family M = −M_dep` — so the row becomes a high-weight observation of the **current**
gauge pinned at the **old** realization.

**Measured (simnet24, TOW 305220, seven G-clock departures in one epoch):** smallest
null-space eigenvalue **1e-11 → 9.43e-3**, five decades over the rank-gate cutoff and
level with the weakest genuine phase-bias layer ⇒ **4 spurious OVER-specified withholds**,
self-healing only at 120 s = `pl_max_gap_s` by blanket carry expiry (a silent mass
re-cold-start).

### 13.2 Why "annihilate the family sum" is the WRONG fix

The obvious repair — project the coefficient vector onto the complement of the null basis
so the family sum is zero — is wrong for exactly the rows the carry consists of.  The
target is `−g_r(c)`, not `0`; annihilating it would **inject** a gauge observation on
every row whose local carries gauge weight, which §1 measured to be the normal case for
an ambiguity row.

The correct target is the **harvest** sum, and it needs no null basis at all: at build
time `lc.coupling` still holds every harvested entry, present and departed, so
`Σ_all coeff` **is** the harvest sum (up to the 1e-8 relative sparsify threshold).  This
also means **no measured eigenvector basis `V` is required** — which matters, because the
rank gate is `EigenvaluesOnly` and no eigenvector solve of the border runs on a default
config, a stashed `V` would be one epoch stale, and on a truncation epoch the previous
epoch's near-null set is precisely the one **missing** the direction that got filled.

### 13.3 What 1f actually does

It redefines Card 1d's deficit.  `tuGaugeRestore` restored
`Σ_present(m_un − m_shr)` — the κ shrink alone, blind to deletion.  1f seeds the same
per-(constellation,kind) deficit map with the truncated columns' coefficients, so the
restored quantity becomes the **harvest** sum.  Consequences:

- it fixes departures and the κ shrink in **one** expression;
- it reduces **exactly** to Card 1d when nothing was truncated (so the OFF path and the
  no-departure ON path are byte-identical);
- the y-correction is automatic — the post-restore emission loop already accumulates
  `rhs += m_shr · s_prev` with the corrected coefficient;
- it is frame-agnostic: restoring each family sum to its harvested value requires no
  knowledge of *which* combination is the true null direction.

With κ OFF the present terms are routed through the same buffer (unshrunk, `m_shr = m_un`)
so the restore can run; iteration order is preserved and departed terms never touch `rhs`,
so that re-routing is bit-identical.

**A family with truncated mass but NO present recipient** has nowhere to put the deficit.
Those rows stay gauge-dirty and are counted (`cpl_gauge_orphan`) rather than
half-corrected — invariant #4: the miss is telemetry, not a silent partial fix.

### 13.4 The 1g half

`tuReinitSince` was gated on `carry_cpl_tu`, and `tu_reinit_hist` was fed **only** by
clock-QC sources (`prev_carry.inflate_q`, clock refloats).  **No realization-step source
reached it.**  A phase-anchor handover, an eq34 chain re-reference or a datum-slip PB
re-realization each erase the re-referenced family's *own* carry, but every *other*
carried local keeps a `coupling[]` entry on that column holding a **retired-frame
`s_prev`**, folded at full weight — the stale-`ŝ_prev` wire-poison channel (30 PBGAUGE +
27 CLK_STEP re-realizations in 120 epochs).

1g keys the history `(kind, sat, signal)` — an empty signal being the **sat-wide
wildcard**, which is what the clock-QC feeds write and what preserves their existing
kind-wide matching — feeds it from `phase_anchor_reref` / `eq34_reref` /
`datum_reref_pb_sats`, and honours it independently of κ.  Re-realized columns then take
the departed route (κ=0), which is the only correct treatment for an **unknown** step;
1f then repairs the deficit that marginalization opens.  **The two are designed to be
armed together** — arming 1g alone trades a stale frame step for a gauge observation, and
the loader warns.

### 13.5 First armed run — WIRING PROVEN, EFFECT UNGRADED

`config/test_suite/_card1f_wiring_probe.json` (= `_dcmp_50sta_10ep_cpllive.json` + the three
knobs; 50 sta, 11 epochs, x64-Release, 2026-08-15).  This is a **wiring probe, not a
validation** — 11 epochs cannot grade any estimation channel.

| arm | departed terms | 1f held / orphan | C-GF downdated / declined | inv-#7 R max |
|---|---|---|---|---|
| baseline (all three OFF) | **31** | — | — | 2.191e-06 |
| 1g only (departures, NO repair) | **356** | — | — | 2.191e-06 |
| 1f + 1g + C-GF | **415** | **310 / 0** | **310 / 10** | 2.554e-06 |

What this establishes:

- **All three cards FIRE** (invariant #4 satisfied): 1f held 310 family sums with **zero**
  orphans; C-GF downdated 310 keys with **zero** inert epochs and **declined 10** directions
  for partial support, so the closure test is doing real refusals rather than being
  decorative.
- **1g does what it claims, and the attribution closes exactly**: departures 31 → 356 on the
  knob alone, and the card's dedicated counter reports **384 terms diverted by the widened
  (kind,sat,signal) probe** on the all-armed arm — against an observed departed-term increase
  of 415 − 31 = **384**.  The two are independent measurements of the same quantity and they
  agree to the term, so the mechanism is precisely attributed rather than merely correlated.
  Those 384 terms were previously folded at FULL WEIGHT in a retired frame.
- **The invariant-#7 counter is calibrated**: R ∈ [0, 2.6e-06] on a healthy config, i.e. the
  declared gauge carries ~six decades less information than the gate's own cutoff.  A run
  whose datum genuinely owns its gauge now says so positively.

⚠ At this scale the 1g-only arm drove 356 UNREPAIRED departures and the ratio did **not** move
(2.191e-06, identical to baseline).  That was NOT evidence the counter cannot fire — see §13.7,
where it fires at 7.1e+06 in the regime the defect was actually measured in.  **The window, not
the scale, was the problem**: 11 epochs from a cold start is far too short for the carry to age
into the failure.  §13.8 shows the same 50-station lineage breaching at 1.557e+03 once it runs a
full hour, and 305 stations breaching at 7.537e+03.  ❌ A hypothesis that dense rosters make
departures rare (raised from this same 11-epoch probe, which measured ZERO departed columns at
305 sta) is **FALSIFIED**: over 1 h departures scale UP with station count — 14 989 @50,
111 599 @150, 284 618 @305.  Never characterise this defect from a short cold-start window.

Arms: `_card1f_wiring_probe.json` (all three) and `_card1f_negctl.json` (1g only, no repair),
both derived from `_dcmp_50sta_10ep_cpllive.json` by adding knobs and nothing else.

### 13.6 Defects found by the post-implementation review (all fixed)

Two adversarial passes (39 candidate findings → 11 surviving independent refutation).  Recorded
because each is a *class* that will recur in the next card of this family.

0. **MAJOR — a fix that added a parameter and never read it.**  The withdraw primitive
   `refoldSharedPriorClockGaugeFreeDropping` gained `require_closure` in its signature — and
   the callers passed it in good faith — while the body ignored it, so the withdraw rebuilt
   and subtracted the GAUGE-FREE block for a group the fold had (correctly) folded PLAINLY.
   Consequences were PSD-preserving but real: a partial refloat that retains ~1/n of the
   refloated satellite's prior, survivors re-anchored against a mean they should not see,
   and — on a lone-clock group — a `cap_priors` desync in which the caller erased a prior as
   "withdrawn" while it was still folded.  *Class: threading a flag through a signature is
   not threading it through the behaviour; grep the body, not the call sites.*

1. **CRITICAL — the closure test dropped the carry instead of declining the downdate.**  On
   the DIAGONAL gauge-free path the loop body is what ACCUMULATES the priors, so `continue`
   on a closure failure folded **no prior at all** for the whole group, silently discarding
   carried clock information.  The dense path has no such hazard (Λ already holds those rows;
   closure only removes a column of `U`).  Fixed by folding the group PLAINLY on decline.
   *Class: when two paths "mirror" each other, verify what the loop body OWNS, not just what
   it decides.*
2. **CRITICAL — `require_closure` reached the FOLD sites but none of the WITHDRAW/REPLAY
   sites.**  Closure selects WHICH directions `U` spans, so it is part of "what was folded"
   exactly as `clock_gauge_free` is — and the header already said that flag "MUST match the
   flag the original fold used".  Threading one and not the other reintroduces the
   negative-semidefinite residue this card exists to prevent.  Fixed at all four
   withdraw/restore sites plus both replay folds.
3. **MAJOR — the breach set latched across un-evaluated epochs.**  A direction whose
   constellation dropped out of the layout kept its breached flag forever, so its next breach
   fired no edge.  That is invariant #7's own failure mode (a returning member read as a
   continuing one).  Fixed by emitting a `ddi_clear` edge for any latched direction the epoch
   did not evaluate.
4. **MAJOR — the run-end advice named a fold the counter cannot see.**  The ratio is measured
   on the reduced normals AS THE GATE SEES THEM: after the station blocks (which carry the
   coupling rows) and the pre-gate datum rows, but **BEFORE** the D2/C1 dense carry fold and
   the clock-osc prior.  The advice listed the D2/C1 fold as a candidate it could indict.
   Corrected to state the observable window explicitly.
5. **MINOR** — the partial-skip tally was epoch-scoped and read at the last fold site, so it
   under-counted the withdraw and replay folds that run later in the epoch; it is now
   run-scoped on the estimator and read once at teardown.  Plus three missed
   `cta_gauge_free` → `carry_gauge_free` replacements and one dangling doc reference.

⚠ **ONE DELIBERATE BYTE-IDENTITY EXCEPTION, and it matters for A/B methodology.**  Everything
above is byte-identical with the new knobs off — *except* the within-epoch RAIM replay, whose
BODY changed unconditionally.  It previously re-folded the captured priors with the plain
accumulator even when the primary solve had folded them gauge-free, so on a config running
`clock_anchor_gauge_free` **+** `raim_within_epoch` the replayed system was not the system
being tested and the RAIM trip decision was taken against normals containing information the
primary solve had removed.  That was a defect, and fixing it necessarily moves those configs.
**No fingerprint token discriminates the old from the new behaviour**, so an A/B that spans
this commit on such a lineage is not a single-variable comparison — re-baseline it.  Configs
that arm neither knob, and configs that arm the anchor without the within-epoch tier, are
unaffected.
### 13.7 POSITIVE CONTROL — simnet24 at TOW 305220 (2026-08-15).  **PASS.**

The pre-registered test of §13.5, executed.  Single-variable A/B over
`_simnet24_inj_pareref_on` with the Card-1 coupling made LIVE
(`carry_conditional_coupling` + `shared_carry_full_cov` + `carry_coupling_time_update`,
`carry_invariant: false`); the two arms differ **only** in `carry_coupling_gauge_project` and
`carry_coupling_reinit_sources`.  Configs `config/test_suite/_s24cpl_{off,on}.json`, runs
`runs/s24cpl_{off,on}`, x64-Release.

**The OFF arm reproduces the documented defect exactly.**  Four OVER-specified gate withholds,
at TOW **305220 / 305250 / 305280 / 305310** — the first is the epoch named in the theory
campaign, and the four consecutive 30 s epochs span exactly the **120 s = `pl_max_gap_s`**
blanket-carry-expiry self-heal the card predicted.  This is the first time that defect has been
reproduced on demand rather than observed in passing.

| | OFF (defect) | ON (1f+1g) |
|---|---|---|
| epochs emitted | 116 / 120 | **120 / 120** |
| OVER-specified withholds | **4** | **0** |
| invariant-#7 R max | **7.123e+06** (CLK\|G, TOW 305310) | **2.086e-03** |
| breach epochs / edges | 4 / 2 | **0 / 0** |
| observed near-null min..max | 0 .. 1 | **1 .. 1** (= declared, every epoch) |
| `ddi_over_breach` | **4 of 4** | 0 |
| 1f held / orphan | — | **560 / 0** |
| 1g diverted | — | **507** |

R falls **9.5 decades**, from far above the gate cutoff to far below it.  Every one of the four
withholds coincided with a breach, so the new run-end advice correctly refused to say
"re-calibrate" and instead named `network.carry_coupling_gauge_project` — the fix that in fact
resolves it.  The instrument, the diagnosis and the remedy agree.

**Quality, graded against injected truth on MATCHED epochs.**  The ON arm's four extra epochs
were removed so the populations are identical (n = 5502 code-bias residuals, 1776 phase):

| channel | OFF | ON (matched) |
|---|---|---|
| code-bias residual rms | 0.4306 m | 0.4347 m (+0.9 %) |
| code-bias residual mean | −8.5e-04 m | **−4.4e-04 m** |
| sum-to-zero datum check | mean −8.53e-04, rms 4.60e-03 | **mean −4.36e-04, rms 2.91e-03** |
| phase SD fractional rms | 0.269–0.293 cyc | 0.282–0.288 cyc |

Read honestly: code-bias quality is **FLAT** (+0.9 % on a channel whose rms is dominated by a
heavy-tailed C5Q at 0.75 m), the **datum realization is 1.6× tighter**, and the phase channel
sits at 0.269–0.293 cyc in BOTH arms against a uniform-random rms of 0.2887 — i.e. the
fractional phase bias is unresolved either way and that column carries no signal for this A/B.
The four recovered epochs are genuinely harder than average (their inclusion moves the
unmatched rms 0.4347 → 0.4412) but are usable, not garbage.

**Verdict: the defect is real, reproducible, and fixed, at no measured quality cost and with a
measurably better-realized datum.**  ⚠ Scope: **n = 1**, 24 stations, GPS-only, simulated, 1 h,
one lineage.  Not graded at 63/150/305, not on field data, not multi-constellation, not warm.
`network.mw_wl.bridge_sample_debias` was validated at 50 stations and is net-HARMFUL at 305 —
that is why this is registered **RECOMMEND_SCOPED**, not RECOMMEND_ON.

### 13.8 THE VALIDATION LADDER — 24 (sim) / 50 (field) / 150 (field).  **PASS at every point.**

Three single-variable A/Bs, defect present in every OFF arm and absent in every ON arm.

| arm | stations | data | withheld OFF → ON | inv-#7 R max OFF → ON | departed OFF → ON |
|---|---|---|---|---|---|
| `s24cpl` | 24 | sim (truth-graded) | **4 → 0** | 7.123e+06 → 2.086e-03 | 3 308 → 1 334 |
| `fld50`  | 50 | **real IGS** | **20 → 0** (16.5 %) | 1.557e+03 → 3.148e-06 | 14 989 → 38 771 |
| `s150`   | 150 | **real IGS** | **20 → 0** | 7.807e+02 → 9.254e-06 | 111 599 → 6 393 |
| `s305`   | **305** | **real IGS** | **20 → 0** | 7.537e+03 → 1.664e-05 | 284 618 → 15 131 |

(`fld50` is the one rung where departures RISE with the fix, because it is the only field rung
where Card 1g was live — 37 441 diverted terms are added departures by construction.  At 150/305
1g diverted nothing, so only the cycle-breaking drop shows.)

Two results the ladder adds beyond §13.7:

- **The field defect is ~5× more prevalent than the sim one** (16.5 % of epochs vs 3.3 %), so the
  sim control UNDER-states the field payoff rather than over-stating it.
- **The withhold→expiry→more-departures cycle is real and the fix breaks it.**  At 150 stations
  departed coupling terms fell **111 599 → 6 393 (17×)** with the knobs on.  The OFF arm's
  withholds age the carry past `pl_max_gap_s`, blanket expiry then manufactures a fresh crop of
  departed columns, and those breach the gauge again.  That is the "silent mass re-cold-start"
  the theory card named, observed closing on itself.
- ⚠ **At 150 stations Card 1g diverted ZERO terms**, so the entire 150-sta improvement is
  attributable to **1f alone**.  Cause (measured, now a loader warning + a ledger interlock):
  1g's realization-step feeds are populated ONLY by `phase_datum_eq34` or
  `phase_anchor_carry_reref`, and that lineage ran `phase_datum_prioritized` with neither.
  Diverted counts across the ladder: 507 (sim, pa_carry_reref) · 37 441 (50-sta, eq34) ·
  **0** (150-sta, neither).

**QUALITY — improved on both available instruments, not merely neutral.**

| instrument | arm | OFF | ON |
|---|---|---|---|
| sim truth grade (matched epochs) | s24cpl | code-bias rms 0.4306 m | 0.4347 m (+0.9 %, flat) |
| sim sum-to-zero datum check | s24cpl | mean −8.53e-04, rms 4.60e-03 | **−4.36e-04, 2.91e-03** (1.6× tighter) |
| FIELD datum-invariant wire step | fld50 | **5.631 cyc/ep** (1.2549 m/ep) | **4.445 cyc/ep** (0.9983 m/ep) — **−21 %** |

The field wire number is the pivoted between-satellite step from `scripts/grade_wire.py` (the
Psychas single-realization quantity).  RAW rises 7.470 → 8.365 cyc/ep, but RAW deliberately
includes the benign common-mode frame drift the pivot exists to remove — grading on it is the
error that grader was written to prevent.  The ON arm achieves the lower pivoted step *while also
emitting 20 more epochs*, i.e. while covering the harder epochs the OFF arm withheld.

⚠ **A first field wire grade was DISCARDED as invalid**, and the reason generalises: the trace
CSVs land in a log dir derived from the CONFIG path (`./logs/`), NOT next to the exe, and they are
overwritten by every run.  An initial snapshot helper pointed at `build/x64-Release/logs/` and
silently captured a **two-day-old fossil** — four different arms produced byte-identical 85 442-byte
files, which is what exposed it.  Always verify a captured trace differs between arms before
grading it.

**COST — none.**  305-station arms: 15.4 min OFF vs **14.7 min ON**.  The fix is marginally FASTER,
because holding the gauge suppresses the withhold→carry-expiry→more-departures cycle and there is
less marginalization work to do.

### 13.8.1 FIELD ARM detail — 50 real IGS stations, 1 h, DOY-129

The arm §13.7 said was owed.  Real observations (not the synthetic fixture), 50 stations,
1 h at 30 s, GPS, coupling LIVE (`carry_invariant: false` + `phase_leveling`), single-variable
A/B on the two knobs.  Configs `_fld50_{off,on}.json`, runs `runs/fld50_{off,on}`.

| | OFF | ON |
|---|---|---|
| epochs emitted | 101 / 121 | **121 / 121** |
| gate-withheld (all OVER-specified) | **20  (16.5 %)** | **0** |
| invariant-#7 R max | **1.557e+03** | **3.148e-06** |
| breach epochs / edges | 20 / 1 | **0 / 0** |
| coupling terms present / departed | 4 339 142 / 14 989 | 5 340 791 / 38 771 |
| 1f held / orphan | — | **15 852 / 0** |
| 1g diverted | — | **37 441** |

**The field defect is five times more prevalent than the sim one** — 16.5 % of epochs withheld
against 3.3 % on simnet24 — and the fix recovers all of them.  R falls **8.7 decades**.  Note
the breach is ONE sustained era (20 breached epochs, a single edge), not 20 independent events:
the field carry sits in the filled-null state for a stretch and the gate withholds throughout,
which is exactly the "silent mass re-cold-start" shape the theory card described.

⚠ **Quality on the field arm is NOT graded here.**  Field data has no truth, so the available
instrument is the datum-invariant wire step (`scripts/grade_wire.py`).  A first attempt was
DISCARDED as untrustworthy: the trace CSVs are written to a SHARED `build/x64-Release/logs/`
that the next run overwrites, and a concurrent 305-station job was writing there at the time.
Re-run both field arms back-to-back with `scripts/_arm_run.sh` (which snapshots and VERIFIES
the capture) before quoting any wire number.  Availability and the gauge ratio above are
unaffected — they come from each run's own captured `run.log`.

**Promotion (2026-08-15).**  Registered in `docs/CONFIG_promotion_ledger.json` (1f and 1g
RECOMMEND_SCOPED with the regime `on("network.carry_conditional_coupling") and not
on("network.carry_invariant")`; C-GF UNVALIDATED — it was NOT varied in this A/B) and as
loader POSTURE rows in `GnssConfigNetworkPosture.cpp`, so every in-regime run that leaves them
off now says so at startup with the evidence.  Loader↔checker parity verified
(`config_posture_check.py --verify-loader`).  Code defaults deliberately UNCHANGED: both knobs
are fingerprinted, so flipping a default invalidates every stored snapshot — a migration card,
not an edit.

⚠ One defect was found and fixed BY this run: the counter's range-error sentinel was
overloaded onto the sign of the accumulating quadratic form, and since `N`'s off-diagonals
are freely negative, an ordinary partial sum aborted the accumulation.  First armed run
reported **11 epochs, 0 direction samples** — the exact "armed and inert" signature invariant
#4 exists to surface.

### 13.9 THE 2×2 ISOLATION — 1f and 1g each carry a DIFFERENT half of the win (2026-08-17)

§13.8's open item ("1g's INDEPENDENT effect is unisolated"), closed with a full 2×2 factorial
on BOTH lineages, one build (post-§14-fix — structurally inert here, shift door unarmed on
both):  field `_fld50_{00,1f,1g,11}` (eq34, 50 real IGS sta, 1 h) and sim `_s24cpl2_{00,1f,1g,11}`
(pa_carry_reref, 24 sta, truth-graded).  Every config certified single-knob against its §13.8
parent, and the corner cells REPRODUCE the stored §13.8 arms **byte-for-byte** (products.txt
and network_solution.csv identical for 00↔off and 11↔on) — the two intervening commits are
certified inert on these lineages and the 2×2 sits exactly on the ladder baseline.

**Field (matched-epoch wire = the 101 epochs every cell emits):**

| cell | withheld | inv-#7 R max | departed | diverted (1g) | held (1f) | pivoted wire step |
|---|---|---|---|---|---|---|
| 00 | 20/121 | 1.557e+03 | 14 989 | — | — | 5.631 cyc/ep |
| 1f | **0** | healthy | 892 | — | 667 | 5.543 |
| 1g | 20/121 | **1.030e+04** | 72 019 | 54 662 | — | **1.532** |
| 11 | **0** | healthy | 38 771 | 37 441 | 15 852 | 3.935 |

**Sim truth (matched to the common 116 epochs — n identical in every cell: clock n=1776,
code-bias n=5502):**

| cell | withheld | clock CONTRAST rms (datum-free) | clock max abs | code-bias rms | datum-check rms |
|---|---|---|---|---|---|
| 00 | 4/120 | 4.157 m | 17.75 m | 0.4306 m | 4.60e-03 |
| 1f | **0** | 4.688 m | 17.99 m | 0.4495 m | **2.85e-03** |
| 1g | 4/120 | **0.435 m** | 1.01 m | 0.4195 m | 4.65e-03 |
| 11 | **0** | **0.378 m** | 0.96 m | 0.4347 m | **2.91e-03** |

**Attribution — the two cards decompose almost cleanly:**

- **1f owns availability, gauge honesty, and the datum realization.**  Withholds → 0 in both
  1f-armed cells on both lineages; the invariant-#7 ratio collapses only when 1f is armed; the
  sum-to-zero datum check tightens 1.6× only in the 1f-armed cells.  On the wire it is NEUTRAL
  (5.631 → 5.543 matched-epoch, −1.6 %) and on truth channels ~flat (code-bias +3–4 %, inside
  the C5Q heavy tail; clock contrast +13 % alone / −13 % on top of 1g).
- **1g owns the product-quality win, and the channel is the CLOCK.**  Datum-free clock contrast
  rms 4.157 → 0.435 m alone (**~10×**, max |err| 17.8 → 1.0 m), 4.688 → 0.378 m on top of 1f;
  field pivoted wire step 5.631 → 1.532 alone (−73 %), 5.543 → 3.935 on top of 1f (−29 %).
  Mechanism: a coupling term whose column was RE-REFERENCED folds a retired-frame `s_prev` at
  full weight, and the injected error lands in the emitted clock/phase products; the divert
  removes exactly that.
- **The §13.8 quality attribution is CORRECTED**: the fld50 wire −21 % was implicitly credited
  to 1f (the promoted knob); the isolation shows the wire/clock gain is **1g's**, while 1f's
  quality contribution is the datum check plus emitting 20 more epochs at no wire cost.
  (§13.8's note that the **150-sta** rung's gains are 1f-alone stands — withholds/R are 1f's
  instruments, and 1g was inert there.)
- **Over-retention ADJUDICATED (the lane-A/B suspicion pattern):** 1g-alone's very quiet wire
  under a breached gauge (R 1.0e+04) looked like the "quiet prior / illegitimate pin" shape.
  The sim truth grade refutes that: the improvement holds on the **datum-free clock contrast**,
  which a pinned frame cannot fake.  The quiet is real product accuracy, not over-retention.
- **1g alone remains contraindicated**, now with measured teeth: it recovers zero epochs, and on
  the field eq34 lineage its unheld diverted mass makes the gauge breach **6.6× hotter**
  (R 1.557e+03 → 1.030e+04) while departed terms balloon 14 989 → 72 019.  Arm it WITH 1f.
- Caveats: the clock-truth instrument exists only in sim (one lineage, pa_carry_reref); the
  field eq34 lineage has no clock truth and its 1g R-worsening is lineage-specific (sim R was
  ~unchanged, 7.12e+06 → 6.91e+06).  1g stays SCOPED.

**Scale addendum (2026-08-17): the FULL 2×2 at 150 stations on eq34** (`_s150eq34_{00,1f,1g,11}`
— the `_s150` roster with the datum swapped prioritized→eq34, every cell certified single-knob
against its §13.8 parent; same snapshot build; the 00/1g cells emit the IDENTICAL 101-epoch
set and the 1f/11 wire numbers below are MASKED to that same set, so every wire comparison is
matched-epoch; all four solution CSVs pairwise distinct):

| cell | withheld | inv-#7 R max | departed | diverted (1g) | held (1f) | pivoted wire, matched 101 ep |
|---|---|---|---|---|---|---|
| 00 | 20/121 | 6.446e+03 | 98 866 | — | — | 10.788 cyc/ep |
| 1f | **0** | healthy | 5 644 | — | 3 272 | 10.633 |
| 1g | 20/121 | **1.473e+04** | 168 346 | 65 988 | — | 9.328 |
| 11 | **0** | healthy | 67 966 | 61 445 | 33 723 | **7.592** |

What replicates, what changes vs 50 stations:

- **1f's half replicates exactly**: withholds → 0, gauge healthy, departure cycle broken
  (98 866 → 5 644), and wire-NEUTRAL alone (−1.4 % here, −1.6 % at 50).
- **1g's with-1f wire contribution is SCALE-STABLE: −29 % at both scales**
  (50 sta: 5.543 → 3.935; 150 sta: 10.633 → 7.592).  That is the number to quote for the
  pair's quality win — not the 1g-alone figure, which is scale-dependent (−73 % at 50,
  −13.5 % at 150) because the eq34 baseline itself degrades with station count
  (10.788 vs 5.631 cyc/ep, R 6.4e+03 vs 1.6e+03, erases 3 740 vs 1 599) and the
  retired-frame population 1g removes becomes a minority of the churn.
- **The 50-sta anomaly does NOT replicate**: at 50, 1g-alone's wire (1.532) was quieter than
  the pair's (3.935) — under a breached gauge.  At 150 the pair is the QUIETEST cell
  outright (7.592 < 9.328), i.e. with the gauge held honestly there is nothing left that
  the breached-gauge quiet was "adding".  This strengthens the pair doctrine and retires
  any reading that 1g-alone's extreme quiet is desirable.
- **1g alone stays contraindicated at scale**: zero epochs recovered, R 2.3× hotter
  (6.6× at 50), departed +70 %.
- The divert→hold coupling between the cards is visible in the counters: 11's held count is
  ~10× 1f-alone's (33 723 vs 3 272; 24× at 50 sta) — 1g's diverted terms create the
  truncations 1f then holds.

Also noted in passing: on the IDENTICAL roster the eq34 datum baseline breaches ~8× hotter
than the prioritized datum (R 6.446e+03 vs §13.8's 7.807e+02) and carries a ~2× noisier
pivoted wire (10.788 vs the prioritized lineage's ladder regime) — a datum comparison, not a
knob verdict.

**Multi-constellation coverage arm (2026-08-19): GPS+Galileo at 50-eq34 — PASS, and the
ORPHAN channel is nonzero for the first time.**  `_fld50ge_{00,11}` = the §13.9 corner cells
plus `Galileo` in `active_systems` / `signal_priorities` ("CBXIQ") and nothing else, same
snapshot build; pair A/B (both knobs together — the per-knob attribution is §13.9's, this arm
is coverage).  This is the FIRST arm in which the Card-1f family machinery runs with two
(constellation,kind) family sets, i.e. the first execution of the branch the §1 mechanism's
own Galileo Σ_E = −1 example describes.

| cell | withheld | inv-#7 R max | departed | diverted (1g) | held (1f) | ORPHANED | pivoted wire, matched 101 ep |
|---|---|---|---|---|---|---|---|
| ge_00 | 20/121 | 4.132e+02 (CLK\|G) | 21 366 | — | — | — | 4.815 cyc/ep |
| ge_11 | **0** | healthy | 84 673 | 82 226 | 32 066 | **62** | **3.383 (−29.7 %)** |

- **PASS on every primary gate**: withholds → 0, gauge healthy, eq34 pin audit COMPLETE for
  all 10 Galileo signal groups (declared == folded == landed, every epoch), diverted/held live
  at multi-GNSS volume, §14 counter 0 alongside 0 v2-shifts (door unarmed, posture
  comparable).
- **The pair's wire contribution is CONSTELLATION-STABLE: −29.7 % matched-epoch**, the third
  lineage in a row at −29 % (50-GPS −29.0 %, 150-GPS −28.6 %).  Quote −29 % as the pair's
  quality number across every regime tested.  The multi-GNSS baseline itself is quieter than
  GPS-only (4.815 vs 5.631) and breaches softer (4.1e+02 vs 1.6e+03) — more satellites
  constrain both the frame and the datum directions.
- ⚠ **62 ORPHANED family sums — first nonzero in any arm** (every GPS-only cell measured 0).
  Structurally a multi-constellation phenomenon: a coupling row's Galileo family can lose ALL
  its present columns while its GPS family survives, leaving truncated mass with no present
  recipient; GPS-only rows in that state are simply not built.  This is the documented
  gauge-dirty residue the card counts honestly and cannot remove — at 62 / 32 066 held
  (0.19 %) it does not trip invariant-#7 here (0 withholds, no breach), but the channel is now
  MEASURED nonzero.  OWED before any multi-GNSS promotion tightening: per-epoch/per-family
  attribution of the orphans, and a decision on whether the residue needs a recipient
  fallback (e.g. holding against the row's OTHER surviving family is NOT obviously correct —
  the sums are per-constellation by construction because the datum realizes per
  constellation).
- Still owed for full multi-constellation coverage: per-knob isolation at multi-GNSS
  (this arm is pair-vs-off), GLONASS/BeiDou regimes (each has its own known hazards —
  see project_glonass_ifb_network, project_beidou_network_clock_divergence), and the
  sim-truth clock instrument with a second constellation.

**Warm-resume chain (2026-08-19): the pair survives the seam; the seam sheds history — PASS
with a quantified cost.**  `_fld50ws_{seed,resume,seamless}` on the 50-eq34 pair-ON lineage,
same snapshot build: seed = hour 1 + `state_snapshot_path` (canonical log-dir-relative
`network_state_snapshot.json`, 10 s interval, 113 KB — the base snapshot carries `local_carry`
values UNCONDITIONALLY, coupling left empty per the documented v1 shape;
`state_snapshot_carry` is the Card-4 blob and stays off here); resume = same +
`state_snapshot_require: true` + `start_offset_hours: 1.00417` (half-epoch back-off; the
offset fast-forwards the OBS READER while the snapshot warms the STATE — its "cold-starts the
carry" doc describes the no-snapshot case), seeded via `run_decomp_arm.py --seed`; control =
seamless 2 h.

- **INTEGRITY PASS**: require-armed restore succeeds (fingerprint round-trips WITH the
  `carrycplgauge=`/`carrycplreinit=` conditional tokens), the leg positions at exactly the
  next epoch (TOW 262830), emit counters continue chain-cumulatively (241 = 121+120 emitted —
  read run-end "emitted" as CHAIN totals on a resumed leg, not leg totals), **0 withholds, no
  invariant-#7 breach at the re-coupling edge, 0 orphans**, §14 counter 0 alongside 0 shifts.
- **Snapshot WRITING is solve-neutral**: the seed leg's solution rows are byte-identical to
  the seamless run's hour-1 rows (27 547 rows).
- **The pair re-ignites through the documented empty-coupling degrade**: leg-local held 9 886
  / diverted 27 588 vs the seamless control's hour-2 increments 12 043 / 31 067 (−18 % /
  −11 % — the coupling stash re-grows from empty over the hour).
- **The seam cost, quantified**: the resumed hour runs **+28 % eq34 re-references (339 vs
  265) and +45 % erases (2 343 vs 1 617)** vs the seamless same hour — the seam sheds
  leveling history.  Consequently the resumed wire reads **37 % QUIETER** (pivoted 1.639 vs
  2.599 cyc/ep on the identical 119-epoch set): **quiet-by-forgetting, NOT a quality gain** —
  each extra erase re-levels an arc and zeroes its accumulated fractional drift.  This is the
  standing "a warm-resume seam measurably erases the events" doctrine, now measured on the
  coupling pair specifically.  Corollary: never compare wire/event statistics across a seam
  boundary against a seamless arm — the seam side is biased quiet.
- Still owed: a resume chain on a lineage where the DEFECT fires post-seam (this chain's
  hour 2 is defect-free even seamlessly, so "does 1f still recover withholds after a resume"
  is untested — needs a roster/window whose breach epochs land in hour 2), and multi-leg
  chains (seed → leg → leg). — INERT-BY-CLOSURE on every roster tested (2026-08-17)

§13.8's other open item ("C-GF was not varied in any A/B"), closed with four arms on the same
build as §13.9: `_fld50_cgf{00,11}` (field eq34) and `_s24cpl2_cgf{00,11}` (sim), each the
matching §13.9 cell plus `network.shared_carry_gauge_free: true` and nothing else.

**Result: all four C-GF arms are BYTE-IDENTICAL to their parents** (products.txt and
network_solution.csv), and the mechanism is visible in C-GF's own counters: the closure test
DECLINED the (single) declared CLK|G direction essentially every epoch — field 100/101 and
120/121 emitted epochs, sim 115/116 and 119/120.  A roster's carried clock set essentially
never spans the full registered constellation (some registered satellite is always unobserved
or departed), full-support closure therefore never holds, and per §13.6-defect-1's fix the
group folds PLAINLY on decline — which byte-identity proves is a true no-op.  The earlier
50-station wiring probe already showed the same signature (10 declines / 11 epochs) and it
went unread.

Three consequences:

- **The knob is SAFE and NULL as built**: proven solve-inert in four arms across two lineages.
  It IS fingerprinted (`cta:gf` / `cta:gf+cl` via the resolved `carry_gauge_free` predicate,
  NetworkRunContext.cpp:680), so arming it splits a warm-resume lineage while changing
  NOTHING — the worst combination.  Registered **KEEP_OFF**: nothing to gain, a fingerprint
  split to lose, and a knob that reads as armed while doing nothing is posture noise.
- **The refusal is CORRECT, not a bug** (the run-end message states the theory): restricted to
  a carried subset, the constellation-sum direction is no longer the datum's functional, and
  downdating it would delete an estimable carried-vs-uncarried CONTRAST rather than gauge.
  The design gap is real though: a gauge-free treatment that engages on realistic rosters must
  re-derive the datum functional WITHIN the carried subset (datum pins + carried span), which
  is a new derivation card, not a gate tweak.
- **Counter-semantics wart (minor, invariant #4)**: "N clock prior key(s) gauge-downdated"
  counts keys processed through the gauge-free-capable path, not keys whose information
  changed — next to universal declines it reads as armed-and-working when the net effect is
  nil.  The decline count printed beside it is the honest number; read them together (the
  message's own closing sentence says exactly this).

Note the within-epoch RAIM replay gauge-flag fix that shipped WITH the C-GF card (§13.6, the
deliberate byte-identity exception) is unconditional and unaffected by this verdict — it is
live regardless of the knob.


