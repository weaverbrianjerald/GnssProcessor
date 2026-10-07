# IF-TD ref gate (v2): warm-up abstain, elevation-normalized bar, gating leak

**STATUS**

- **id:** IF-TD ref gate (v2)
- **topic:** slip
- **status:** closed
- **summary:** On a slip-free bed warm-up abstention removes all 118 cold-era fires and the elevation-normalized bar adds 2; both knobs filed UNVALIDATED (elev_norm: DO NOT ARM) pending a fault arm.
- **dates in the text:** 2026-09-09
- **knobs / posture:** `network.cycle_slip.if_phase_td_ref_warmup_abstain`, `network.cycle_slip.if_phase_td_ref_elev_norm`
- **hashes named in the text** (commit / exe / byte-cert): `2456cdfa`
- **run dirs named in the text:** `runs/ifsc_new`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md`: §6.14.6, 6.14.6.1, 6.14.6.2, 6.14.6.3, 6.14.6.4, 6.14.6.5, 6.14.6.6

**Outcome.** Four follow-ups to [IF-TD ref gate](IFTD-REF-GATE.md). The gating leak is fixed; the nuisance gate is built with its feed deferred, and the floor_frac card is pre-registered. In the three-arm A/B the OFF control is byte-identical; abstention removes only unactionable diagnostic fires (the acting path was already 0 under Card R), and elev_norm moves in its pre-registered, more sensitive direction at a net cost on this bed. A 0-slip bed cannot test detection power, so the [ledger](../../CONFIG_promotion_ledger.md) files both knobs UNVALIDATED, blocked on the §6.14.6.4 fault arm.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.14.6 The follow-up build (2026-09-09) — abstain while cold, normalize the bar, close the gating leak

Four items came out of §6.14.  Two landed and are validated; the third landed as a
complete mechanism with its FEED deferred, for a reason worth recording; the fourth
turned out not to need the bench it was blocked on.  Every knob is default-OFF and
interlocked to its host arm, so the OFF path is byte-identical.

### 6.14.6.1 Item 1 — ABSTAIN while the empirical arm is cold

`network.cycle_slip.if_phase_td_ref_warmup_abstain` (default false).

With zero empirical information the cold prior `floor_frac·λ_IF/k` makes `k·s_eff`
collapse onto `phys`, so the hardened gate reverts to *exactly* the legacy 3σ fixed
bar for its first few epochs.  That fallback is deliberate — shrinking toward the
current epoch's own dispersion is self-contaminating and once swallowed the
OWMG00NZL/G31 re-lock — but §6.14.4 measured where it leaves us: **all 118 surviving
fires were at epochs 1 and 3, and every one was `action=diagnostic`**, because
`carry_warm` (n_emitted ≥ 6) is false there and a cold detection cannot refloat
anything.  Firing on a bar we cannot justify, to raise an event that cannot act, is
pure event-stream pollution.

Armed, the gate returns `Suppress` until `ref_scale_count` reaches `shrink_kappa`
— the sample count at which the shrinkage weight reaches ½.  Abstention asserts
nothing about the scale; it declines to decide.  The cost is explicit: a genuine
reference slip inside the warm-up is not caught by THIS arm, where it was already
unactionable, and MW/LLI still screens the window.  `suppressed_warmup` counts what
was given up, and the abstained epochs are still counted as legacy candidates — the
refusal is a refusal to decide on a real candidate, not a failure to see one.

### 6.14.6.2 Item 2 — the bar has to track the REFERENCE's elevation

`network.cycle_slip.if_phase_td_ref_elev_norm` (default false),
`…_ref_elev_clamp_deg` (5.0, matching the pool arm's clamp).

The gate tests the MEDIAN of the family, and §6.14.2 measured that 88 % of that
statistic's variance is the reference satellite's own noise divided by sin(el_ref)
— a term in every member that no family size averages away.  The scale it was
judged against, `disp`, is a different quantity: the family's scatter about that
median, i.e. the per-SATELLITE noise at whatever elevations happened to be up.
Unlike the pool arm the gate applied no elevation map at all, and the state holding
that scale (`ConstellationState`) outlives the reference satellite it describes.

Armed, the EWMA stores the ZENITH-equivalent `disp·⟨sin el⟩_family` — a stationary
per-station receiver-noise scale that survives a handover — and the test rebuilds
the statistic's own sigma from it:

```
σ_ref = s_z / sin(el_ref)                  the common term
σ_fam = 1.2533 · (s_z / ⟨sin el⟩) / √n     the part that DOES average down
thr   = max(phys, k · hypot(σ_ref, σ_fam))
```

Validated against the 24 h bed before it was written: s_z ≈ 12.8 mm at el_ref ≈ 62°
gives σ_ref 14.5 mm against a measured station-local common of **14.83 mm**, σ_fam
10.1 mm, and hypot 17.7 mm against a measured σ(median) of **17.70 mm**.  The flat
`k·s_eff` form is blind to both el_ref and n; this tracks them.  The physical FLOOR
stays raw in both modes — a slip is a slip at any elevation (slip-QC lesson 7).

⚠ **The direction is NOT "fewer fires" — measure it before assuming.**  Replayed
offline over the settled 24 h trace (314,318 station-epoch-constellations, s_eff
approximated by that epoch's own dispersion), the normalized bar is p50 **0.78×**
the raw one (p10 0.67, p90 1.00); only 2.5 % of groups get a HIGHER bar.  On this
bed's geometry — an overhead reference over a family whose scatter largely averages
down — the correct statistic is SMALLER than `k·disp`, so `elev_norm` makes the gate
MORE sensitive, and could raise the fire count.  Its case is that the bar tracks the
quantity it is testing, not that it suppresses anything.  Pre-registered before the
arm ran: with `warmup_abstain` already near zero fires, `ifg_we` and `ifg_w` should
be CLOSE, and any difference must appear at the geometry extremes rather than
uniformly.

⚠ One wart, deliberately left: the cold prior is a RAW scale while the EWMA under
`elev_norm` is zenith-normalized, so the shrinkage blend mixes units while cold.
The direction is conservative — the rebuilt bar is ≈1.4× the raw one at typical
geometry, i.e. it errs toward NOT firing, the same direction as item 1, and the two
knobs are meant to be armed together.  Normalizing the prior instead would mean
making a *physical* floor geometry-dependent, which is the thing lesson 7 forbids.

### 6.14.6.3 Item 3 — the nuisance gate, and the leak it exposed

Two halves.  One is a defect fix and landed; the other is a mechanism whose FEED is
deferred, with data.

**The measurement.**  The only filter state that survives both the between-satellite
and the between-epoch differencing is ZWD.  Median |ΔZWD| per epoch against the
reference gate's own fire count over a cold start:

| ep | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| med \|ΔZWD\| mm | 539 | 471 | 431 | 466 | 438 | 619 | 279 | 151 | 98 | 126 | 82 | **62** |
| refsat fires | 116 | 114 | 113 | 112 | 110 | 106 | 99 | 88 | 70 | 43 | 25 | **0** |

They die together.  And the mechanism does not stop at ignition: in the settled era
(station-scoped, baseline P(fire) 0.862 %) the fire rate rises monotonically with
|ΔZWD|, from 0.55 % in the quietest decile to **1.77 % in the top decile — 2.05×
baseline, 3.2× across the deciles**.  This also corrects the header's claim that the
detector is "independent of Kalman filter state": the OBSERVABLE is, but
`phi_if_omc` subtracts `obs.modeled`, and the burst is that model settling.

By contrast, PRODUCER-side model events do NOT drive it — station-scoped lifts are
`anchor_handover` 0.31×, `handover_continuous` 0.35×, `handover_cold` 0.00×,
`attrib_arc` 0.00×, `mw_wl_slip` 0.00×, all at or BELOW baseline.  "Any disturbance"
is the wrong generalization; the troposphere is the specific one.

**THE LEAK (landed).**  A gated epoch suppressed this detector's own flagging but
still published its jumps as `valid`, and the pool arm admits any sample with
`jp.valid` — so the pool went on thresholding double differences the detector had
just declared untrustworthy.  Gating has to reach both arms or it protects neither.
`info.valid` now carries `&& !pos_gated`; jumps stay cached and the gradient chain
still advances, so detection resumes seamlessly.  A second, latent half of the same
defect: `processDoubleDifferences` re-derived `pos_gated` from `position_sigma_3d`
alone, so any new gating arm would have been silently excluded from the reference
gate.  Both now read one member, `epoch_gated_`, set once in `detect()`.

**The gate (landed) and the FEED (deferred).**  `detect()` takes a
`model_nuisance_m` and `setNuisanceGate(max_m)` arms the threshold; 0 disables it.
There is deliberately NO config knob yet, because there is nothing to feed it:

- `st.pstate.troposphere_zwd` is set to 0.0 at setup and never updated on the
  network path — a dead field there.
- The live ZWD is reconstructed in the serial POST-SOLVE diagnostic sweep
  (`NetworkIonoDiag`), behind the `trace_zwd` gate.  Feeding the detector from it
  would mean either gating a QC decision on a trace knob — which this codebase has
  an explicit rule against — or hoisting the reconstruction into the hot path.
- The detector runs in PASS-A, BEFORE the solve, so the feed is one-epoch-lagged by
  construction.  (Defensible — ΔZWD is smooth on both the ignition and the weather
  timescale — but it is a real property of the design, not an implementation detail.)
- Decisively: items 1 and 2 are predicted to leave it no target.  Shipping a config
  knob for a mechanism with no demonstrated work to do is exactly the armed-but-inert
  failure this codebase keeps paying for (standing invariant #3: triage the deferral
  with DATA).  The pool arm it would also protect has 0 flags in 2,754,791 samples.

So: mechanism complete and unit-tested, feed deferred until the §6.14.6.4 bench can
show it a target.  If the armed A/B below leaves a residue that tracks |ΔZWD|, the
feed is the next increment and the harvest point is named above.

### 6.14.6.4 Item 4 — the bench mostly EXISTS; the gap was elevation

Item 4 was filed as "needs a bench that doesn't exist yet".  That is not right, and
the correction matters because it moves the (1,1) floor decision from blocked to
one option away.  `scripts/simnet_inject_arms.py` already has:

- a magnitude ladder defaulting to ±1, 2, 3, 5, 10, 50 cycles — the sub-2-cycle
  rungs the floor decision needs are already there; and
- `--signal-mode mw-blind`, which puts the SAME delta on both bands of a pair —
  which is exactly the (n,n) class MW is structurally blind to and the only class
  where IF-TD has unique reach.

The one thing it could not express is a slip at HIGH elevation.  The random slip
model is capped at `max_elev_deg = 45` while the reference sits at p50 61.7° (§6.14.5:
zero overlap, so a reference-satellite slip is unreachable by construction) — and
the planner emitted only form-A (exact-epoch) events, which need an elevation the
visibility oracle does not carry.  `ScriptedSlip` has supported form B (an elevation
WINDOW, `elev_lo_deg` / `elev_hi_deg` / `not_before_epoch`) all along; the planner
just never emitted it.  Added: `--elev-window LO HI`.  The planner still verifies the
station actually TRACKS the (sat, signal) and still places the event inside the
requested arc window (the chosen epoch becomes `not_before_epoch`), but hands the
firing decision to the simulator's own elevation trigger.

⚠ Form B fires on the FIRST visit to the window, and an unfired scripted slip is a
run-end failure — so verify every event fired and that the fixture's arcs reach that
elevation.  The runner prints the warning.

**PRE-REGISTRATION for the floor_frac card (write this down before running it).**
The decision is whether `floor_frac = 1.0` — the physical floor sitting exactly ON
the (1,1) jump with a strict `>` — should come down.  Measured constraints going in:
the floor binds on only 5.21 % of rows and NEVER below 30°, so on 94.8 % of samples
the empirical arm is already the binding constraint and lowering the floor changes
nothing there; the historical field sweep gave floor_frac 0.5 → 45 flags, 1.0 → 26,
1.5 → 24.  PASS = on a `--signal-mode mw-blind --elev-window 60 90` fault arm, the
(1,1) and (2,2) rungs are detected at a materially higher rate than at
`floor_frac = 1.0`, with the false-alarm count on the matched NULL arm unchanged
(0 pool flags, and the reference gate at its §6.14.6 armed level).  FAIL = any
false-alarm increase on the null arm; the (1,1) rung is not worth buying with one.

### 6.14.6.5 The three-arm A/B — item 1 confirmed, item 2 COSTS on this bed

`config/test_suite/_ifg_{off,w,we}.json`, each differing from
`golden_sim_tri_w6` only by the run paths, a 2 h window, and its own knob; same
exe (`gnss_processor_ifg.exe`, md5 `2456cdfa…`); sequential, SOURCE-mtime capture
assertions, all three "capture: all fresh".

| | ifg_off | ifg_w (+abstain) | ifg_we (+abstain+elev_norm) |
|---|---|---|---|
| refsat ACTING refloats | 0 | 0 | 0 |
| refsat diagnostic | **118** | **0** | **2** |
| fires ep 0–11 / ep ≥ 12 | 118 / 0 | 0 / 0 | 2 / 0 |
| tier T1r opp / flags | 28680 / 118 | 28680 / 0 | 28680 / 2 |
| tier T1 (pool) opp / flags | 228634 / 0 | 228752 / 0 | 228750 / 0 |
| MW/WL events | 142 | 142 | 142 |
| total events logged | 509 | 391 | 393 |

Run-end counters (candidates → fired; suppressed by scale/coherence, thin, cap, COLD):

- `ifg_off` 1090 → **118**; 948, 13, 11, 0
- `ifg_w`  1111 → **0**;   818,  9,  0, **284**
- `ifg_we` 1111 → **2**;   816,  9,  0, **284**

**OFF control byte-identical** to `runs/ifsc_new` (pre-change exe, same config) on
`network_solution.csv`, `network_events.csv`, `network_flag_rates.csv` and the
31 MB `network_if_slip.csv` — the new code is inert with the knobs off.

The candidate count moving 1090 → 1111 is explained, not noise: in `ifg_off` the
118 fires each reset a reference arc and perturbed the stream behind them, so
abstaining leaves more families intact to be screened.  Same for T1's opportunity
count (228634 → 228752): fewer reference resets means more satellites retained in
the pool.

**Item 1 is confirmed, and its win is smaller than it looks.**  118 → 0, with 284
abstentions naming exactly what was given up, MW and the pool arm untouched, and
zero change to the acting path.  But the acting path was ALREADY zero under Card R
(§6.14.4) — on this posture every fire is cold-era `diagnostic`.  So what item 1
removes is 118 unactionable events, i.e. event-stream pollution and a principled
cold posture, NOT any refloat damage.  Worth having; not worth overselling.

**Item 2 is confirmed in DIRECTION and it is a net cost here.**  The pre-registered
prediction (§6.14.6.2: bar p50 0.78× raw, more sensitive, difference at the geometry
extremes not uniform) is exactly what happened — `elev_norm` ADDED 2 fires, both in
the burst window, on a bed with zero slips.  On a truth bed with nothing to detect,
extra sensitivity is pure false-alarm cost.  Its case remains that the bar tracks the
statistic it is testing rather than a proxy for it, and that case can only be settled
on the detection side, which this bed cannot measure.  **It is therefore NOT
recommended for arming until the §6.14.6.4 fault arm runs** — the honest verdict is
"more correct, measurably more expensive, benefit unmeasured".

### 6.14.6.6 What is NOT validated here

**Detection power is not validated by any of this, and cannot be on this bed.**  The
quiet fixture injects 0 slips, so every arm here measures FALSE-ALARM behaviour only.
Items 1 and 2 both make the gate *less* likely to fire; on a zero-slip bed that is
unfalsifiable as a quality claim — a gate that never fires scores perfectly.  What
would falsify it is the §6.14.6.4 fault arm, and until that runs neither knob has
evidence that it preserves a real detection.  Both are filed UNVALIDATED, and the
goldens are deliberately NOT changed.

**Item 2's stated motivation was partly already discharged.**  The 25× reference-
handover lift (12.26 % vs 0.487 %) was measured with the gate OFF, i.e. against the
legacy FIXED bar — so it cannot have been caused by a stale empirical scale, because
on that path there is no empirical scale.  It is the handover rebase's own residual,
and §6.14.4 shows Card R already absorbs it (0 steady-state fires, 11 reselects).
So elev_norm is not buying back a measured residue here; its case is that the bar
should track the geometry it actually depends on — which matters most where the
el_ref distribution differs from this bed's, i.e. the field.  Recorded so nobody
re-derives the 25× as evidence FOR this knob.

**Also out of scope:** sim-only, no field arm; a 2 h window, which is inside the
level channel's convergence budget and so supports counting fires but no wire-quality
claim; and the nuisance gate has no feed (§6.14.6.3).

