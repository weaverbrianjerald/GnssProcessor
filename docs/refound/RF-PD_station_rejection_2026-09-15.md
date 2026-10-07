# RF-PD: the null behind the per-station `addStation` rejections, named (2026-09-15)

**STATUS**

- **id:** RF-PD
- **topic:** network-vehicle
- **status:** closed
- **summary:** Null NAMED, signal hypothesis REFUTED: every per-station PD rejection is one zero-eigenvalue null (the receiver-DCB / slant-iono / ambiguity confound) whose gauge row is installed after the test.
- **dates in the text:** 2026-09-15
- **knobs / posture:** `network.trace_station_pd_null` (bool, default OFF, trace-only, solve byte-identical); the fix pre-registered as `network.station_prior_before_pd` (section 6 (a), built in RF-PD-fix); RF-0g golden and RF-1 + RF-2b + RF-2d vehicle, field, 6 h
- **hashes named in the text:** exe `gnss_processor_rfpd.exe` sha256[:16] `F953562FD1132B4A` (main `9a779e8` + the knob); OFF smoke `products.txt` `9add1d3ab9d70657`
- **run dirs named in the text:** `runs/_rfpd_smoke_off`, `runs/_rfpd_rf0g_field6`, `runs/_rfpd_field6_both` (sources `runs/_rf0g_field6`, `runs/_rf2d_field6_both`); reader `scripts/rf_pd_null_read.py`
- **source:** [RF-NS-b](RF-NS-b_vehicle_obs_loss_2026-09-15.md) sections 2.3 and 4 (the drop and the vehicle's extra rejections)

**Outcome.** The card asks why `addStation` drops a settled median of 29 (golden) and 42 (vehicle) of 59 field stations per epoch. Only a phase-datum anchor pin in the station's own block closed the null (0 of 14 370 anchored station-epochs rejected). Fix (a), built in [RF-PD-fix](RF-PD-fix_station_prior_before_pd_2026-09-15.md), took PD rejections to 0 on all four beds but FAILED the consumer bars of section 6; (b) and (c) were conditional on (a) leaving rejections, so nothing is owed here. The knob joined the [RF-0hv](RF-0hv_working_posture_2026-09-15.md) working posture and, since 2026-09-22, the goldens ([GRB](GRB_golden_rebase_2026-09-22.md)).

**Status as recorded (2026-09-15):** open — **the drop is ONE structural null, it is the same null on every station and every epoch, and the datum
that would close it is installed one line too late.**  `NetworkNormalEquations::addStation` rejects a station whose local
block `N_rr_r` fails its relative PD test and the driver drops the station for the epoch.  Armed with the new
`network.trace_station_pd_null`, all **23 633** golden and **30 340** vehicle rejections now name their null direction, and
the answer does not vary: **exactly ONE eigenvalue, and it is zero to machine precision** (`n_eig = 1` on 100 % of rows in
both runs; `ev_min` ∈ [−9.8e-11, +1.4e-10] against `ev_max ≈ 5.8e6`, negative on 49.7 % — a coin flip) whose mass splits
**AMB 77.8 % / IONO 21.0 % / CBIAS 1.2 % / CLK 0.0 % / ZWD 0.0 %** — a spread with p50 0.779 and max 0.781 on the ambiguity
half, i.e. the SAME shape 54 k times.  That shape is the receiver-DCB ↔ slant-iono ↔ ambiguity confound in closed form: with
the receiver code-bias column present, a shift `η` of the DCB is cancelled by a common per-satellite iono shift `ε` and the
induced ambiguity shifts `b = ε` on L1 and `c = γε` on L2, so the predicted mass ratio `(b²+c²)/a² = 1 + γ² = 3.713`
(γ = (f₁/f₂)² = 1.6469) against the **measured 3.70** (0.778 / 0.210), and the receiver clock drops out exactly — which is
why `CLK = 0.0 %`.  The RXCB-DATUM prior that gauges that very column is built at
`ApplicationNetworkAssembly.cpp:4072-4078` and installed by `est.addStationPrior(...)` at **`:4605`** — *after*
`est.addStation(...)` at **`:4555`**, i.e. after the PD test that rejects the station for the missing datum.  The only thing
that closes the null today is an accident: a prioritized phase-datum ANCHOR pin (`foldAmb`, weight 1e10,
`ApplicationNetworkAssembly.cpp:2819-2860`) landing in that station's own block.  The correlation is **exact** — of 14 370
anchored station-epochs in the golden, **0** were rejected; all 23 633 rejections are anchorless station-epochs.
Nothing was fixed; the fix is pre-registered in §6.  Instrument: `scripts/rf_pd_null_read.py`.

## 1. The finding, re-graded

Re-counted directly from `logs/main_log*.log` (rotating — all rotations parsed, oldest first):

| run | rejections | consume epochs | per epoch (median, settled TOW ≥ 261660) | ignition median | min–max | distinct stations | rejected on ALL 720 |
|---|---|---|---|---|---|---|---|
| `_rfpd_rf0g_field6` (RF-0g golden) | **23 633** | 720 | **29 of 59** | 46 | 25–48 | 49 | **24** |
| `_rfpd_field6_both` (RF-1+2b+2d vehicle) | **30 340** | 720 | **42 of 59** | 46 | 37–48 | 49 | **36** |

Two corrections to the earlier readings.  **The always-rejected set is 24 stations in the golden and 36 in the vehicle, not
six** — RF-NS-b §2.3's "six" was the first six rows of a `Counter.most_common()` listing, all of which happen to be tied at
720.  And the whole-run median is 31 (golden) / 42 (vehicle), the SETTLED median 29 / 42; the run-wide figure is pulled up
by an ignition hour that drops 46 of 59.  The rate decays monotonically with time — golden hourly epoch-medians
**46 / 39 / 33 / 29 / 26 / 25** — which is the anchor roster filling, not the geometry improving (see §5).

The drop itself is unchanged: `ApplicationNetworkAssembly.cpp:4555-4577` catches the throw, logs a warn, sets
`present[i] = false` and leaves `est_index_of[i] = -1`, while `network_epoch_status.csv` still reports `n_present = 59`.
That is an uninstrumented amputation (standing invariant #4) whose only prior record was a warn line in a ROTATING log
naming a pivot MAGNITUDE and no parameter.

## 2. Step 0 — the signal hypothesis is REFUTED; anchor tenure is the exact predicate

The pre-registered hypothesis was that the always-rejected stations lack a signal the station-side datum assumes (e.g.
`rx_code_bias_datum` pinning C1C on a P-code-only receiver).  Read off the RINEX headers under `data/2024-129/obs/<sta>/`
against the golden's own rejection census:

| class | stations | GPS L1/L2 observables |
|---|---|---|
| always rejected, **no C1W** (11) | CAS1, CPVG, GOPE, HKSL, KRGG, LCK4, LMMF, MRO1, SAVO, WROC, WTZR (Trimble ALLOY / NetR9, Leica GR50) | `C1C C2W L1C L2W` |
| always rejected, **full C1W, no L1W** (9) | GOP6, GRAZ, HOLB, KIT3, MIZU, MRC1, PARK, PTBB, WTZS (Sept POLARX5/TR, AsteRx4) | `C1C C1W C2W L1C L2W` |
| always rejected, **full + L1W** (4) | POTS, ULAB, URUM, WIND (JAVAD TRE_3) | `C1C C1W C2W L1C L1W L2W` |
| NEVER rejected (11) | DARW, GCGO, JPLM, MBAR, NYA2, PALM, SGOC, STHL, SVTL, WTZZ, YKRO | 6 carry `L1W`, 5 do not; **all** carry `C1C C1W C2W` |

Lacking `C1W` is *sufficient* but not *necessary*: 11 of the 18 C1W-less stations are always-rejected and no never-rejected
station lacks C1W — but 13 C1W-carrying stations are also always-rejected, and 7 C1W-less stations are not.  The signal set
does not separate the classes, and neither does the receiver model (SEPT POLARX5 appears on both sides).  **Hypothesis
refuted.**

What IS exact is phase-datum anchor tenure.  Cross-tabulating `logs/network_phase_anchor.csv` (which station holds each
`(constellation, signal, satellite)` anchor that epoch) against the per-epoch rejection set over the golden's 60 rostered
stations × 720 epochs:

| golden / vehicle | rejected this epoch | not rejected |
|---|---|---|
| **holds ≥ 1 anchor row this epoch** | **0 / 0** | 14 370 / 9 500 |
| holds no anchor row this epoch | 23 633 / 30 340 | 5 197 / 3 360 |

**A station holding a phase-datum anchor is never rejected, and every rejection is an anchorless station-epoch — in BOTH
runs.**  Per station over the golden run: all 24 always-rejected stations have 0 anchor rows in the whole run, all 34
anchored stations are rejected on fewer than 720 epochs, and rejections fall monotonically with anchor tenure (MAJU 5 269
rows / 4 rejections; BOGT 2 886 / 62; POVE 127 / 619).  The two stations with 0 anchors that are not always-rejected are
MAC100AUS (719/720) and WTZZ00DEU, the colocated consumer hold-out that is not in the producer's station roster.

## 3. The instrument

`network.trace_station_pd_null` (bool, **default false**; `include/config/GnssConfigTypes.hpp:2333`, parsed at
`src/core/config/GnssConfigLoadNetwork.cpp:2325`, sink cross-check at `:2649`, manifest echo — armed-only, so an unarmed
run's manifest bytes are unchanged — at `src/core/logging/NetworkRunTraces.cpp:1539`).

- **Kernel half** (`src/core/filter/NetworkNormalEquations.cpp:476-508`, struct + accessors at
  `include/filter/NetworkNormalEquations.hpp:1182-1212`).  Inside the PD-failure branch and only when armed, one
  `Eigen::SelfAdjointEigenSolver` on the SAME `N_rr_r` the test rejected fills a `StationPdFailure` record: `ev_min`,
  `ev_max`, the count of `|λ| < 1e-12·ev_max`, the pivot triple `d_min / d_ref / d_max` the decision used, and the unit
  eigenvector of `ev_min` in LOCAL column order.  Deliberately NOT the LDLᵀ pivot index — `Eigen::LDLT` permutes
  (`transpositionsP`), so pivot *k* is not local column *k* and reading it as one names the wrong parameter.  Only a
  REJECTED block pays the O(n³); an accepted station never reaches the branch.  Setter forwarded through
  `NetworkEstimator::setStationPdNullTrace` (`include/filter/NetworkEstimator.hpp`), armed once at
  `src/core/app/ApplicationNetwork.cpp:1097` beside `setStationPdMedianScale`.
- **Naming half** (`src/core/app/ApplicationNetworkAssembly.cpp:4411-4512`, the `pdDropEvent` lambda, called from both
  rejection branches at `:4568` / `:4574`).  The kernel has no label vocabulary, so the caller inverts its OWN `h2local`
  map against `StateManager::getStateLabels()` — the same inverse `network.neq_export_epoch` uses
  (`NetworkProducerAr.cpp:428-443`) — and writes the top-5 |weight| columns in the estimability DIR-row style
  `FAMILY|SAT|SIG=weight`.
- **Event schema** — one `network_events.csv` row per rejection, `tier = TOPOLOGY`, `event = sta_pd_dropped`,
  `station`, `action` ∈ {`dropped`, `epoch_sunk`}, `statistic = ev_min`, `threshold = d_ref`, and `detail`:

  ```
  2313,259200.000,TOPOLOGY,sta_pd_dropped,,KRGG00ATF,-0.000000,15850.724483,dropped,
  evmin=-1.525165e-12;dmin=-3.676632e-10;dmax=3.491217e+06;evmax=5.803471e+06;neig=1;nlocal=24;
  fam=AMB:0.7779 CBIAS:0.0125 CLK:0.0000 IONO:0.2095 ZWD:0.0000;
  cols=AMB|G19|L2W=-0.2850 AMB|G17|L2W=-0.2850 AMB|G03|L2W=-0.2850 AMB|G04|L2W=-0.2850 AMB|G06|L2W=-0.2850
  ```

  `evmin` is repeated in `detail` because `emitEvent` formats every numeric column `{:.6f}`, so a 1e-10 eigenvalue prints as
  `0.000000` in `statistic` and its sign is lost — the headline number of the row would otherwise read "exactly zero" on
  every rejection.  `fam` carries the WHOLE eigenvector's per-family mass Σw² (it sums to 1); the top-5 is a thin slice of a
  null spread near-evenly over a whole band, so **the census is taken off `fam`, never off the top-5**.
- **Counters** (`include/app/NetworkRunContext.hpp`, `NetworkServiceCounters::pd_drop_*`): always accumulated (the
  increment costs no output), read out — armed-only, so an unarmed run's log text is byte-identical — at
  `src/core/filter/NetworkTeardown.cpp:369-396`, with an explicit INERT warn if there are rejections and zero named ones.
  No column was added to `network_epoch_status.csv` and `network_run_summary.csv`'s schema is untouched.
- **The rejection itself is unchanged.**  Nothing is admitted, nothing is pinned, no threshold moved.

### 3.1 Build and identity

Exe `build/x64-Release/gnss_processor_rfpd.exe` sha256[:16] `F953562FD1132B4A` (main `9a779e8` + this knob).  OFF smoke
`runs/_rfpd_smoke_off` (the `_rf2d_smoke_off` config, paths only): `products.txt` sha256[:16] **`9add1d3ab9d70657`** — the
canonical hash — and the file BYTE-IDENTICAL to `runs/_rf2d_smoke_off/products.txt`, with
`logs/closed_loop_trace.csv` `7476f0a064413ff4` and `logs/network_epoch_status.csv` `a227a6cd9e44e7f0` byte-identical too;
record stream (non-`#`, non-`S,` lines, LF-joined) `603191919e70a2a8`, 6 595 lines.  Whole-file identity subsumes any
sub-stream hash.  `"trace_station_pd_null"` is absent from the OFF run's manifest.  Debug `gnss_tests`: see §7.

Both ON runs are byte-identical to their source runs on all three gated artifacts — this instrument does not move the solve:

| ON run | source | `products.txt` | `closed_loop_trace.csv` | `network_epoch_status.csv` |
|---|---|---|---|---|
| `_rfpd_rf0g_field6` | `_rf0g_field6` | `dbc2713549edc41f` ✔ | `1776ad1939ebf5c7` ✔ | `c4b69cd2e5def6cc` ✔ |
| `_rfpd_field6_both` | `_rf2d_field6_both` | `1b800c79199d6b1b` ✔ | `c5c7abb73d347faa` ✔ | `45ce089e71a7d5e6` ✔ |

## 4. The census

### (1) Null-column family

Top-column counts are nearly useless here and the instrument says so: the top |weight| column is an `AMB|…|L2W` on
23 627 / 23 633 golden and 30 327 / 30 340 vehicle rows, but *which* L2W ambiguity rotates (§(2)), because the weights are
near-equal.  The mass census is the one that holds:

| family | golden mass | vehicle mass | what it is |
|---|---|---|---|
| `AMB` | **77.84 %** | **77.82 %** | per-(sat, signal) ambiguity — the L1 and L2 halves together |
| `IONO` | **21.00 %** | **21.02 %** | per-LOS slant ionosphere |
| `CBIAS` | **1.17 %** | **1.15 %** | the receiver code bias (ONE column) |
| `CLK` | 0.00 % | 0.00 % | receiver clock |
| `ZWD` | 0.00 % | 0.00 % | troposphere |
| other | 0.00 % | 0.01 % | unlabelled local columns |

Per-rejection spread of the AMB share: golden min 0.389, p50 **0.779**, max 0.781; vehicle min 0.394, p50 **0.779**,
max 0.781.  **It is one shape, 53 973 times.**

### (2) Per station — the column ROTATES, the SHAPE persists

Every station in both runs reads `mixed` on the consecutive-label test (consec 38–49 %) with 11–26 distinct top labels over
its rejections and no label above 28 % share (except the four stations with < 100 rejections).  Read naively that says
"birth/transient".  It is the opposite: the null is a fixed DIRECTION whose weights are near-equal across a whole band, so
which coefficient is largest is decided by fourth-digit noise and by which satellites are up.  The *shape* (§(1) mass split,
σ of the AMB share ≈ 0.001) persists on every station and every epoch, and 24 (golden) / 36 (vehicle) stations carry it on
all 720.  **A per-label persistence test is the wrong instrument for a near-degenerate eigenvector; the mass split is the
right one.**

### (3) The always-rejected stations

Golden, 24 of 49: CAS1, CPVG, GOP6, GOPE, GRAZ, HKSL, HOLB, KIT3, KRGG, LCK4, LMMF, MIZU, MRC1, MRO1, PARK, POTS, PTBB,
SAVO, ULAB, URUM, WIND, WROC, WTZR, WTZS.  Vehicle, 36 of 49: those 24 plus BOR1, COCO, DJIG, FFMJ, GAMB, LEIJ, NIUM, REYK,
SCTB, SOLO, SYOG, WARK.  Every one of them names an `AMB|Gxx|L2W` top column that rotates across 11–26 satellites, with the
§(1) mass split — and every one of them has **zero phase-datum anchor rows in the whole run** (§2).  Their RINEX signal sets
(§2) split 10 / 14 across the C1W boundary and explain nothing.

### (4) Time structure, and the vehicle's extra ~10/epoch

Golden hourly epoch-medians **46 / 39 / 33 / 29 / 26 / 25**; ignition (TOW < 261660) median 46, settled 29.  Vehicle
**46 / 43 / 42 / 42 / 40 / 40**; ignition 46, settled 42.  The golden decays by 21 stations over 6 h and the vehicle by 6.

The vehicle's extra is **not** a different family: its mass split is the golden's to within 0.02 pp, its `n_eig` is 1 on
100 % of rows, and its always-rejected set is the golden's 24 plus 12 more (BOR1, COCO, DJIG, FFMJ, GAMB, LEIJ, NIUM, REYK,
SCTB, SOLO, SYOG, WARK).  The §2 predicate is **exact in the vehicle too** — anchored-and-rejected is **0** there as well
(9 500 anchored station-epochs, 30 340 anchorless rejections) — and it says where the extra comes from: the vehicle spreads
its 47 937 anchor rows over **21** stations where the golden spreads 53 621 over **34**.  Under
`ambiguity_vehicle: float_kf` the phase datum is a live state that churns (RF-2 / RF-2b), so the election concentrates on
fewer hosts; the 13 stations it stops anchoring lose their accidental pin and join the rejected class.  **RF-NS-b §4's
inference — that the vehicle's extra drops are unpinned float-ambiguity columns with `amb_float_birth_var_m2` unset — is
half right, and the labels sharpen it**: the extra rejections carry the identical DCB-confound null, not a newborn-arc null.
A float birth prior would close them only because *any* prior on *any* one of those columns closes a rank-1 defect; the
defect is not about newborn arcs, and the vehicle's own contribution is that it hands out fewer accidental pins.

### (5) Multiplicity

`n_eig = 1` on **100.0 %** of rows in BOTH runs (23 633 / 23 633 and 30 340 / 30 340).  There is exactly one null per
rejected station, never several.  Local block dimension: min 12, p50 27, max 36 — always `3 + 3·S` (rx clock, ZWD, receiver
DCB, then per-satellite iono / L1 ambiguity / L2 ambiguity) or `2 + 3·S` where the station has no DCB column.

The eigenvalue is not "small", it is **zero to machine precision, with a random sign**: `ev_min` spans
−9.823e-11 … +1.133e-10 with p50 1.5e-13 on the golden (−9.823e-11 … +1.434e-10, p50 2.4e-14 on the vehicle) and is
negative on **49.7 % / 49.9 %** of rows — the coin-flip a numerically-zero eigenvalue produces.  The LDLᵀ pivot behaves the
same way (`d_min` negative on 50.5 % / 50.4 %, p50 −9.3e-12, range ±3e-9).  Against `ev_max ≈ 5.8e6` that is a relative
2e-17.  ⚠ **The rejection therefore does NOT hinge on the `d_min <= 0` half of the test**: the pin-aware reference `d_ref`
has p50 1.74e+04, so the relative bar `1e-12·d_ref` is 1.7e-8 — three orders above `|d_min|` — and the block fails on
magnitude whichever way the sign lands.  This is a structural null, not a conditioning nuisance, which is exactly the
distinction the median-scale test was built to make.

## 5. The mechanism, per family — one null, in closed form

There is only one family here, so this is one mechanism.  Write the station's local block for satellite *s* with rows
`C1, L1, C2, L2`, unknowns `{κ = rx clock, τ = ZWD, η = rx DCB}` global and `{a_s = slant iono, b_s = L1 amb, c_s = L2 amb}`
per satellite, with `γ = (f₁/f₂)² = 1.6469` and the DCB entering the code rows only, with coefficients `(d₁, d₂)`:

```
ΔC1 = κ + m·τ + a      + d₁η
ΔL1 = κ + m·τ − a + b
ΔC2 = κ + m·τ + γa     + d₂η
ΔL2 = κ + m·τ − γa + c
```

Setting all four to zero for every satellite gives `4S` equations in `3S + 3` unknowns — generically full rank — EXCEPT
along one direction.  `ΔC2 − ΔC1 = 0` gives `(γ−1)a + (d₂−d₁)η = 0`, so `a = −(d₂−d₁)η/(γ−1)`, the SAME for every satellite;
`ΔC1 = 0` then fixes `κ = −a − d₁η`, and the two phase rows give `b = a − κ`, `c = γa − κ`.  `τ` stays 0 because the wet
mapping `m` differs per satellite while `κ` does not, so the two are separable (measured `ZWD = 0.00 %`).  Under the IGS
DCB convention `d₂ = γd₁` this collapses to `a = −d₁η` and `κ = 0` exactly, leaving `b = a`, `c = γa` and the mass ratio

```
(b² + c²) / a² = 1 + γ² = 3.713      measured: 0.7784 / 0.2100 = 3.707   (golden)
                                               0.7782 / 0.2102 = 3.702   (vehicle)
```

and `κ ≈ 0` — the measured `CLK = 0.00 %`.  **The null is the receiver code bias, absorbed by the slant ionosphere and the
ambiguities.**  It is rank-1 by construction: one DCB column, one null.  Its ℓ² mass is small (1.2 %) only because that one
column is compensated by `3S` others; it is nonetheless the column whose presence creates the defect.

Two consequences follow, both with file:line:

1. **The datum exists and is installed too late.**  `network.rx_code_bias_datum: true` is armed on the field golden and its
   row — `λ = 1/σ² = 1/10² = 0.01`, target 0 in `x_lin` coordinates — is built at
   `src/core/app/ApplicationNetworkAssembly.cpp:4072-4078` into `R.carry_cols` / `carry_dNrr` / `carry_dur`, and installed
   by `est.addStationPrior(...)` at **`:4605`**.  `est.addStation(...)` — which runs the PD test — is at **`:4555`**.  The
   station is therefore judged WITHOUT the only row that gauges its receiver DCB, and thrown away for lacking it.  `λ = 0.01`
   against a median |pivot| of ~1.6e4 is a relative 6e-7, four orders above the 1e-12 bar, so the test would pass if the row
   were in the block.  The loader's own RXCB-DATUM text ("realized common-mode information is 1/sigma^2 … it must clear the
   per-epoch rank cutoff on its own") is describing exactly this arithmetic — for the SHARED gauge; nobody checked whether
   the row reaches the per-station gate.
2. **What actually saves the surviving stations is an accident.**  `foldAmb`
   (`src/core/app/ApplicationNetworkAssembly.cpp:2819-2860`) appends the prioritized phase-datum anchor's ambiguity pin as a
   ROW of `A_r` with weight `1e10`, so it IS inside `N_rr_r` when the test runs.  Fixing one `c_s` kills the one-parameter
   family.  That is why anchor tenure predicts survival EXACTLY (§2) and why the golden's rate decays hour by hour as the
   anchor roster fills, while the vehicle's does not (its phase datum churns).  The network's positive-definiteness is
   currently a side effect of the phase-datum election's geography.

⚠ Note for any future reader of the PD test: the message
(`src/core/filter/NetworkNormalEquations.cpp:509-514`) says "station prior too weak or an unobservable local parameter".
Both halves are true here and they are the same sentence — the unobservable parameter IS the one the missing prior would
have observed.

## 6. Pre-registered — NOT BUILT

### (a) FAMILY `CBIAS` — a DECLARATION: install the station's own datum rows BEFORE the gate judges it

The station can satisfy the RXCB datum; it is simply never offered it in time.  Move the diagonal station-prior install so
the prior rows are part of `N_rr_r` at the PD test — either by handing `addStation` the prior triple
(`carry_cols / carry_dNrr / carry_dur`) as an optional argument it folds before factorizing, or by a
`NetworkNormalEquations::addStationWithPrior` that composes them.  Knob `network.station_prior_before_pd` (bool, default
false).  ⚠ It must fold **only** the DATUM/prior rows, not the phase-leveling carry, or the carry's own staleness enters the
admission decision — that is invariant #7's decoupling edge and needs its own card.

**Both edges (invariant #7).**  DECOUPLING is now instrumented by this card (`sta_pd_dropped`, `n_sta_pd_dropped`).
RE-COUPLING owes a `TOPOLOGY sta_readmitted` event carrying the station, the prior that admitted it, and the epoch count it
was out, so a station's return is never read as a first sighting — the exact failure invariant #7 names.

### (b) FAMILY `AMB` / `IONO` — nothing of their own to fix

They carry 99 % of the mass and none of the cause: they are the compensating direction, not the defect.  A birth prior on a
float ambiguity (`network.amb_float_birth_var_m2`, unset on the vehicle arm) WOULD also close the rejection — any prior on
any column of a rank-1 defect does — but it would close it by pinning an ambiguity to cure a code-bias gauge, which is the
"fix the symptom in the wrong state" pattern this program keeps paying for.  Rank (b) below (a) and grade it only if (a)
leaves rejections behind.

### (c) The backstop, if (a) is not sufficient

Admit the station at reduced rank with the direction DECLARED (RF-NS-b §4 (b1) candidate 3), i.e. project the named null out
of the local block and emit `sta_pd_reduced_rank` with the column list this card already produces.  Last resort: it changes
what the station contributes, so it needs the same both-edge treatment plus a consumer grade.

### Bars

Products are **NOT** expected byte-identical — (a) is solve-affecting — so the bars are outcome bars, both arms, 6 h cold
field, `runs/_rf0g_field6` and `runs/_rf2d_field6_both` as controls:

1. `n_sta_pd_dropped` → **0** on the golden arm, with `network_epoch_status.csv`'s `n_present` telling the truth
   (a station counted present is a station in the solve).
2. Golden consumer p50 by hour no worse than **3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 m**, emitted ≥ **640/720**.
3. Vehicle consumer p50 by hour no worse than **4.75 / 3.00 / 0.84 / 0.65 / 1.37 / 0.60 m**, emitted ≥ **640/720**
   (it is 573/720 today; RF-NS-b's own bar of 645 stands, and the two cards must be graded together — half the withheld
   epochs are the `CB|Gxx|C1W` coverage RF-NS-b names, which is itself downstream of the drop rate).
4. OFF byte-identical on `products.txt`, `logs/closed_loop_trace.csv`, `logs/network_epoch_status.csv` — the gate this card
   already passes.
5. ⚠ The GOLDEN is in scope.  It drops 29 stations per settled epoch today, so (a) ON is a POSTURE CHANGE on the golden,
   not a regression check, and needs the sim arm (`golden_sim_refound`) graded beside it.

## 7. Runs, instruments, suite

- `runs/_rfpd_smoke_off` — OFF smoke, exit 0, byte-identical (§3.1).
- `runs/_rfpd_rf0g_field6` — RF-0g golden, knob ON, exit 0, 23 633 rejections named, solve byte-identical.
- `runs/_rfpd_field6_both` — RF-1 + RF-2b + RF-2d vehicle, knob ON, exit 0, 30 340 rejections named, solve byte-identical.
- `scripts/rf_pd_null_read.py <run_dir> [...] [--settled-tow T] [--top N] [--station STA]` — the five reads of §4 off
  `network_events.csv` alone; standard library only, no plotting.
- Debug `gnss_tests`: **2217 tests from 292 test suites ran. / [ PASSED ] 2216 tests. / [ SKIPPED ] 1 test** —
  `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the standing bootstrap-gated skip.
- No config under `config/golden/` was touched; the ledger was not touched; nothing was armed on a golden.
