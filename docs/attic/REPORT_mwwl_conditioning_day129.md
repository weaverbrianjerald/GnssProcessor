# Can the MW/WL cascade CONDITION the DOY-129 305-station network against the SH detonation?

**Verdict: no — and the ordering question has a third answer.** The cascade is neither a
conditioner nor a victim of the SH-detonation disease. It is **decoupled** from the disease
(its de-bias never reads the solve) and **geometrically dead** on a 305-station global
network (its only estimator-facing constraint is manufactured by a phase bridge whose error
budget is 8–17× smaller than the double-differenced ionosphere on the chords this network
actually forms). Arming it as shipped would write **wrong-by-one-cycle widelane integers into
the stiff `q_z = 1e-4` carry at a 27–71 % rate** — a standing-invariant-#1 violation, not a
stiffening.

Session 13e1dc21, 2026-07-28. Instruments: `scripts/mwwl_event_census.py` (new,
estimator-free), `scripts/mwwl_cond_arm.py` (new). Code audit: 14 agents, 7 questions ×
independent adversarial verification, 0 errors.

---

## 1. The disease does not touch the data, and does not touch the cascade's information

Estimator-free census of all **305 rosters stations**, raw RINEX, GPS, 17:30–21:30 UTC
(481 epochs at 30 s), spanning the 19:26:00 (tow 329160) SH detonation, the 19:34 epicentre
spike and the 20:47 ratchet.

| band | dI_med (m/30 s) | dI_p99 | icres_med (m) | icres_p99 | LLI/ep | MWjump/ep | EWL med\|f\| | EWL circR |
|---|---|---|---|---|---|---|---|---|
| quiet 17:35–19:00 | 0.0178 | 0.202 | 1.82 | 34.7 | 28 | 88 | 0.024 | **0.969** |
| pre 19:00–19:21 | 0.0175 | 0.277 | 1.54 | 28.1 | 31 | 90 | 0.024 | **0.979** |
| **detonation 19:26–19:31** | **0.0176** | 0.289 | **1.27** | 29.8 | 31 | 89 | **0.024** | **0.976** |
| spike 19:31–19:40 | 0.0173 | 0.218 | 1.29 | 31.5 | 34 | 94 | 0.020 | 0.975 |
| ratchet 20:47 | 0.0175 | 0.220 | 1.80 | 26.8 | 35 | 101 | 0.026 | 0.978 |

`dI` = |Δ slant ionosphere from PHASE| per epoch (network median over ~3060 LOS).
`icres` = |code-minus-phase ionosphere| after per-arc levelling — **the error the code-only
SH field actually eats**.

- **There is no data-side insult at 19:26.** The phase TEC rate is flat to ±2 % across four
  hours; the code-side ionosphere residual is *lower* during the detonation (1.27 m) than in
  the quiet baseline (1.82 m); LLI and MW-jump rates are unchanged. This confirms the
  state-endogeneity verdict from `runs/raim_cold_event` **from a completely independent
  instrument** — observations rather than filter state.
- **The cascade's extra-widelane information is pristine and unmoved**: EWL(2,5) DD
  median |frac| 0.019–0.029 cyc, circR 0.96–0.98, unchanged through every event.
  L5 data present at **293 / 305** stations (297 declare it — 4 are the header≠data trap);
  ~52 % of LOS carry L5 (18 of 32 GPS satellites).

So the "cascade is a victim" hypothesis is **falsified at the source**: its raw statistic is
untouched by the disease.

---

## 2. But the cascade's only estimator-facing constraint is manufactured by the bridge

Structural audit (all claims file:line-verified and adversarially re-checked):

| link | finding |
|---|---|
| EWL rows → normals | **Never.** `if (nmw.ewl_fold_rows) …` — default **false**; the EWL lane is decision-only (`NetworkMwWlGlue.cpp:1004`). |
| WL decision source | **Bridge only.** The walk is gated `(wl_inject_on \|\| lane_fix_on \|\| lane_bridge_on) && !ewl_fixed.empty()` (`NetworkMwWlGlue.cpp:439`). |
| an iono-free WL arm? | **Not constructible.** `lane_carry.enable` *requires* `mw_wl.ewl_enable` (`GnssConfigLoadNetwork.cpp:2714-2720`, fail-loud), and `bridge_meas=false` suppresses only the raw-GF *increment*, not the decision. The loader's promise of an "MW(1,2) fallback" when `wl_inject_enable` is set without `ewl_enable` (`:2588-2592`) describes a path the code does not implement — `wl_inject_on = ewl_on && …` (`ApplicationNetwork.cpp:575`). |
| bridge ↔ SH circularity | **None.** The de-bias reads `x_lin(iono_idx)` — a **frozen, arc-start, code-derived seed** (`MeasurementModel.cpp:414`, never written back) — not the solve. `networkMwWlInject`'s signature takes no estimator, no `x_s`, no `sh_model`. |
| ambiguity → SH coupling | **Live within one epoch.** Ambiguity and slant-iono share `N_rr`; carried DD priors fold post-Schur through `Gacc += N_sr · Y` onto the SH shared rows, with `M = (Pᶻ+B)⁻¹` — so stiffening *amplifies* the SH perturbation. Magnitude not measured. |

The conditioning **mechanism** therefore exists. What does not exist is a clean way to drive
it: the disease-immune rungs (EWL, MW(1,2)) are deliberately kept out of the normals, and the
one rung that reaches the normals is the iono-sensitive bridge.

---

## 3. The bridge is a short-baseline instrument, and this network has no short baselines

Bridge oracle, 183-station (W,Q)-channel group, truth = whole-window MW(1,2) arc median,
n = 10 trailing epochs, **at the detonation epoch 19:26:00**:

| baseline bucket | DDs | med \|DD iono\| | **bridge integer hit** | de-biased hit | MW(1,2) hit |
|---|---|---|---|---|---|
| < 100 km | 68 | 0.19 m | **0.971** | 0.868 | 0.985 |
| 100–400 km | 219 | 0.48 m | **0.945** | 0.685 | 0.968 |
| 400–1500 km | 682 | 0.71 m | **0.628** | 0.613 | 0.957 |
| 1.5–4 Mm | 896 | 1.63 m | **0.287** | 0.257 | 0.933 |
| 4–12 Mm | 243 | 3.35 m | **0.091** | 0.123 | 0.897 |

The mechanism is arithmetic: `z_WL = N₁₂ − 0.5049 · I₁_dd[m]`, so a 0.1-cycle lane needs the
DD ionosphere to **0.198 m**. On global chords the unmodelled DD ionosphere is 1.6–3.4 m —
**8–17× the entire error budget**. This answers `SPEC_ewl_wl_cascade_card` §11 Q2 with data:
the bridge's iono term is **local**, and it does not survive a global network's chord geometry.

**De-biasing does not rescue it — it destroys the one regime where the bridge worked.**
Gated wrong-integer rate at 19:26 with three de-bias inputs (the third is a proxy for what the
code actually subtracts, `x_lin(iono_idx)` = a frozen arc-start code seed; I model it as the
code ionosphere ~2 h earlier):

| bucket | raw bridge | de-biased, phase-levelled code | de-biased, **stale arc-start seed** |
|---|---|---|---|
| < 100 km | **0.0 %** | 4.0 % | **12.5 %** |
| 100–400 km | **1.4 %** | 23.2 % | **17.6 %** |
| 400–1500 km | 28.2 % | 25.4 % | 29.0 % |
| 1.5–4 Mm | 59.7 % | 71.9 % | 65.7 % |
| 4–12 Mm | 91.3 % | 75.0 % | 95.8 % |

The de-bias input's own error exceeds the bias it removes wherever the bias was small, so it
is **net-harmful at short baselines and no help at long ones**. (Proxy caveat: the shipped
path de-biases per sample inside the Welford with a `computeIonosphereSeed` value; my proxy is
directionally the same but the magnitudes are indicative, not exact.)

**Roster geometry** (305 stations): nearest-neighbour median 232 km and 68 % of stations have
a neighbour within 400 km — but only **0.85 % of the 46 360 station pairs** are under 400 km,
and the all-pairs median is **8341 km**. Chord selection is therefore worth ~10× in bridge
yield, and today it is not geographic: `tree_edge` comes from the eq34 pinned-ambiguity
election (`NetworkDdPriorBuild.cpp:606,640`), not from proximity.

---

## 4. What actually reaches the stiff carry: the wrong-integer injection rate

Applying the **shipped** FFR gate `|z − round(z)| < 0.15` (`network.mw_wl.wl_fix_frac_cyc`):

| bucket | bridge admits | **of admits, WRONG** | \|err\|=1 | \|err\|≥2 | after MW(1,2) closure | MW(1,2) admits | MW WRONG |
|---|---|---|---|---|---|---|---|
| < 100 km | 85.3 % | **0.0 %** | 0.0 % | 0.0 % | 0.0 % wrong | 61.8 % | 0.0 % |
| 100–400 km | 47.0 % | **1.9 %** | 1.0 % | 1.0 % | 1.0 % wrong | 47.9 % | 1.9 % |
| 400–1500 km | 30.5 % | **34.1 %** | 29.8 % | 4.3 % | **27.5 % wrong** | 57.5 % | 1.8 % |
| 1.5–4 Mm | 31.8 % | **69.8 %** | 42.8 % | 27.0 % | **51.1 % wrong** | 55.0 % | 1.6 % |
| 4–12 Mm | 29.6 % | **91.7 %** | 23.6 % | 68.1 % | **71.4 % wrong** | 48.1 % | 1.7 % |

Three things this establishes.

1. **The gate's admit rate saturates at ~30 %** on long baselines — matching, to within
   measurement noise, the *independently derived* prediction from the code: the frac gate is
   periodic, admitting within ±0.297 m of every multiple of 1.980 m, i.e. a 30 % duty cycle
   on the iono-error axis. Theory and measurement agree.
2. **The dominant error is wrong-by-one cycle**, which is exactly what the ±1.98 m periodicity
   predicts for 0.7–1.6 m of DD ionosphere — and the shipped MW(1,2) closure backstop
   (tolerance `0.5 + 3σ` ≈ 1.16 cyc at n = 10) **cannot catch it**. After the closure, 27–71 %
   of surviving admissions are still wrong. The companion σ gate is structurally blind: a
   *constant* de-bias error contributes zero Welford variance (the code records this
   falsification itself at `NetworkDdPriorBuild.cpp:206-208`).
3. **The iono-free MW(1,2) statistic is ~40× more reliable at long baselines** — 1.6–1.9 %
   wrong at *every* bucket, baseline-independent — and it is the rung that cannot decide.

**This is disease-independent.** Repeating at a quiet epoch (18:30:00) gives bridge-wrong
0.0 / 1.0 / 32.7 / 59.2 / 87.3 % — the same. The bridge does not fail *because* of the
detonation; it fails on geometry, all day.

MW(1,2) arc-length sweep (hit rate, worst→best bucket): n=5 0.81–0.94 · n=10 0.90–0.99 ·
n=20 0.91–1.00 · n=30 0.95–1.00 · n=60 0.96–1.00. It plateaus near 0.96 rather than falling
as √n theory predicts — a systematic per-arc floor. *(Caveat: the truth is a long-arc MW(1,2)
median, so MW's own rate is optimistic by correlation; the bridge's is not. The two agree at
<100 km, which cross-validates the truth.)*

---

## 5. Two mechanism findings worth keeping

**(a) The D5 storm erases the SH deviation pins.** `carry_slipped_ambs` erases `sh_dev_carry`
per (station, satellite), and those entries supply `r.innov` / `r.var` for the SH pin rows.
A 92 %-re-tap seam therefore mass-reverts every per-LOS SH deviation pin to the cold seed
(dev 0 at full representativeness σ) — **a direct, solve-affecting mechanism for the SH field
going code-determined**, independent of the lane machinery. This is a concrete causal link
for `project_day129_gate_policy` defect 2.

**(b) The out-of-band memory is real within a run, and absent across a resume.** When D5
re-taps a chord, everything the lane put *inside* the carry is erased (`d5GateHalf` /
`laneFixErase`), but the decision source — the per-station `MwSlipDetector` Welford
accumulators — is untouched, so a re-tapped chord can be re-pinned at full strength in **one
epoch**. That is precisely the capability the 92 %-re-tap collapse needs. **But
`MwSlipDetector` has no serialize/restore**, so on a warm resume every arc restarts immature.
Any warm-start arm must allow ≥ `min_arc_epochs` (default 10) before the cascade can act.

**Cost:** arming `lane_carry` flips the carry into joint 2K pair blocks (~2×/4–8× on the carry
stages), and `resolve_dropping()` re-runs `buildCarryDdPriors()` on every RAIM adapt pass
(up to `kMaxAdapt = 5`) — so the cascade is **up to 6× more expensive during a detonation**,
exactly when it is least able to help.

---

## 5b. Estimator-side smoke (305 sta, cold at 19:00, cascade fully armed)

`runs/mwwl_cond/casc` — guards_cont leg shape + `ewl_enable` + `uwl_enable` +
`lane_carry.enable` + `bridge_sample_debias`, 8 Eigen threads. **Stopped after epoch 3** to
return the machine to the co-owned anchor (see §7).

- **Cost: 88.6 then 75.7 s per epoch** at 8 threads. `solve-glue` dominates at 33.7–35.7 s;
  `dd-build` only 0.6–5.9 s; `install-post` 9.1–11.8 s. The lane bill lands on the joint-2K
  carry stages, as the audit predicted — not on dd-build.
- **Clean incremental measurement of `lane_carry`** (same build, same 8 threads, same epochs):
  `ewl_enable` + WL-1 float layer **without** lane_carry (`runs/mwwl_cond/geom`) runs
  **56.2 / 48.6 / 44.3 / 47.6 s/ep**; adding `lane_carry.enable` takes it to 88.6 / 75.7 —
  i.e. **lane_carry alone costs 1.6–1.9×**, which is the joint-2K pair-block flip. (The
  anchor's 45–52 s/ep is cascade-OFF but at 20 threads, so it is not a clean comparator.)
- ⚠️ **A cold 305-station start at 19:00 emits NOTHING**: the SH gate engages at the first
  epoch (`a00 −1406.9 TECU, rms_all 352.74`) and the run ends "0 epochs emitted, 0 skipped …
  of 2285 synced". Cold micro-windows are therefore usable for engagement/cost probes only —
  they cannot substitute for the warm anchor when studying the field.
- **Lane population: 2317–2328 primary cycles/epoch** in the `G L1C+L2W [PAIR]` group
  (2652 arcs). But ~2888 arcs stay in **residual per-band** groups (L2L 1054, L2X 726, L1W 553,
  L2S 244, L1L 206, L1X 90, L1C 15) — roughly **half the arc population is outside the lane
  pair basis** at 305 stations, the UWL card's `skip_basis` problem at scale.
- `decided = held = injected = 0` for all 4645 traced rows — correct for a cold epoch 1–2
  (EWL needs n≥2, MW increments n≥10). A cold micro-window cannot show steady-state
  engagement, and its first epoch withholds anyway (`SH gate ENGAGED, a00 −1406.9 TECU`) —
  a cold-start artefact, not the disease.

**The one load-bearing item not yet measured to a distribution** is the *baseline distribution
of the estimator's lane cycles*. The lane trace records the chord `(station, sat)` but the
cycle closes through the eq34 tree, whose path it does not record. Structural inference: the
Eq-34 datum is a **single-receiver chain** — one datum receiver per (constellation, signal)
component — so a chord's cycle closes back through that datum receiver, making the effective
DD baseline global (roster all-pairs median 8341 km), i.e. §3's bottom bucket.

**Estimator-side confirmation of §1 and §2, from run-end counters** (`runs/mwwl_cond/geom_ccs`,
305 sta, 14 epochs, `cold_clock_seed` armed):

- **WL-2 EWL lane: 3372 rows decided, 3357 INTEGER-FIXED via FFR, mean R = 0.0001 cyc²** over
  32 850 primary cycles inspected — and the counter line states it outright: *"rows NOT folded
  into the solve"*. The EWL rung is near-perfect at 305-station scale **and contributes nothing
  to the estimate**. That is §1's census result and §2's "decision-only" structural claim,
  both confirmed from inside the estimator.
- WL-1 float layer: 4399 WL rows folded, mean R = 0.0984 cyc² (σ ≈ 0.314 cyc) — the honest
  MW code-noise level, matching the census.
- **`cold_clock_seed` works and this lineage lacks it**: it cut `x_lin1_m` from **269 352 m
  (269 km) to 57 m** at station 1. The guards_cont leg does not set it; arm it for any lane
  work here ([[project_network_coldseed_clock_leak]]).

## 5c. ★ Lane-cycle geometry — MEASURED, and it closes the argument

The F6 dump was hard-capped at 8 rows of one block (`nrow = std::min(wb.rows.size(), 8)`,
`NetworkMwWlGlue.cpp:160`). Two log-only knobs now lift it — `network.mw_wl.f6_max_rows`
(default 8) and `network.mw_wl.f6_all_blocks` (default false), both reproducing the legacy
dump exactly. *(An earlier revision of this report inferred a "99.7 % `x_lin` quarantine" from
that 8; that was wrong — arming `cold_clock_seed` fixed `x_lin` by four orders of magnitude and
the dump still returned exactly 8. Retracted.)*

Uncapped run (`runs/mwwl_cond/geom_full`, 305 sta, `cold_clock_seed` armed): **939 cycles,
4410 arcs.** Cycle span = max pairwise station distance inside the cycle (the ionosphere lever
arm), against the buckets §3/§4 measured the bridge in:

| bucket | share of lane cycles | bridge WRONG % (§4) | after MW closure |
|---|---|---|---|
| < 100 km | **0.1 %** | 0.0 % | 0.0 % |
| 100–400 km | **0.2 %** | 1.9 % | 1.0 % |
| 400–1500 km | 7.5 % | 34.1 % | 27.5 % |
| 1.5–4 Mm | 19.6 % | 69.8 % | 51.1 % |
| **4–12 Mm** | **72.6 %** | **91.7 %** | **71.4 %** |

Span distribution: min 19 km, p10 1644, **median 7433 km**, p90 10 756, max 12 658.
**92.2 % of lane cycles are ≥ 1500 km; 0.3 % land in the < 400 km regime where the bridge
works.** Cycles span 2 / 3 / 4 distinct stations (671 / 209 / 59) — a third are multi-station
tree paths, i.e. longer lever arms still.

**Folding the measured per-bucket wrong-rates against the measured cycle population:**

> **≈ 83 % of gate-admitted WL integers on this network would be wrong**, and **≈ 64 % still
> wrong after the MW(1,2) closure backstop.**

The star structure is visible too: **`BELE00BRA` appears in 679 of 939 cycles (72 %)** — it is
the eq34 datum receiver for that component, so cycle baselines are essentially
(station → datum receiver) distances. The single-receiver-chain inference of §5b, confirmed.
This is also precisely why lever 1 (geographic chord selection) is the highest-value fix: the
population is not *inherently* global, it is global because the datum tree is a star.

---

## 6. Ordering verdict and the constructive path

**The disease must be addressed first — but not for the reason the framing assumed.** The
cascade is decoupled from the SH field; it is not poisoned by the detonation. It cannot
condition this network because its WL-manufacturing path is *geometrically* dead here, and it
would be actively harmful if armed. Fixing the disease would not by itself make the cascade a
conditioner; the geometry fix is independent and must come first.

Ranked levers:

1. **Geographic lane-chord selection.** The bridge is 0–1.9 % wrong below 400 km and 68 % of
   the roster has a neighbour within 400 km. Preferring proximity in the pair/chord election
   would move most cycles into the working regime. Largest single win; no new estimator maths.
2. **An MW(1,2)-decided WL (SPEC §3.5 / Increment 4), hard-FFR-gated.** Iono-free and
   baseline-independent, but 1.6–1.9 % wrong at n=10–60 is *not* FFR grade for a stiff carry
   (invariant #1) — it needs a gate that reaches ≲1e-4, i.e. long arcs plus the closure, or it
   must enter as a soft prior rather than a set.
3. **Re-measure the `ewl_fold_rows` trade under a sick network.** Folding was rejected on a
   D5 cost measured in a *healthy* network (679 vs 348). The EWL is the only integer
   information that survives a detonation intact (circR 0.976 through it); the trade should be
   re-taken where it matters.

**Recommendation on the prescribed warm-start arms:** do **not** arm `network.mw_wl.*` /
`network.lane_carry.*` on the DOY-129 305-station resume as originally scoped. The treatment
is now predicted — from measurement, not speculation — to inject wrong-by-1 widelane integers
into the stiff carry across most of the DD population, and arming it on a resume additionally
changes carry membership (`ewl_carry_exclude` drops every GPS L5 ambiguity) with **no
fingerprint check**, where a shape mismatch is a silent per-block skip that still logs
"BRANCH-FAITHFUL". If a field arm is wanted, the informative one is lever 1's A/B, not the
stock cascade.

---

---

## 7. Machine-state note for the guards_cont owner (session d6031295)

`gnss_processor_gcont` is **healthy** — ep 0115 at 21:33, ~43–52 s/ep, writing log, products
and per-epoch history normally. (An earlier note in this report claimed a stall; that was a
clock-arithmetic error on my part and is withdrawn.) I did not touch `runs/guards_cont` —
read-only inspection of `leg.json`, the `seed/` listing and `stdout.log` only.

**Scheduling consequence that does stand:** at ~45 s/ep the anchor needs ≈ 8 h to reach
ep 0721 (19:00 content, the warm-start anchor point) and ≈ 11 h to reach 21:29:30. Warm-start
arms off this history are not available before roughly 05:30–06:00.

My own 305-station runs use 8 Eigen threads and ≈ 48 GB, against the anchor's 20 threads /
33 GB on a 20-core / 137 GB machine; both fit, but they do share cores.

---

## Instruments left behind

- `scripts/mwwl_event_census.py` — `parse` / `analyze` / `oracle`. Estimator-free, windowed,
  cached (re-analysis is seconds). `oracle` grades any lane against a long-arc MW truth,
  bucketed by baseline, and reports the shipped-gate wrong-integer injection rate.
- `scripts/mwwl_cond_arm.py` — derives `ctl` / `lane` / `casc` arms from the guards_cont
  anchor's own `leg.json`; documents the loader interlocks in its header.
- Cache: `…/scratchpad/mwwl_census_cache/census_173000_213000.pkl` (305 stations, 4 h).
