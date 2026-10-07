# §18.10/§18.11: the seam is a FRAME loss, `resume_frame_restore` built and graded

**STATUS**

- **id:** §18.10/§18.11
- **topic:** resume-seam
- **status:** closed
- **summary:** The FRAME-loss mechanism is CONFIRMED and the restore closes the phase side, but 33 of 52 anchored clocks trip at the first fold: KEEP OFF pending the coupling-regressor increment.
- **dates in the text:** 2026-09-02 → 2026-09-03
- **knobs / posture:** `network.state_snapshot_frame_capture` (write side, safe on any writer), `network.resume_frame_restore` (read side), `network.resume_slip_warmup_ep` (5 on the ON leg), `amb_seam_reinflate_var_m2` 0.0025 (rung 3, FALSIFIED as a seam response)
- **hashes named in the text** (commit / exe / byte-cert): `35860c5f45b8a004` (warm-resume fingerprint, v1 library = v2 writer)
- **run dirs named in the text:** `runs/camp60wu_quiet12h_frwriter` (v2 writer), `runs/camp60wu_q12warm_fron` (ON), `runs/camp60wu_q12warm_frctl` (OFF control), `runs/camp60wu_q12warm_fron_ari` (rung 3)
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §18.10, §18.11

**Outcome.** The question is whether the sim warm-resume seam is the frame bookkeeping the snapshot omitted (anchor election, Q3 clock anchor, CLK-R held frame, slip history). The restore removes the carry erase (54 erased vs 2862) and the datum mark, but FAILS two frozen bars (clock between-satellite SD 0.28 m against 0.02; `cta_recouple` 33 against 0), and the new clock tails cost the consumer 0.96 m rms at h19; the rung-3 proxy is FALSIFIED. With the regressors ([§18.12](../resume-seam/RESUME-COUPLING-RESTORE.md)) and the dense border ([§18.13/§18.14](../resume-seam/RESUME-DENSE-BORDER-E2.md)) the knob became RECOMMEND_SCOPED on 2026-09-06 ([ledger](../../CONFIG_promotion_ledger.md)); seams are off under the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) scope cut.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
### 18.10 RESUME FRAME RESTORE — the seam is a FRAME loss; the card restores the
### frame state the snapshot omitted (built 2026-09-02; verdict in §18.11)

**Root cause, verified with counters (five readers, R1/R5 derivation).**  RTB-7
restores every carry (376 D2, 2811 leveling, 60 Z) but NOT the realization-frame
bookkeeping, and the seam census (§18.9) is entirely that omission:

1. **Phase-anchor election state.**  `anchor_incumbent` / `anchor_pin_target`
   (NetworkRunContext.hpp:609-644, "deliberately NOT snapshotted") are empty at
   the first warm epoch, so the prioritized election (NetworkPhaseDatum.cpp:
   300-470) finds NO incumbent for any of the 162 (sat,signal) families.  The
   QUEUE-1(d) clause `inc == anchor_incumbent.end()` (:431-435) then marks every
   family as re-referenced — silently, no event — and `networkApplyPhaseAnchor
   CarryReref` (:453-471, called from ApplicationNetwork.cpp:1745-1750 BEFORE
   PASS B) ERASES every restored leveling carry; the marked families' D2
   phase-bias prior is marginalized for the epoch (NetworkRunContext.cpp:
   1909-1911); every family re-pins at target 0 = the anchor's fresh single-
   epoch code seed (pin_y = target − piv_x, Assembly:2500-2517; sticky targets
   up to |T| 29 m in this leg).  Counters: "phase-leveling local carry(ies)
   erased" **2862** on the 12 h warm leg vs **129** on the 24 h continuous run
   (~2811 = the whole restored set); B1b phase-bias σ at 302430 **1.112 m vs
   0.012**; the estimability DIR rows appear only at 302430–302580 on the warm
   leg and only at 259230–259380 (ignition) on the continuous run.  The
   restored `LocalCarry` values are therefore folded ZERO epochs — which is
   also why leg C (ledger row 321) measured NULL (§18.7 correction note).
2. **Q3 clock-anchor state.**  `ClockAnchorState` (NetworkRunTypes.hpp:891-920)
   is "deliberately NOT snapshotted … every satellite must re-earn the anchor
   from LOOSE after a resume"; a first sighting folds LOOSE at the legacy
   sat_clock_q 0.01 m²/s (σ 0.55 m over 30 s, NetworkClockAnchor.cpp:104-149)
   for `clock_anchor_recouple_epochs` epochs, then re-engages on whatever the
   code-only solve realized.  Events: 302430 nothing; 302520/302550 **48 + 4
   `CLK_ANCHOR cta_recouple reengage`** (continuous run at the same TOWs: 0).
   That is the constant post-seam clock re-split (G 1.30 m / E 0.53 m SD).
3. **CLK-R held frame.**  `NetworkClockEmitRebase` is a runNetwork local
   (ApplicationNetwork.cpp:918) with no snapshot fields; its accumulator A_c
   and per-sat baselines restart at zero, so the emitted common mode steps by
   A_c (G −0.87 / E −0.22 m at the seam — the continuous run emits the pivots
   G01 at +1.2179 m and E02 at +0.27 m held) and the F1 rule (NetworkEmitStore
   .cpp:338-340, `counters().epochs == 0 && n_emitted > 0`) marks `CLK_FRAME
   resume_seam` for both constellations — the ONLY events at 302430.
4. **Slip-detector history.**  Not snapshotted; the MW detector's first post-
   resume difference (two samples, no Welford reference; MwSlipDetector.cpp:
   1376-1385 gross gate, :2100-2127 one-sample integer path) declares **146**
   "confident" WL slips at the second warm epoch (continuous run: 0) and the
   `carry_warm` switch (NetworkSlipQc.cpp:828) makes them ACT — 292 latches, a
   second re-level of arcs that had just been re-seeded.  The raw MW TD census
   is identical in both runs (984 arcs, MAD-σ 0.553 m ≈ 0.7 WL cycle): a
   one-sample integer gate on that noise is the h0 cold-start population
   (139 flagged at 259260, diagnostic there) re-fired through an open guard.

Corollaries the readers settled: `η = Λ·(value_abs − x_lin)` is x_lin-invariant
(NetworkRunContext.cpp:2329-2333), so the cold `x_lin` is harmless — what does
not survive is the phase-bias GAUGE (the pins), and restoring VALUES against a
stepped frame would be the §5.07 defect (41 σ innovations) 1(d) exists to
prevent; the frame STATE must be restored instead.  Code bias survives to
0.1 mm because its D2 carry is neither marginalized nor loosened and its frame
is re-fixed by the IF/GF/s2z gauge rows every epoch — and holding B while C
re-drew is exactly what put the re-split on the consumer's code side.  The
per-sat clock re-split moves TOWARD truth on aggregate (R4: G rms 2.41 → 1.66,
regression slope −0.42 / r −0.79 on the continuous run's own error; G17/G10/G29
hold 0.1–0.2 m all day on the warm leg vs 1.9/2.2/1.6 m frozen on the
continuous) — so a continuity mechanism deliberately carries the OLD split
forward; the §18.1 tails are E2's card, not this one's.  Consumer-invariance
subspace (R3/R5): a wire transform is invisible iff it is a per-constellation
common κ on C, a per-sat lump δ on C AND every B_j AND every P_j, per-(sat,
signal) integers on P, and per-(const,band) commons; the seam violates both the
code condition (dB ≡ 0, dC per-sat SD 1.3 m) and the phase condition (uniform
fractional part) — no outputs-only hold can bridge it without lying, and a
decaying hold is invariant #7's silent walk.

**Mechanism (single card, two knobs, both default-OFF, both fingerprint-exempt
as the ninth/tenth forward-only resume response policies):**

- **Snapshot format v2** (include/products/NetworkStateSnapshot.hpp, .cpp):
  the frame rows are captured whenever a snapshot is written under
  **`network.state_snapshot_frame_capture`** (write-side, default OFF — the
  judges' graft over the Z-row precedent: a v2 file is unreadable to a
  pre-card binary, so the DEFAULT snapshot must stay v1 byte-for-byte) —
  `PA,<sat_sig>,<station>` (phase-anchor incumbent),
  `PT,<sat_sig>,<target_m>` (R2 sticky pin target), `CA,<sat>,t_last,last_value,
  last_var_eff,in_band,anchored` (Q3 per-sat state, folded sats only),
  `FA,<const>,A_c` / `FP,<sat>,last_held,last_seen` / `FE,<epochs>` (CLK-R held
  frame, exported through the new `NetworkClockEmitRebase::exportFrameState`).
  The v2 header is written only when at least one frame record exists, so a run
  carrying none writes v1 byte-for-byte; the reader accepts v1 and v2 (a v1 file
  restores empty maps = today's behaviour, and an armed resume logs INERT).  The
  snapshot writer and the resume take the CLK-R engine as a new (nullable)
  parameter (ApplicationNetwork.cpp call sites).
- **`network.resume_frame_restore`** (GnssConfigLoadNetwork.cpp beside
  zwd_seam_reseed; NetworkRunContext.cpp restore block after the ZWD-seam
  arming): at a warm resume, restore `anchor_incumbent`, `anchor_pin_target`,
  `clk_anchor.sat` (with t_last, so the stale test runs on the TRUE gap and a
  resume older than `clock_anchor_max_gap_s` still decouples honestly) and the
  engine's frame state BEFORE the first election / fold / emit.  The election
  then keeps each incumbent (`sticky` path, NetworkPhaseDatum.cpp:302-308), the
  R2 clause pins at the restored target (:355-361), `is_handover` is false and
  `inc != end()` so the 1(d) clause does not mark → no erase, the D2 PB prior
  folds, the restored leveling carries fold in-frame; every anchored clock folds
  at `cta_q` over dt_sat; `counters().epochs > 0` so the F1 seam mark does not
  fire.  Re-coupling edge logged with identities (counts per class) and counted
  (`n_resume_frame_anchors/targets/cta/cta_anchored/clk_keys/ledger`, run-end
  line in NetworkTeardown, manifest fields).  Judges' graft (Card 3): the
  two-edge handover LEDGER is primed from every restored leveling row
  (`handover.value_abs[{station, sat_signal}] = {value_abs, var}`, only under
  `pd_handover_continuous`), so a family whose restored incumbent no longer
  tracks the satellite at the first live epoch takes the CONTINUOUS handover
  path through the existing re-validate gate (`pd_handover_max_var`, 1e-4 m²
  on r2q3) instead of the cold pin + 1(d) mark + erase; the first post-resume
  harvest rebuilds the ledger wholesale, so the seed lives one epoch like any
  ledger entry.  Judges' second graft: `amb_seam_reinflate_var_m2` becomes
  ACTIVE once the erase stops (its restore-site add was neutralized by the
  erase, §18.7 correction) — it is pinned to 0 in every arm here and its
  ledger row (321) must be re-read as "untested under frame restore".
  Accepted limitations named by the judges: (i) the `resume_seam` mark is
  suppressed by the restored engine state whether or not every family's frame
  came back — a family the election still cold-pins (absent incumbent, ledger
  entry over the ceiling) is marked individually by 1(d) and its phase-bias
  step by the emitter's 0.05 m jump test, so the consumer is not blind, but
  the constellation-level mark no longer fires (an invariant-#7 identity
  gap, visible in `n_resume_frame_anchors` vs the election's cold-pin tally);
  (ii) there is no seam-scoped validator of the restored zero-q levels beyond
  the live T1 drain and the post-fold instruments — a family-median innovation
  test was shown to be attenuated by the stiff rows it would test; the rail is
  the bounded gap (≤ 120 s) and the continuous-handover path above.
- **`network.resume_slip_warmup_ep`** (int, epochs): for K processed epochs
  after a resume the MW/LLI (T0) drain runs DIAGNOSTIC-ONLY (its `carry_warm`
  argument is forced false at the single dispatch site, ApplicationNetwork
  Assembly.cpp:1500); the T1 IF-TD and wind-up drains keep their live gate, so
  a genuine in-gap slip keeps a detector.  Countdown in `ctx.carry.resume_slip_
  warmup_left`, never serialized; ARMED at the resume and RETIRED (logged) at
  the dispatch.  Rationale: the false-alarm population (146/2811 arcs at the
  second warm epoch) is two orders above the true slip rate over a ≤120 s seam
  (the continuous run's MW rate is 1–6/h network-wide), and the same one-epoch
  no-slip assumption already underlies every one of the 2811 restored carries.
- **Invariant #1 rail** for the restored pin targets: the seam gap is bounded by
  the resume gate (age ≤ max(pl_max_gap_s, clk_model_max_gap) = 120 s), the
  restored target is the same one-epoch continuity assumption as any carried
  arc, and an anchor arc that DID slip in the gap is caught by the live T1
  drain (refloat → not usable → the ledger, harvested at the first post-resume
  solve, hands the pin over CONTINUOUSLY at the second epoch — the existing
  machinery for a mid-run anchor slip).  Invariant #7: both edges are events
  with identities (ARMED line + counters at the re-coupling edge; the warm-up
  RETIRED line at the decoupling edge; the run-end line reads 0 under an armed
  knob as INERT).
- **Structural byte-identity:** knobs OFF ⇒ the resume path reads nothing new,
  `mw_carry_warm == carry_warm`, and a snapshot without frame rows is v1
  byte-for-byte; a run with frame rows writes v2 (an output-format change on
  the snapshot only — products/traces untouched).  Unit contract:
  tests/test_resume_frame_restore.cpp (v2 round-trip, v1-header byte-identity
  without frame rows, v1 backward parse, malformed-row refusal, CLK-R
  export/restore equivalence, loader defaults/validation).

**Venue and pre-registered predictions.**  The v1 `.ep1440` library cannot
carry the rows, so the branch point is re-created: `runs/camp60wu_quiet12h_
frwriter` = the 24 h base config limited to 12 h 5 min under the v2 writer
(gate: its products.txt is a byte-identical PREFIX of the continuous run's).
Legs branch from ITS `.ep1440` via `scripts/simnet_make_seam_leg.py`: OFF
control (same snapshot, knobs off — must reproduce leg A's numbers), ON
(`resume_frame_restore` + `resume_slip_warmup_ep 5`), and a frame-only ablation
if the ON leg passes.  Graded with `scripts/simnet_seam_tools.py` (diff /
decompose / steps / quality / walk against the continuous run's same hours) and
the consumer-only replays (§18.9).  Predictions (from R5 + §18.9, to be resolved
in §18.11): first-warm-epoch B1b PB σ **≈ 0.012 m** (from 1.112); emitted clock
between-sat SD at 302430 **≤ 0.02 m** G / E (from 1.30 / 0.53); phase-side
|frac| **≤ 0.05 cyc** (from a uniform 0.28); URA **0.004–0.005** (from 0.30);
keys over the 0.05 m disc threshold at the second warm epoch **< 10** (from
150), consumer resets there **< 10** (from 51); the MW burst **146 → ≲ 2** with
the warm-up armed; run-end "local carries erased" **≈ 129-class, not 2862**;
`cta_recouple` at 302520 **0** (from 48); the `CLK_FRAME resume_seam` mark
**absent**; consumer h13–h15 within **1.5×** of the continuous-wire replay
(0.060 / 0.067 / 0.062) instead of 0.254 / 0.336 / 0.300; and the late-start
consumer (+30 min) converges monotonically.  The held-frame common mode κ
(G −0.87 / E −0.22) collapses too, because the engine's A_c travels.  What the
card does NOT promise: the per-sat tails (E2), the eclipse-cluster excursions
(sticky_pivot / E2), and any h16+ difference (shared eclipse schedule).

### 18.11 RESUME FRAME RESTORE — VERDICT (2026-09-03, legs from the v2 .ep1440 of
### `runs/camp60wu_quiet12h_frwriter`; ON `runs/camp60wu_q12warm_fron`, OFF control
### `runs/camp60wu_q12warm_frctl`, rung 3 `runs/camp60wu_q12warm_fron_ari`)

**Writer gate PASSED.**  The 12 h frame writer (24 h base config, 12 h 5 min,
`state_snapshot_frame_capture` — under the pre-knob build the capture was
unconditional, equivalent) produced products BYTE-IDENTICAL to the continuous
run over all 1450 epochs (34.8 MB prefix compare), so the capture is
write-outputs-only as claimed; its `.ep1440` is a v2 snapshot carrying 162 PA
(every family has an incumbent), 153 PT (sticky targets), 52 CA (all 52
folded clocks ANCHORED), 54 FP + FA(E −0.2675, G −1.2179 — the held frame the
continuous run emits) + FE 1440, with fingerprint `35860c5f45b8a004` = the v1
library's, so the same-snapshot OFF/ON A/B is legal.  (The other session's
unrelated tree changes — Planetary / SatAttitude / GnssCorrections — were
compiled in; the prefix identity shows they are default-inert on this bed.)

**1. The seam itself is CLOSED on the phase side and on the marks.**  ON leg
at the first warm epoch: the restore logs 162 incumbents / 153 targets / 52
anchored clocks / 54 CLK-R keys / 2811 ledger entries; B1b phase-bias σ
**0.008 m median** (leg A 1.112; prediction ≈ 0.012); the E record carries
**no datum mark** (leg A: `CLK_FRAME resume_seam` ×2); run-end 1(d) **48
marked / 54 erased** (leg A 210 / 2862; continuous 24 h 253 / 129) — the
seam erase is gone; the MW burst is **146 "detected (carry cold;
diagnostic)"**, acted on 0 (leg A refloated 146); consumer ambiguity resets
at the second warm epoch **5** (leg A 51), then 1/1/0/0/0; emitted
phase-bias between-sat SD vs the continuous run at 302430 **0.04–0.08 m**
(leg A 8–12 m) and it STAYS there all evening (P L1C SD 1.5–2.1 m vs leg A's
8–12 m: the ON wire continues the continuous realization; code bias
identical to 0.1 mm as before).  Phase-side fractional part |frac| p50
0.12 (E) / 0.19 (G) cyc — improved from a uniform 0.28 but not integer-
continuous, for the reason in 2.

**2. The residual is the CLOCK: 33 of the 52 restored anchors TRIP at the
first fold.**  Q3 run-end: "Trips 247 = **33 nis** + 0 abs + 13 gap + 201
datum" (leg A: 0 nis) — `CLK_ANCHOR cta_decouple` ×33 + `cta_disc_landed` ×33
at 302430, innovations e = 3.6–7.6 cm against √S 3–5 mm (w 6.1–25.5, k 6;
worst G22 25.5, G14 19.5, G17 15.2, G20 15.2), all 33 re-engaged by
`cta_recouple` within the hour.  Cause (R5's catch, verified): the restored
leveling rows fold WITHOUT their Card-1 coupling regressors (the snapshot
carries value/var only), so 2811 stiff rows constrain the ambiguities
absolutely instead of "relative to the shared border" and the anchored clock
priors (9.5 mm) lose the vote by 5–7 cm → the guard refuses them, they fold
LOOSE (σ 0.55 m) on a code-only border for 4 epochs and the anchor freezes
whatever that re-draw produced.  Wire: emitted clock between-sat SD vs the
continuous run **0.28 m G / 0.16 m E** at 302430 (leg A 1.30 / 0.53), growing
to 0.48 / 0.24 while the 33 are loose, then flat (0.62 / 0.33 all evening).
Truth-graded (null-truth bed, per-constellation median-demeaned emitted
clock = the per-sat error): GPS rms over h13–15 ON 2.19 / leg A 1.59 /
continuous 2.11 m; over h18.7–20.5 ON 2.16 / leg A 2.21 / continuous 2.21 —
in aggregate no worse than the continuous run, but the TAILS moved onto the
tripped satellites: hourly means h13–h21 **G14 +3.0 (continuous +1.85),
G17 +2.9 (+1.9), G22 +2.2 (+0.78), G20 −1.35 (0.0)**; leg A's full cold
re-draw had landed G17 −0.27, G20 −0.35, G22 +0.65 (the R4 "fresh re-draw is
closer to truth" effect).  The ON leg got the worst of both: the pre-seam
frame for most satellites AND a code-only re-draw for 33 of them at the one
epoch when the border was over-stiff.

**3. Consumer: the seam transient is 2–3× smaller but a NEW h19 excursion
appears, and it is a WIRE property of the residual in 2.**  Hourly 3D rms
(ON / leg A / cold consumer on the continuous wire from h12): h12 0.564 /
0.736 / 0.523 (p50 0.30 / 0.71 / 0.12); **h13 0.173 / 0.254 / 0.060; h14
0.135 / 0.336 / 0.067; h15 0.114 / 0.300 / 0.062**; h16 0.468 / 0.437 /
0.405; h17 0.386 / 0.833 / 0.457; h18 0.543 / 0.874 / 0.636; **h19 0.964 /
0.119 / 0.223; h20 0.576 / 0.096 / 0.262**; h21 0.130 / 0.078 / 0.205; h22
0.089 / 0.073 / 0.113; h23 0.098 / 0.048 / 0.107.  The h19 excursion (p50
≈ rms ≈ 1.0 m; E −0.8 / U +0.5; ZERO consumer resets in h18.7–h20.5; the
wire's per-epoch phase-side steps and σ at the continuous run's level) is
reproduced by a FRESH consumer replayed cold from h18.0 on the ON wire
(20-min bins 1.14 / 1.19 / 1.04 / 0.88 at h19.0–h20.0) and NOT on the
continuous wire (0.33 / 0.25 / 0.19 / 0.24), so it is carried by the wire:
after the h18.3 eclipse-cluster resets the consumer re-seeds its float from
the code side, whose per-sat clock tails on the ON wire (G14/G17/G22 +2–3 m,
G20 −1.35 m — high-weight satellites for this consumer) pull the float
solution ~1 m off for the ~2 h the phase needs to override it; on leg A's
wire the tails sit on other satellites and its consumer recovered in 20 min.
Same family as the h13–h15 residual (0.11–0.17 vs the 0.06 floor).

**4. Verdict.**  The FRAME-loss mechanism (§18.10) is CONFIRMED and the
frame restore closes it: no erase, no marginalized phase-bias prior, no
datum mark, no reset storm, phase-bias σ 0.008 m at the first warm epoch,
and the wire continues the pre-seam realization in every channel but the
tripped clocks.  The card is NOT yet shippable on this posture because of
the second-order effect in 2: the restored leveling rows are over-stiff
without their coupling regressors, the guard (correctly) refuses 33 anchored
clocks, and the LOOSE re-draw creates new metre-class clock tails on ~4
satellites that cost the consumer more at h19 than the seam ever did.  The
stop rule from §18.10's own predictions applies (clock SD ≤ 0.02 m: FAILED
at 0.28; `cta_recouple` 0: FAILED at 33).  Second increment, named:
serialize the Card-1 `CoupledShared` regressors of every restored L row (M_j,
s_prev, p_prev — R2 sized it at ~250 KB for 2811 rows) so the first fold is
conditional and the anchored clocks are not out-voted; rung 3 (ON +
`amb_seam_reinflate_var_m2 0.0025`, the now-live one-shot 5 cm σ soft refloat
of the restored rows) is the cheap proxy test of that hypothesis —
rung 3 (`runs/camp60wu_q12warm_fron_ari`, run 2026-09-03 16:35 under the same exe; it had NOT started at the earlier wind-down — a launcher bug) FALSIFIES the variance-inflation proxy: the first-fold clock trips fall only 33 → **14**, B1b PB σ 0.009 m, but the inflated restored levels RE-FLOAT on the cold station layer and the consumer walks **5.13 m rms in h12, 2.93 h13, 1.79 h14, 1.10 h15, 0.86 h16**, recovering to 0.07 only by h21 — the slip-relevel-walk family (a level re-seeded from one epoch of code carries station-common iono error; standing invariant #1's re-coupling edge). The h19 excursion is absent on rung 3 (0.42 / 0.27), consistent with its clocks not being frozen on a code-only re-draw, but that does not rescue a 5 m ignition.  So the stiffness of the restored rows is not the lever; their MISSING COUPLING is — serialize the regressors.  Until then: `network.resume_frame_restore` is **BUILT, mechanism
verified, KEEP OFF pending the regressor increment** (ledger); the capture
knob is safe to arm on any writer (write-outputs-only, proven).

**5. Structural control PASSED.**  The OFF control (same v2 snapshot, knobs
off, same build) reproduces leg A — run on the Sep-1 build from the v1
library — TO THE DIGIT in every hourly consumer figure h12–h23 (0.736 /
0.254 / 0.336 / 0.300 / 0.437 / 0.833 / 0.874 / 0.119 / 0.096 / 0.078 /
0.073 / 0.048 rms) and in every run-end counter (1(d) 210 marked / 2862
erased; Q3 0 nis / 13 gap / 201 datum trips, 1068 LOOSE folds; the two
`CLK_FRAME resume_seam` marks; B1b σ 1.112 m).  So (a) the v2 capture and
the resume-side code are byte-inert with the knobs off, (b) the other
session's concurrent tree changes are inert on this bed, and (c) every ON
vs OFF difference above is the knob.
