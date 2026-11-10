# LV-SEED: where the remaining per-LOS level is born (pre-registered reads R1 to R9)

**STATUS**

- **id:** LV-SEED
- **topic:** ionosphere
- **status:** closed
- **summary:** H1 to H3 all REFUTED: the per-LOS level is not born at arc start, not ionospheric, not station geometry; the follow-up reads pointed at the anchor election and the satellite clock.
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.trace_station_block` (card ZP's measure-only trace); `estimation.ionosphere_weighting.elevation_cutoff_deg` (arms c7 15.0, c7b 20.0; 25 stays); `estimation.require_bia_for_signals`; arm c3 `apriori_clock_anchor_sigma_m` 0.02
- **hashes named in the text** (commit / exe / byte-cert): `cb8e39a`, `ec66d42`, `2c4be49` (ZP worker), `87a33bf0` (smoke), `6b75e22` (R5), `8d2ccb2`, `13fd225` (c7 / c7b worker), `78d0875` (main Release); exes `D7D7CFC1…`, `4CD7D664…`
- **run dirs named in the text:** `runs/_cm_pvb`; arms `_cm_b7`, `_cm_b1`, `_zp_ci`, `_zp_pl`, `_zp_pva1h`
- **source:** `docs/archive/SPEC_event_harness.md` sections 24.13, 24.15

**Outcome.** The question: where the per-LOS code level left on the leveling vehicle (PV-B) is born. R1 to R4 ([CE](../network-vehicle/CE-chain-era-bisect.md) section 24.13) refuted all three hypotheses: only 0.39 to 0.46 of arcs start within 0.3 m of their settled level (bar 0.7). R5: G06 is an outlier record (1.78 % of the wire's D_phase sum of squares), elected onto a newborn 15° anchor; tie cutoffs of 15° and 20° regressed, so 25° stays. With R7 ([Q3-F](../clock/Q3-F-self-contained-time-link.md)), R9 and R10, [CS](../clock/CS-clock-step-signalling.md) section 24.20 names the wire's within-arc wander as T4-refloat re-realizations of the satellite clock; the owed anchor election rule (elevation, minimum age) was built in [RF-2](../../refound/RF-2_held_basis_2026-09-15.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.13 Card LV-SEED — where the remaining per-LOS level is born (pre-registered reads on the EXISTING PV-B rows; no new mechanism)

Three hypotheses, each with a read that can refute it on `runs/_cm_pvb` (with `_cm_b7` and `_cm_b1` as contrasts) — no producer run
except a trace-only twin of PV-B if a trace is missing (products must hash identical):
- **H1 — the level is born at arc start.**  On the leveling vehicle a new arc's phase level is set from its first code epoch(s) and held
  at zero q, so its code residual level is −δ_seed for the arc's life.  **R1:** per LOS arc (≥ 30 rows) the residual mean over the arc's
  epochs 1–3 against the mean over epochs 10–end; the fraction of arcs with |Δ| < 0.3 m; the correlation of the settled level with the
  first-epoch residual; arcs born inside the run against arcs present at epoch 1.  H1 named if the settled level is set at birth
  (fraction > 0.7, r > 0.8); then **R4:** the wire's D_phase within-arc series per satellite against that satellite's arc-birth epochs at
  the observing stations — the fraction of |ΔD| > 0.3 m steps within ± 1 epoch of a birth, against a shuffled null.
- **H2 — the level is ionospheric (at seed, or the tie's representation error).**  **R2:** per LOS, the C1C vs C2W level ratio and
  correlation (γ = 1.647 and r > 0.8 ⇒ dispersive); the level against the LOS's tie residual (the per-LOS iono state minus the SH field
  at its pierce point: `network_ionosphere.csv` — arm `trace_ionosphere` on a byte-identical PV-B twin if absent — with
  `scripts/sh_vtec.py`); and the b7 − PV-B per-LOS level change against elevation and station latitude.
- **H3 — station geometry.**  **R3:** a per-station position vector + clock fit to the per-LOS levels (the §24.6 test, now on a
  determined block): explained SS fraction, |d| and dU per station, against the RINEX `ANTENNA: DELTA H/E/N` and the roster; H3 named if
  the 13 ZWD stations carry |d| > 1 m with a consistent vertical and the others do not.
**Frozen reading:** H1 ⇒ the posture card is the vehicle + a CONVERGING arc level (a float ambiguity accumulating information over the
arc, or the level re-estimated from the arc's accumulated code — a design card), graded on the same three bars; H2 ⇒ the tie's
representation is the card (degree / stiffness / a per-station DCB datum, §24.10 (c)), one arm per lever; H3 ⇒ a roster correction
(data, not code) and PV-B re-run; nothing named ⇒ the residual is the field's code noise floor at this weighting and the bars re-base on
it.  Card ZP's trace on PV-B is read beside these as the block's rank / conditioning witness.

**§24.10 RESULTS — card ZP's measure-only half (2026-09-14 ~02:00 local; worker on worktree-agent-a5ab19146ca127d71, commits cb8e39a /
ec66d42 / 2c4be49, merged to main; exe `D7D7CFC1…`; suite 2089 / 283 (2072 + 17 fixture-gated skips, exit 0); smokes OFF / ON both
`87a33bf0…`; `--verify` PASS; manager grade).**  `network.trace_station_block` → `logs/network_station_block.csv` (the local block
`N_rr = A_rᵀ W A_r` + folded station priors with the shared states held at `x_s` — `reconstructStation`'s conditioning; rank at 1e-9 ×
λ_max; the three smallest eigenpairs labelled by every `StateManager` kind; `zwd_info_eff = 1/(N_rr⁺)_zz`; redundancy on the non-null
subspace, so a row lying partly in the null space reads CHECKABLE — unconstrained, not absorbed — which is why `rank < n_local` and
`zero_red` are complementary).  Two 1 h cold arms, 61 stations, the trace on (`_zp_ci` from `_cm_a0`, `_zp_pl` from `_pv_b`):

| epoch 1 / 5 / 20 / 22 | `carry_invariant` (`_zp_ci`) | `phase_leveling` (`_zp_pl`) |
|---|---|---|
| stations with rank < n_local (of 61) | 58 / 58 / 58 / 59 | **15 / 15 / 0 / 0** |
| null dimension p50 (max) | **7 (10)** | 0 (9) |
| condition number p50 | 6.6e8 → 5.7e8 | 3.0e8 → **2.7e4** |
| `zwd_info_ratio` p50 | 2.6e-5 → 3.0e-5 | 2.9e-7 → **3.4e-3** |
| reconstructed ZWD p50 (m) | −3.14 → −3.39 → −1.08 | **+0.086 → +0.193 → +0.253** |
| zero-redundancy phase rows | 0.227 → 0.225 | 0.691 → **0.007** |
| smallest eigenvector, epoch 22 | `amb` 34, `iono+amb` 10, `rxpb` 8, `clk+zwd+amb` 7 | **`clk+zwd+iono` 30**, `iono+amb` 9, `rxcb` 8, `clk+zwd` 7 |

WROC (the §24.9 station): CI `n_local 32, rank 24` all hour, `zwd_info_eff` 73–127 against raw 4.49e6, ZWD −9.2 → −10.5 m; PL rank 29 =
n_local from epoch 1, `zwd_info_eff` 0.77 → 8749, ZWD +2.77 → +0.195 → −0.085 m, its weakest direction changing identity from
`amb:G15:L2W` to `clk | zwd | iono` at tow 259620.  **Read:** under `carry_invariant` the local block is genuinely singular — a median
7-dimensional null space at 58 of 61 stations, unchanged over the hour, spanned by an L2W-ambiguity combination, the ZWD column keeping
1/38 000 of its raw information — the "near-rank-deficient at ignition" of §24.9 is rank-deficient, permanently; under `phase_leveling`
the same stations start nearly as badly and the carry closes the block within ~10 epochs (full rank 61/61, condition number down four
orders, ZWD information up four orders, phase zero-redundancy 0.69 → 0.007, ZWD physical), and the weakest direction becomes the
physical `clk + zwd + iono` trio — the block stops being under-determined and is merely correlated.  Card ZP's mechanism half (the
per-epoch ZWD prior) is retired by §24.12 (b2 / b2b / b8); the trace stays as the standard witness for any vehicle or datum card.

**§24.11 instrument gap CLOSED (commit ec66d42, trace-only, both smokes unchanged):** the P1-4 pass files a data row that is not phase and
touches a satellite `CLK:` column as CODE (the counterpart of the phase side's ambiguity-column touch); such a row carries no `signal`
label (per-STATION / per-SATELLITE pools only); the same gap stays open in `NetworkRowClassify::classifyRow` (prefit census, seed
tripwire, topology, FUSE — solve-affecting under `prefit_fuse_sigma`; its own card).  PV-A re-read over 1 h (`_zp_pva1h`): 0 → 118 775 code
rows, all 61 stations, **per-station code rms p50 0.436 m (p10 0.236, p90 1.050, max 3.633)** — the standalone-PPP floor, and **4× below
PV-B's 1.79 m** while PV-A's wire (D_phase 1.07) and consumer (2.26 m) are NOT better than PV-B's.  Two consequences: (1) §24.12's b6 was
mis-specified — the CODE-OSB posture needs `external_sat_code_bias: true` + `require_bia_for_signals: true` beside `estimate_sat_code_bias:
false` + `enable_code_phase_bias: true` (the pl2 chain's recipe), or the loader forces W6 back on; the CODE-OSB arm has never validly run
on either vehicle; (2) the CHAIN-ERA class of §24.11's diff carries a 4× code-residual difference the vehicle does not explain, and a wire
difference it does not track.

### 24.15 Additions pre-registered from §24.13 (before any is read)

- **CE arms c7 / c7b** (on the running card CE, base `_pv_b.json`): `estimation.ionosphere_weighting.elevation_cutoff_deg` **15.0** (the tie
  admitted on every observed row) and **20.0**.  Same bars.  Predictions written down: the per-LOS level tail collapses (p99 below 25°
  → the above-25° 3.3 m), (i)'s p90 / max and (ii)'s count fall, (i)'s p50 barely moves; the wire's D_phase moves only if the tail was
  feeding the clocks.  A tail fix that leaves the p50 is recorded as a contributor to the tail, not to the bars.
- **Read R5 (card LV-SEED, follow-up on the existing PV-B / b7 / b1 traces):** per satellite, the within-arc D_phase rms of the census
  and G06's share of PV-B's 0.95 m (the wire with G06 excluded); the RAIM / pool verdict on DJIG/G06's rows over k 210..240 (which
  screen saw a 100 m residual and what it did); the G06 anchor-election record (`phase_datum_prioritized`: the candidates at tow 265470,
  DJIG's age and elevation, why PALM lost); the consumer's response around 265470.  Frozen reading: G06 carries > 30 % of PV-B's D_phase
  ⇒ the anchor election onto a newborn low arc is a wire defect with a named fix (minimum anchor age / elevation, a re-validation of the
  handover target — invariant #7) and its own card; < 10 % ⇒ an outlier record, the anchor rule still owed but not on the bars.

**§24.15 R5 RESULTS (2026-09-14 ~03:15 local; worker commit `6b75e22`, merged; existing traces only).**  **G06 carries 1.78 % of PV-B's
pooled D_phase sum of squares** (3.1 % of the rows) and is QUIETER than the pool (own within-arc rms 0.725 vs 0.953 m); excluding it moves
the pooled D_phase by ± 2 % and makes the clock LEVEL worse (0.736 → 0.78–0.81) — **the "outlier record" reading applies**; identical on
b7 / b1.  The noisy satellites are elsewhere: per-satellite within-arc D_phase rms **G29 2.18, G28 1.99, G32 1.94, G18 1.81 m**, then G27
0.97 and the pool below — four satellites carry the wire's excess.  What the producer did with DJIG/G06 (93 code rows at −75 … −142 m over
k 210..240): `network_raim.csv` **0 rows** for that satellite or station in the window (964 rows over the run); no withhold, no adaptation
(`n_adapt` 0); only MDB saw it, twice in 31 epochs (4.7× and 4.2× its threshold, action `observe`) — **a 140 m code residual invisible to
every acting screen**, because the LOS sits below the SH tie's 25° admission with a free per-LOS iono that absorbed 76 m of it.  The
election: PALM00ATA's arc ended at 265440; of the 12 candidates at 265470 the rule ("highest-coverage receiver tracking each satellite,
pref elevation 15.0°, sticky") found identical coverage (3 code + 3 phase rows) at all twelve and elected the LOWEST and YOUNGEST —
DJIG at 15.09° (0.09° above the mask) on its first epoch, over KRGG at 84.1° with a 209-epoch arc — the run's only cold PB-gauge
re-realization (1 cold / 60 continuous) and its worst PB frame prior-residual (|t| 12.3, ê 1.41 m); run-wide 43 elections, 20 onto
successors below 25°, 2 onto arcs younger than 5 epochs.  The consumer (WTZZ, Germany) was unharmed — 3D 1.20 m in a smooth 1.56 → 0.89 m
window, largest step 0.05 m, 0 resets — but was handed G06 L1C's −1.41 m declared step followed by 11 consecutive epochs of declared
discontinuities (a decaying −0.21 … −0.10 m), a bump rate of 0.55 against a window base rate of 0.007.  **Owed, not on the bars:** an
election tie-break on elevation and arc age (a minimum anchor age; invariant #7's re-validation of the handover target), and the screens'
blindness to a gross error on an untied low-elevation LOS (RAIM 0 rows, MDB observe-only) — both their own cards.

**R6 (pre-registered, same worker, existing PV-B traces): where the wire's excess lives.**  For G29, G28, G32, G18 (and G27, G06 as
controls) on PV-B, the D_phase within-arc series over the full 2 h: the share of the within-arc sum of squares (a) within ± 2 epochs of a
declared PB / CB discontinuity or an anchor election for that satellite, (b) in a per-arc linear drift, (c) the remainder; the number of
stations tracking the satellite per epoch and r(|ΔD|, Δn_stations); the elected anchor station's elevation and arc age per satellite over
time; and the satellite's eclipse status over the window (β angle / shadow crossing from the SP3 and the Sun).  Frozen classification:
(a) > 50 % ⇒ the excess is the re-realization mechanism at or beside declared boundaries (the census's arcs are declared, so a > 50 %
share means the declarations lag or lead the step); (b) > 50 % ⇒ a slowly varying satellite term (the a-priori clock / orbit model or the
anchor's own drift); station-count r > 0.5 ⇒ coverage-driven re-referencing; an eclipse crossing inside the noisy arcs ⇒ the T3 tails
(SPEC_amb_iono §6.10's re-admission edge) on the field.

**§24.14 RESULTS + §24.15 c7 / c7b (2026-09-14 ~04:00 local; worker on worktree-agent-a1e5de39467c4312c `8d2ccb2` / `13fd225`, merged;
exe `4CD7D664…` = main's Release at 78d0875 — the ZP merge certified, smoke `87a33bf0…`; manager grade).**  Corrections the data forced:
c4 and c5 as written in §24.14 were the GOLDEN's own values — the arms take pl2's (`layer2_max_degree` 4 → 0 + `mean_vtec_prior` 15 ± 30 →
32.24 ± 2; `frequency_mode` triple → dual); `global_vtec_sh.max_degree` is 8 in BOTH configs (the lineage's "deg-15" is not a leaf);
`estimation.require_bia_for_signals`, `model_corrections.require_atx_coverage`; no .BIA path key exists (PathResolver).  **Three arms cannot
run alone on the golden base** — c1 (EXT-CB with the golden's L5 signals: the S-basis null-space gate sees 17 near-zero eigenvalues
against a declared 1, all 240 epochs withheld — with W6 gone Card KP's OSB bound and Card 2's time-linked code-bias datum leave, and the
L5 signals have no CODE OSB to pin them), c5 (dual alone: W6's L5 code-bias columns registered but unobserved, 13 near-zero), c6
(`require_atx_coverage` STRICT drops exactly the L5 observations — a byte-identical gate failure to c5): **the class does not decompose**;
EXT-CB and the L5 rows are mutually load-bearing and only the pair (c0) runs.  **c0 ≡ PV-A byte-identically** (`pf_rows`, `pf_keyed`,
`zwd` sha256-identical, every run-end counter equal) — the 13 GOLDEN-NEWER pins are solve-inert on this lineage; on the comparable
last-hour statistic c0's (i) is **1.19 m**, not §24.11's 0.44 (a first-hour 1 h read): the chain-era difference is 1.79 → 1.19.

| arm | (i) p50 / p90 / max m | (ii) n (worst) | (iii) D_phase / D_code / clock LEVEL m | consumer p50 tail (at 2 h) | verdict |
|---|---|---|---|---|---|
| PV-B | 1.79 / 3.28 / 7.08 | 13 (BOGT −3.5, GOPE +2.5, GAMB −2.1) | 0.95 / 0.13 / 0.74 | 1.19 (0.55) | control |
| c0 = pva2 (the 17 keys) | 1.19 / 3.22 / 6.67 | 13 (BSHM +4.3, MAC1 −4.2, KIT3 +1.9) | 0.96 / — / 0.06 | 2.26 (2.18) | exonerated |
| c2 `rx_code_bias_datum` | 1.95 / 3.28 / 7.01 | 13 | 0.95 / 0.13 / 0.73 | 1.20 (0.49) | exonerated |
| **c3 `apriori_clock_anchor_sigma_m` 0.02** | **1.08 / 1.97 / 2.86** | **7** (GOPE +1.4, GRAZ +1.1, BOGT −1.1) | 0.82 / 0.12 / **0.04** | 1.34 (0.74) | the only mover: (i) 0.60×, (ii) 0.54×, (iii) 0.86×; max 0.40×; level p99 below 25° 6.34 → 2.59 |
| c4 SH as pl2 | 1.99 / 3.38 / 8.81 | 14 | 0.94 / 0.13 / 0.74 | 1.32 (0.61) | exonerated |
| c7 tie cutoff 15° | 2.50 / 4.46 / 11.14 | 21 (LCK4 +5.7, GOPE +3.6, SEYG +3.4) | 0.91 / 0.14 / 0.83 | **6.84** (6.87) | regression |
| c7b tie cutoff 20° | 2.17 / 3.77 / 11.67 | 16 (GOPE +4.4) | 1.13 / 0.13 / 0.81 | 1.96 (1.19) | regression |

Availability identical (240/242) on every runnable arm.  **Grade:** no arm crosses or formally halves a named bar; c3 is the only mover
and carries the class's whole code-side movement (c3 alone 1.08 beats the union 1.19 — c2 and c4 push back); c1 unreadable.  §24.14's first
reading applies (c0 reproduces pva2 to Δ = 0).  **c7 / c7b refute §24.15's premise as a fix:** admitting the sub-25° LOS to the tie barely
moves their own tail (p99 6.34 → 5.58 m) and contaminates the healthy population (above 25°: p99 3.29 → 5.29, max 6.1 → 12.0, median
0.46 → 0.75), monotone in the cutoff on every read ((i) 1.79 / 2.17 / 2.50; (ii) 13 / 16 / 21; prires cb 3.12 / 3.28 / 4.91; consumer
1.19 / 1.96 / 6.84) — 25° stays; the untied low-elevation columns are not the tail's cause, tying them exports it.  **What c3 says:** the
FREE per-epoch satellite clock is a contributor to the code side — pinned to the a-priori CODE clock at 2 cm the residual tail collapses
(station max 7.1 → 2.9 m) and the unphysical ZWDs halve — but the wire's within-arc phase (0.82) and the consumer (1.34) do not follow, so
the consumer's remaining metre sits on the phase side (R6 reads it); and the pin is EXTERNAL data the self-contained producer cannot use.
Its self-contained counterpart is the Q3 temporal clock anchor the SIM golden arms (`clock_temporal_anchor` + its guard family, q 3e-6)
and the field golden does not (SPEC_clock_temporal_anchor: built default-OFF, every producer gate passed, G4 — the DOY-129 event window —
owed as invariant #1's blocker).

