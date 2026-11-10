# Investigation report — DOY-129 continuous-run open questions (Q1–Q4)

Follow-on to `docs/HANDOFF_night305_day129.md`. Answers its ranked open questions
from the existing artifacts plus code tracing — **no re-run was needed**. Tags:
**[M]** measured in this investigation (scripts in the session scratchpad rebuild
every number from `runs/day129_cont/seg01/{logs,products.txt}` + `leg.json`);
**[H]** hypothesis consistent with all evidence but needing the instrumented
re-run (§6) to confirm.

**One-line synthesis: the four findings are one disease.** The producer's
between-satellite frame has no guarded stiff anchor: phase memory is porous
(13.5 % of carried DDs re-tapped fresh *every* epoch [M]), satellite clocks are
effectively per-epoch (q ratio clock:CB = 10⁸), and the only stiff cross-epoch
anchors are the near-static code-bias states and the two hard datum pins. In
quiet hours the frame wanders metres/hour on the clocks (Q3). Under a
common-mode excitation the system slides along a near-null global mode that
only the pins resist; what lands on the pinned satellite ratchets into its
near-static code-bias states and bleeds out on their Kalman time constant
(Q2, τ = 3.79 h *derived from q — see §2.3*). The SH field is a follower with
weak smooth-mode observability by design (Q1). The emitted disc counters signal
datum bookkeeping, not faults (Q4).

---

## 1. Configuration facts the mechanisms hang on

From `runs/day129_cont/seg01/leg.json` [M]:

| knob | value | consequence |
|---|---|---|
| `filter.process_noise.sat_code_bias_q` | **1e-10 m²/s** (3e-9 m²/ep) | CB states near-static → K ≈ 2.2e-3/ep gain |
| `filter.process_noise.sat_clock_q` | **1e-2 m²/s** (0.3 m²/ep) | clocks effectively white; 10⁸ softer than CB |
| `filter.process_noise.sat_phase_bias_q` | 1e-12 m²/s | PB stiffest — moved ≤ 4 mm at the event [M] |
| `network.code_datum_time_linked` | true | IF-pin + s2z rows carry previous values (Card 2) |
| `network.cb_kaula_prior` | absent → **false** (`GnssConfigLoadNetwork.cpp:2046`) | no zero-pull on CBs; decay must come from data |
| clock pivot | auto-lexical (no config pin) | G01 when present, else G02 — matches 2051/109 [M] |
| `estimation.global_vtec_sh` | deg 15, Kaula 10/n TECU, a00 15±30, q 0.01 TECU²/s, rep-σ 5 TECU | |
| `estimation.ionosphere_weighting` | σ_c = **50 m** common-mode, floor 0.05 m, elev 25° | SH sees only between-sat structure |
| `network.iono_constraint_deweight_*` | adaptive, 3σ↑/2σ↓, **floor 0.0**, rate 0.25 | w_s ≤ 0 drops a station's whole SH block |

The satellite-clock datum is the hard pin `clock(G01) = 0`
(`NetworkSbasis.cpp:184-185`); the code-bias datum is one IF pin per satellite
plus per-(constellation,signal) sum-to-zero, folded at
`kCodeBiasGaugeWeight = 1e3` (`NetworkSbasis.hpp:226`,
`NetworkSolveGlue.cpp:204`). `selectIfPinPair` sorts by (band digit, name), so
with full signal sets the pinned pair is **(C1C, C2L)** — *not* the (C1W, C2W)
pair the grading formula and RTKLIB-convention consumers use.

---

## 2. Q2 — why the pinned pivot's code bias absorbed the damage. ANSWERED

### 2.1 What actually stepped at tow 334020 [M]

Product records 333990 → 334020 (`products.txt`):

| quantity | step |
|---|---|
| G01 CB C1C / C1W / C2L / C2W / C2X | +9.66 / **+30.53** / +16.52 / **+18.54** / +15.12 m |
| G01 IF(C1C,C2L) — the **datum-pinned** combination | **−0.95 m** (held) |
| G01 IF(C1W,C2W) — the **graded/consumer** combination | **+49.06 m** |
| G01 phase biases (all 5 signals) | ≤ 4 mm |
| all 31 non-pivot clocks | −4175 … −5636 m (transient; G01 pinned 0) |
| per-channel ΣCB over all sats | held to < 0.25 m on every channel |
| each non-pivot sat per channel | −Δ_G01/31 exactly: predicted −0.985/−0.598/−0.312, measured −0.978/−0.598/−0.319 (C1W/C2W/C1C) |

So: the Card-2 time-linked datum held everything it actually pins (pinned-pair
IF and channel sums) — and the sum-to-zero carry made the *other 31 satellites
pay* for G01's step (~0.3–1.0 m each, uniform per channel, invisible to
pairwise spans). The +49 m consumer-visible damage lives entirely in
combinations the datum does **not** pin, i.e. in data-determined directions.

### 2.2 The mechanism

At the excursion epoch the solve moves along the cheap global common mode
{all satellite clocks −δ, all receiver clocks −δ}: phase and code residuals are
invariant along it — **except at the two hard pins** (G01 satellite clock,
MBAR00UGA receiver clock), which cannot move. The pin converts G01's
clock↔code split from a gauge direction into a *measured* quantity, and the
excursion pressure that every other satellite dumps into its soft clock
(q = 0.3 m²/ep, recovers in ~2–4 epochs) lands on G01's **near-static** CB
states instead. This also *derives* the handoff §4.2 discriminator: damage ∝
the excursion's common-mode (one-sided) projection — the two-sided morning
event (common/diff 0.05) had almost no projection onto the pins and cost
nothing; the one-sided evening event (ratio 5.4) ratcheted the pivot.

### 2.3 The arithmetic closes on both sides [M]

Steady-state Kalman gain of a CB state: K = √(q/R_eff).

- **Decay:** observed τ = 3.79 h = 455 ep ⇒ K = 2.2e-3 ⇒ R_eff = 6.2e-4 m²
  ⇒ σ_eff ≈ 25 mm/epoch network-wide code information on one satellite's bias
  ≈ 0.3 m/√(≈250 stations). Self-consistent with the configured q = 3e-9 m²/ep
  to ~15 %.
- **Injection:** K × (~4600 m clock-space excursion) ≈ 10 m/epoch × the few
  out-of-band epochs ≈ the observed 30–49 m step.

The persistence is **not** the Card-2 datum rows (they held); it is the CB
states' own near-static random walk with **no jump/event response** — a
standing-invariant-#1 violation of exactly the Card-4-slip-hole class, one
level down: the C3c/D5 machinery guards the *ambiguity* carry; nothing guards
the *code-bias* carry. `CarryPrior.inflate_q` reinit machinery already exists
for clock-monitor-flagged satellites (`NetworkRunTypes.hpp:108`,
`ApplicationNetworkAssembly.cpp:1247-1257`) but `clock_runaway_check` was off.

**Recurrence:** guaranteed for any one-sided excursion while (pivot hard pin +
near-static CB + no event response) coexist. The handoff's Q2 hypothesis is
confirmed in refined form — not "sole free parameter", but "the only satellite
whose split is data-exposed *and* stiff-carried".

---

## 3. Q1 — the a00 collapse. Anatomy established; trigger needs one trace

### 3.1 What the artifacts rule in and out [M]

- **No precursor** — confirmed now also in the channels the handoff didn't
  check: `slant_resid_{mean,rms,max}`, `n_los_gt3sigma`,
  `n_stations_gt3sigma` are exactly baseline at t−1 for all three events.
- **The three events are different directions** in coefficient space
  (cosines −0.05 / −0.48 / +0.04) — *not* one recurring null mode. Power sits
  at degrees 0–3; **n=1,m=1 out-steps a00 in two of three events**
  (+2018 TECU vs +1236 at 07:32). The a00-only band check watches the wrong
  scalar even before its 23.8× edge-trigger under-count.
- **Formal σ is frozen** (a00_std 0.622→0.623 TECU across a +1236 TECU step;
  all-coefficient mean 0.15 TECU) — the step is ~1500–2000σ against the folded
  prior. The prior injection (`networkInjectShVtecPrior`,
  `NetworkRunContext.cpp:800-901`) is healthy: Kaula + RW-toward-previous,
  always on. A weighted solve cannot move 2000σ against its own prior unless
  the *data side* moves: and the SH rows' innovation is literally
  `−x(iono_idx)` (`ApplicationNetworkAssembly.cpp:374`) — **the field follows
  the per-LOS iono states; the SH collapse is a symptom, not the disease.**
- **Smooth-mode observability is weak by construction:** the per-constellation
  R = diag(var) + (50 m)²·11ᵀ deliberately kills each station's common mode
  (receiver-DCB freedom), so a00-like modes are observed only through the
  mapping-function *spread* (the code comment says exactly this,
  `ApplicationNetworkAssembly.cpp:304-308`).
- **Chronic saturation:** 69 % of LOS exceed 3σ against the SH constraint in
  the *quietest* hours (documented as expected: 70–90 %,
  `NetworkIonoDeweightGate.hpp:21-23`); slant-residual mean ≈ 10 m all day.
- **Deweight gate:** very active — 174 stations ever, **peak 72
  simultaneous**, 326 engage / 307 release (`main_log.log:7539`) — but
  network-relative (median-centred ⇒ structurally blind to common-mode
  excursions) and **not the proximate trigger at onset** (formal σ unchanged ⇒
  weight pattern unchanged that epoch). With floor 0.0 an engaged station's
  whole SH block drops (`ApplicationNetworkAssembly.cpp:330-331`) and its
  per-LOS iono floats.

### 3.2 The enabler and the trigger [H]

The phase frame that should forbid a joint clock/iono/field slide is porous:
**2,421,756 D5 innovation re-taps over the run = 1,121/epoch ≈ 13.5 % of the
~8.3 k carried DDs re-tapped fresh every epoch**, with LS-VCE σ̂₀ = 4.74
(`main_log.log` run-end) [M]. The working hypothesis for the single-epoch
onset: an epoch where the re-tap/release pattern (D5 avalanche and/or a
deweight release wave re-coupling stations whose iono floated for hours)
momentarily releases the weakly-resisted joint mode; the solve slides until
secondary stiffness catches it; sign and mixing differ per event (hence the
uncorrelated step directions). Untestable from this run's artifacts — the
per-epoch re-tap count, deweight state, and SH spectrum are not traced (§6).

---

## 4. Q3 — the quiet-hour wander is satellite-clock wander. ANSWERED

Hour-to-hour delta of per-sat deviation from the constellation median, quiet
hours 06–18 [M]:

| component | median \|Δ\|/hr | median hr-pair span |
|---|---|---|
| total IF (CLK + IF_W(CB)) | 2.68 m | 16.1 m (handoff's ~18 m ✓) |
| **clock alone** | **2.83 m** | **16.5 m** |
| IF(C1W,C2W) of CB | 0.18 m | 3.2 m |
| IF(C1C,C2L) of CB — the pinned pair | 0.009 m | 0.14 m |

The wander **is** the clocks. G01 is among the *steadiest* satellites
(1.98 m/hr vs 5.6 max) because its total-IF rides the stiff CB carry. The
0.18 m/hr on the consumer pair's CB-IF is the slow integral of the same frame
wander leaking into the near-static CB states at gain K (order-consistent with
§2.3). Structural reading: nothing carries absolute per-sat clock information
across epochs — the emitted clock is re-derived every epoch against a frame
whose only stiffness is the CB carry plus a phase graph that leaks 13.5 %/ep.
Fixing Q3 is therefore not a tuning exercise on code weights; it is "give the
clock product a guarded temporal anchor" (clock model / C1 clock-rate product
with event response), or accept the wander and emit honest URA.

---

## 5. Q4 — the disc counter is a datum channel, not a fault channel. ANSWERED

From `network_solution.csv` counter diffs [M]: **5,126 CB bumps concentrated in
128 epochs**, arriving as mass waves of ~150–204 keys (a channel-membership
change bumps *every* satellite in that channel —
`NetworkProductEmitter.cpp:345-349`); the two pivot handovers (tow 319260,
322530 — the handoff's two `datum_changed` epochs) contribute 402 (7.8 %).
CLK bumps: 0 all day (`emit_clock_disc` off). PB bumps: 3,711.

Triggers in code: forced datum change, per-channel set change, per-sat
band-tuple change, Card-2 reseed — all **structural**; the only
magnitude-sensitive trigger is `value_jump` (`NetworkProductEmitter.cpp:336`),
gated by `code_bias_disc_jump_test` = false. At 334020 G01's CB bumps = **0**
[M]. Conclusion: the emitted stream today carries **no usable magnitude
integrity** on B lines, and worse, a counter-following consumer would reset
~200 satellites' CB state ~128 times per day for bookkeeping reasons while
integrating straight across the one real 49 m fault.

---

## 6. What the next run must trace (all `trace_*`/SAFE — no fingerprint cost)

1. **Per-epoch D5 flag/re-tap count** (and per-group), to test the §3.2
   avalanche hypothesis — run-end totals exist, per-epoch does not.
2. **Per-epoch deweight state** (engaged set size, min/mean w_s) — `n_adapt`
   in `network_epoch_status.csv` is a *different* counter (0 all day [M]).
3. **SH estimability spectrum** per epoch around excursions
   (`NetworkEstimator::dumpEstimabilitySpectrum`) — λ_min of the joint mode is
   the direct test of the near-null-slide story.
4. **σ0 on the emit path** — the `sigma0` column is 0.0000 on all 2160 rows
   [M]: no post-fit residual statistic exists on the emit path at all.
5. `trace_dd_fractional` (Q6, known fingerprint-safe).

## 7. Recommendations (delta over handoff §7)

- The re-convergence arm-together list (handoff §7) gains one item and one
  emphasis: **wire an event response for the CB carry** (invariant #1) — the
  existing `inflate_q` reinit path extended to the pivot's CB under a clock
  monitor flag is the minimal shape; `code_bias_disc_jump_test` alone only
  *signals* (and only helps a consumer that resets, at ~0.6 m/√ep re-tracking
  cost); and the σ̂0/A4 ceiling should be armed so a −4500 m clock epoch
  cannot ship `gate_ok=1`.
- Health gate: band-check the low-degree coefficient *vector* (or total-field
  RMS), not a00 alone (§3.1); count epochs; make it gate (handoff §7 already
  says gate + un-throttle).
- Datum design question to decide deliberately: `selectIfPinPair` pins the
  lexically-first pair (C1C,C2L) — the stable combination is not the one
  consumers apply (C1W,C2W). Re-pointing the pin at the consumer pair is a
  datum re-reference (fingerprint-breaking, needs fresh convergence) but puts
  the carry's stability where users look.
- Q5 (position-domain) remains the cheapest unmeasured item: the closed-loop
  replay needs no producer re-run (`NetworkProductReader::replay`;
  `GnssConfigLoadNetwork.cpp:3218-3262`); grade the A/B *model-toggle* design
  per handoff §7 — with the added prediction from §2 that a consumer using
  C1W/C2W sees the +43 m era as a G01-specific pseudorange fault its RAIM
  should exclude, while a C1C/C2L consumer sees almost nothing: the delta
  between those two arms is now a *predicted* signature worth one replay each.
