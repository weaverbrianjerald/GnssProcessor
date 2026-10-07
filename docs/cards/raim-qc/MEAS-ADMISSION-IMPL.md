# meas admission impl: the admission screen's implementation record

**STATUS**

- **id:** meas admission impl
- **topic:** raim-qc
- **status:** closed
- **summary:** Admission screen: gates 1 to 4 PASS (BSHM00ISR G07, G16 refused at birth; the 83-epoch era-2 withhold era gone); gate 1b cannot pass in this tree; 5 to 7 ungraded.
- **dates in the text:** 2026-07-31 → 2026-08-12
- **knobs / posture:** `network.adm_screen`, `network.adm_trace` (measure-only, solve-inert), `adm_bound_m` (1000 m), `adm_min_sats` (4), `adm_max_frac`; not fingerprinted; `network.write_run_manifest` (six conditional fields); `tests/test_measurement_admission.cpp` (12 cases after the review)
- **hashes named in the text** (commit / exe / byte-cert): `0458c09a` (`network_solution.csv` md5, census arm = OFF arm), `15629c86` (gate-1 smoke md5, unchanged by the unconditional refloat); exes `gnss_processor_adm.exe`, `gnss_processor_adm2.exe` (the fixed binary)
- **run dirs named in the text:** `ft63_adm_{off,census,screen}` (smoke), `runs/ft63_adm_birth`, `runs/ft63_adm_birthoff`, `runs/ft63_adm_break`, `runs/ft63_adm_breakoff`, `runs/ft63_leg19_dwrat` (need not be re-run)
- **source:** `docs/archive/SPEC_measurement_admission.md` §9, 9.0b, 9.1, 9.2, 9.3, 9.4, 9.5, 9.5b, 9.6, 9.7, 9.8, 9.9, 9.10, 9.11, 9.12, 9.13, 9.13b, 9.13c, 9.14, 9.15

**Outcome.** The build record for the spec's §7 gates; the statistic is observed minus the state-free modeled value. The review's two confirmed defects were fixed, and every field number reproduced on the fixed binary. Gate 2 hit both predicted births within 0.1 %, with nothing between the worst healthy |z| (54.3 m) and the corrupt class; gate 3 kept a00 at 32 to 37 TECU through the break and retired the `dwrat` hunt. Gate 1b (OFF byte-identity) fails on two divergences that predate the card. Continued in [INC-4](../raim-qc/MEAS-ADMISSION-INC4-REENTRY.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_measurement_admission.md -->
## 9. Implementation record (2026-08-01, Opus session)

Built as `filter/NetworkMeasurementAdmission.{hpp,cpp}` + `tests/test_measurement_admission.cpp`
(suite `MeasurementAdmission`, 10 cases).  Unit: **1087/1087 Debug cases pass** (1 pre-existing
fixture skip), plus `gnss_simulator_selftest` 35/35.  Deviations from the spec are recorded here
rather than in a comment nobody reads, per the eq34-hygiene §8 precedent.

### 9.0b The centre is a median over SATELLITES, not over observables

§2 writes `z_i = d_i − med_station(d)`, median "over the station's sats, n ≥ 4".  The first cut
medianed the **code observables** — a screen inversion, not a nicety.

A flat median over observables weights a satellite by its band count.  Three single-band satellites
(the exact BSHM shape — G07, G16, G21 and G17 are all C1C-only) against one four-band satellite puts
four of seven samples on ONE line of sight, so a corrupt four-band satellite **owns the centre**:
z ≈ 0, admitted, while its three healthy peers read z ≈ |d| and are each rejected, observables erased
and bias states pruned.  The station guard cannot save it — with one code per band the guard fires iff
healthy bands outnumber corrupt ones, the exact negation of the capture condition, so **in the regime
where capture happens the guard is provably silent.**

Reachable on the shipped network configs (`config/ppp_rtk_network_internal.json`,
`ppp_rtk_network_global_sh.json`: GPS+Galileo, `frequency_mode "all"`, `channel_mode "all"`, 25° mask)
at any station near the satellite floor — `present[i]` carries no minimum-satellite gate, and the
§9.13 leg's run-end reports 18 station-epochs under the floor on 121 epochs.  Arithmetically **inert**
on the GPS-only `triple`/`single` arms this card validated on (c ≤ 3 < `adm_min_sats` = 4 makes
capture impossible), **which is why no field arm would have caught it.**

Fixed by a two-level reduction: each satellite reduces to the median of its own code terms, and the
station centre is the median over those — so the median's sample size is the satellite count and
`adm_min_sats` bounds the population it names, exactly as §2 writes it.  Pinned by
`OneMultiBandSatelliteCannotCaptureTheStationCentre`.

### 9.1 The statistic — implemented as `observed − modeled`, not as a product re-query

§2's `d = C_obs − ρ_pred(SP3) − c·dtˢ(CLK)` is not implementable literally: **`PppObservation` carries
no satellite clock** — `SatGeometry` is a per-satellite function-local in `PppModel::computeModels`
(`PppModel.cpp:538`), destroyed each iteration, so `geom.sat_clock` survives only folded into
`modeled[]`, and re-querying `GnssCorrections::getSatelliteClockCorrection` would interpolate at a
*different* emission time than the model used.

Implemented as `d = observed[code] − modeled[code]`, with `modeled` = `computeBaseModeledValue`
(`PppModel.cpp:1466`) = `range − c·sat_clock − c·relativistic + ZHD·mf_d + antenna + bias + shapiro +
otl + apl + solid_tide` (+ pole tide + higher-order iono), i.e. **the spec's subtraction plus the
deterministic corrections** — a strictly *tighter* healthy spread, at zero cost and no new seam
dependency.

The load-bearing property is that `modeled` is **state-free**: no receiver clock, no wet-tropo state,
no first-order slant iono, no ambiguity.  That is what lets the screen fire at a cell's **birth**,
where a prefit innovation structurally cannot (a correctly self-seeded birth drives its own innovation
to ~0 whatever the observable says — why the prefit census card's §17 gate 6 could only find this class
after the fact).  Pinned by `StatisticAndBandIdentityAreWellDefined`; demonstrated end-to-end to nine
significant digits across four arms in §10.10.

### 9.2 Seam placement — AFTER `networkSignalIntegrity`, not before

§4 does not fix the order relative to the flicker card.  Implemented **after** it
(`ApplicationNetwork.cpp`, immediately following the `networkSignalIntegrity` call).  `modeled` is
rebuilt from `computeModels` every epoch, so screening *first* would make the classifier read every
withhold as a band **loss**, a `band_dropped_perm` after its flicker window, and a **FLICKER-REGAIN**
on heal — manufacturing precisely the events that card's INC-0 census exists to *measure*, and making
its counters incomparable between an admission-armed leg and its control.  The later slot costs
nothing (this seam reads only `observed` / `modeled`, which the classifier does not write) and the
ordering window is otherwise identical.  Both cards' refloats compose: the funnel is a set.

### 9.3 Wider orphan-state prune than §4 specifies

§4 names the phase-bias prune.  A **code** withhold orphans more, and all three consumers derive from
the STATES rather than the observables:

- `pruneSatPhaseBiasStatesExcept` — the anchor election (later in the epoch) could otherwise seat a
  datum pin on a row this seam removed;
- `pruneSatBiasStatesExcept` — the satellite OSB registration counts *stations holding a state*
  against its ≥2 coverage gate (`ApplicationNetworkAssembly.cpp:1634`), so an unpruned state
  phantom-supports a shared column only one station actually feeds: the single-station code column
  perfectly confounded with that receiver's clock;
- `pruneSatClockStatesExcept` — applied only when the withhold leaves the observation with **no
  observable at all**, because `registerClock` unions the clock states and a satellite that keeps
  another band is still a real supporter.

This mirrors the two-station observability gate's own "Prune the now-orphaned shared states"
(`ApplicationNetworkAssembly.cpp:1456`).  All three counted apart.  The smoke measured 11/11/11 —
G17@BSHM is C1C+L1C-only, so every withhold empties its observation.

### 9.4 Station-guard reading of "hold all"

§3's `(hold all, …, mirror the IF-TD reference gate's incoherent-station precedent)`, implemented as:
hold **every screened band of that station** for the epoch, emit one `adm_station_event`, and leave
**every per-channel admission record untouched** (no `admitted` flip, no heal progress) so the next
epoch re-tests from evidence.  That is the IF-TD gate's `FireNoRebase`
(`IfPhaseSlipDetector.cpp:454`) transposed: act at the family's scope, refuse the per-member claim.

**Added beyond the spec, and required by standing invariant #1:** the held arcs **are refloated**.
Erasing a station's observables punches a one-epoch hole; the invariant-DD chord is keyed
independently of the `StateManager` and would otherwise ride straight across it with a stale value.
The cost is a station-wide re-convergence, proportionate to the trigger (>50 % of a station's code
channels more than a kilometre from the products) and called out by a run-end WARN.  Per-channel
`adm_census` events are also suppressed during a station event — naming individual cells from a
centre we have just declared untrustworthy would be the claim the guard exists to refuse; the event's
`worst=` field carries the locator.

### 9.5 Refloat scope: UNCONDITIONAL, and the resume latch is per station

The first cut followed §3 literally — a **birth** rejection did not refloat ("no state exists yet …
pure admission"), while restore / onset / heal / station-guard rejections did.  **That was a defect**,
found by the §9.15 review and confirmed against the code:

"Is this a birth or a restore?" is answered from the seam's own record map, deliberately **not
serialized**.  On a resumed leg a station that is absent, thin, or whose satellite the two-station
observability gate erased on the epoch the latch was consumed has no entry — so its restored corrupt
arc presents as a *first sighting* at a later epoch, is labelled a birth, and has its band erased with
the Card-4 invariant-DD chord **left riding straight across the hole**: standing invariant #1
violated, on exactly the class of arc this card exists to catch.

Fixed by removing the condition rather than improving the classification: the refloat now runs on
**every** rejection.  At a true birth it costs nothing — the funnel insert erases a DD chord that does
not exist and a GM deviation that does not exist, and `markAmbiguityPendingReseed` on a
not-yet-allocated ambiguity is a set insert PASS B consumes when it allocates.  Measured: the gate-1
smoke's `network_solution.csv` is **byte-identical before and after the change** (md5 `15629c86…`).
Correctness must not ride a classification that can be wrong when the alternative is free.

The birth/restore **event names** are kept — diagnostics, and §7 gate 2 grades on them — so the latch
survives, correctly scoped: `adm_resume_pending` is a RUN flag (set at the warm-resume site, the
`anchor_age_resume_seed` shape) paired with `ctx.carry.adm_resume_seen`, the set of stations whose
first screened epoch has already happened.  Each station gets its own restore epoch whenever it first
appears.

### 9.5b The station guard counts SATELLITES, not bands

§3 writes "more than `adm_max_frac` of a station's screened **bands**".  Measured against a band
denominator the guard misfires: a single very bad satellite tracked on many bands (four of a
four-satellite station's seven channels is ONE corrupt Galileo line of sight) trips it, so the whole
station is held and three perfectly sound per-channel verdicts are refused.

The guard's question is "is the CENTRE trustworthy?", and the centre's population is **satellites** —
so that is its denominator: a station event fires when more than `adm_max_frac` of the station's
screened satellites carry a failing band.  That is what §3's own justification says ("the MEDIAN is
untrusted"); the "bands" wording does not survive contact with a multi-band constellation.  Both
counts are in the event detail (`fail_sats=1/4;frac=0.250;fail_bands=4/7`) so the reading is never
hidden.  Pinned by `OneMultiBandSatelliteCannotCaptureTheStationCentre`.

### 9.6 INC-3's two columns

§3 says "station-worst |z| and network-worst |z| … two columns", ambiguous on a file with one row per
*epoch*.  Implemented as `adm_sta_worst_z` = the **median over screened stations** of each station's
worst |z| (the healthy sea) and `adm_net_worst_z` = the **network maximum** (the extreme).  A single
maximum is an extreme, not a population, and §7 gate 4's separation claim needs both halves to be
falsifiable.  The argmax cell's identity goes to the events, exactly as §3 says.

`adm_census` events are emitted for **every** over-bound channel the response did not already name
this epoch (rate-limited per cell at a fixed 20-epoch holdoff — no knob, because the INC-1/INC-2
events are transitions and self-limiting).  §3's "the argmax cell" would not satisfy §7 gate 5's
"enumerates every |z| > bound channel on the day".

### 9.7 Byte-identity: the two census columns are CONDITIONAL

Unlike the redundancy and prefit censuses — whose unconditional columns changed every unarmed run's
`network_epoch_status.csv` (the deweight card's gate-1 record had to say so: "29 → 48 → 49 columns
over the session") — the two `adm_*` columns and the six manifest fields are emitted **only when the
card is armed** (`NetworkRunTraces::es_adm_columns_`, captured in `openAll`).  This card's OFF
contract is *structural* byte-identity and its own §7 gates are same-build A/B comparisons of these
very files; `goldens/decomp/obs.manifest.json` hashes both.

### 9.8 Not fingerprinted

Recorded in place at `networkSnapshotFingerprint` as the **fifth** instance of the policy-card
exemption (after eq34 hygiene, flicker, deweight release, and the observability families), plus a
banner in `GnssConfigTypes.hpp` and one in the loader.  Forward-only admission policy, RAIM-class:
arming it changes which *observables* enter the model set from here on and re-references no carried
quantity.  That is what makes every §7 gate — all of which replay from a shared `snap_19`/`snap_20`
seed with `state_snapshot_require: true` — legal at all.

### 9.9 Also landed

- `scripts/adm_make_arms.py` — the arm generator (smoke + leg families), with the §7 interaction
  discipline (no deweight-hygiene / fuse / redundancy-escalation knob on any arm) enforced in code.
- `pyviz` `_SHAPE_RULES`: `adm_heal` → `^`, `adm_*` → `v`.  Without them the whole family rendered as
  the default circle and a rejection was indistinguishable from a readmit.  The ADMISSION **tier
  colour** is deliberately left at the unknown-tier grey: extending `_TIERS` re-walks the
  distinguishable-colour list and would shift every existing tier's colour (18 tiers are already
  missing from it — a separate cleanup).
- `CLAUDE.md` test-suite figures refreshed (904/138/55 → 1087/172/71).  ⚠ **Dated record, 2026-08-01
  — not the current state.**  The suite is three re-counts on; CLAUDE.md records **1360 cases / 196
  suites / 89 files** (re-counted 2026-08-12).  The one-file-per-suite invariant was re-verified *at
  that time*; its last verification is at **174 suites, 2026-08-03**, and it has NOT been re-checked
  since — and the block-Baarda build record (now `docs/SPEC_redundancy_census.md` §12.7) adds a file
  with **two** suites, the known exception.  Same caveat applies to the 1088/1089 in §9.15 and the
  1098/1099 / 172 suites in §10.9.

### 9.10 Known gaps in this implementation

- **`sync_skip` epoch-status rows carry blank census columns.**  The two columns are stamped from
  `NetworkStashStatus` (the emit path) and from `NetworkSolveGlue`'s `emitEpochStatus` lambda (which
  covers `datum_failed` / `gate_withheld` / `solve_failed` / `sigma0_withheld` — the four post-screen
  QC dispositions).  The `sync_skip` sites in `ApplicationNetworkAssembly.cpp` run before the screen
  for that epoch, so blank is correct there — but the TOW-stamp guard means it is also *silent*
  rather than explicit.
- **`network.write_run_manifest`'s toggle dump is hand-enumerated**, and the whole prefit-census /
  redundancy / signal-integrity family is still absent from it.  This card added its own six fields;
  the manifest's "full toggle set" claim remains overstated for its neighbours.
- ~~`scripts/config_posture_check.py --verify-loader` is broken in this tree~~ — **FIXED SINCE; the
  tool works, trust its result.**  At the time it pointed at `GnssConfigLoadNetwork.cpp` for
  `logNetworkPosture_`, which had moved to `GnssConfigNetworkPosture.cpp` in the 2026-08-01 split.  It
  now reads `LOADER = .../src/core/config/GnssConfigNetworkPosture.cpp`
  (`config_posture_check.py:52-56`, with the split recorded in a comment) and anchors on
  `void GnssConfig::(appendNetworkPostureItems\w*_|logNetworkPosture_)` (`:273`).  No POSTURE row was
  added here, per the §5 no-promotion rule and the split rule in that file's banner.

### 9.11 §7 gate 1 — smoke, AS RUN

Build: same-build Release, renamed exe (`gnss_processor_adm.exe`) per the concurrent-session lock
rule.  Arms from `python scripts/adm_make_arms.py` (cold start, 10 epochs, 63 stations, DOY-129
00:00): `ft63_adm_off` (nothing armed), `ft63_adm_census` (`adm_trace` only), `ft63_adm_screen`
(`adm_screen` + `adm_trace`).

| check | verdict | evidence |
|---|---|---|
| knobs parse, arming + clamp lines print | **PASS** | both info lines and the solve-affecting WARN in `run.log`; the two sink-dependency warns fire when `trace_events` / `trace_epoch_status` are off |
| OFF twin schema unchanged | **PASS** | `ft63_adm_off/logs/network_epoch_status.csv` header ends at `pc_sh_n1k` — no `adm_*` columns; its manifest carries no `adm_*` field |
| **measure-only is solve-inert** | **PASS** | `census` vs `off`: `network_solution.csv` **md5-identical** (`0458c09a…`, 123 696 B) and `network_trace.csv` md5-identical.  This is the arm that makes the census legal to leave armed anywhere. |
| the screen acts | **PASS** | `screen` differs from `off` in `network_solution.csv` (as it must) |
| counters, invariant #4 | **PASS** | 11 screen passes / 693 station-epoch blocks / 6458 LOS-epochs / **15 935 CODE channels screened**, 0 thin skips, 0 non-finite skips |

**The smoke found a third corrupt BSHM channel, on a different satellite, at a different time of
day.**  At TOW 259200 (00:00:00) — eighteen hours before the G07/G16 births §1 is written around —
`BSHM00ISR` reports **G17 C1C at z = +165 499 m** (d = +124 788.5 m against a station median of
−40 710.8 m over 10 satellites), drifting −0.82 m/s over the ten epochs.  Like G07 it is a
**C1C+L1C-only** line of sight; the withhold removed both observables.  Against it, the healthy sea in
the same rows: `adm_sta_worst_z` = **9.1–9.9 m** across all ten epochs — a **17 000× separation**, the
§2 threshold claim measured, two decades clear on each side of the 1000 m bound.

Run-end (screen arm), verbatim: 1482 birth screenings → 1481 admitted, **1 rejected at birth**, 0
resume rejections, 0 onsets, 0 heals, 10 channel-epochs held while already rejected, **0 station
events**; 11 channel-epochs withheld removing 22 observables and pruning 11 phase-bias / 11 code-bias
/ 11 satellite-clock states; 0 funnel insertions (correct — every withhold was a BIRTH, which by
design has nothing to refloat, and the dead-wiring WARN correctly stayed silent because its trigger is
`onsets + reject_resume + station_events > 0`); 2 events emitted, 9 suppressed by the holdoff.

```
2313,259200.000,ADMISSION,adm_reject_birth,G17,BSHM00ISR,165499.308846,1000.000000,
  withhold,band=1;d=124788.500;med=-40710.808;n_sats=10
2313,259230.000,ADMISSION,adm_census,G17,BSHM00ISR,165252.472049,1000.000000,
  held,band=1;d=124541.320;med=-40711.152;n_sats=10
```

i.e. the channel was refused at the epoch its arc would have been born, and the census then carried
its identity through the hold at the rate limit — the INC-1 + INC-3 contract.

**Caveat stated plainly:** a 10-epoch cold smoke at a window containing neither of §1's named
channels.  It establishes the mechanism, the separation and the OFF/measure-only identity, and
nothing about §7 gates 2–7, which need the leg replays.

### 9.12 §7 gate 1b — the OFF byte-identity gate CANNOT PASS in this tree

The `goldens/decomp` matrix is the repo's real OFF-identity harness and its `obs` arm exercises
exactly the writers this card touched (`network_epoch_status.csv`, `network_events.csv`,
`network_run_manifest.json`).  Run against `goldens/decomp/obs.manifest.json` with a same-build
Release exe, it **FAILS — on two divergences that both predate this card**, and per the harness's own
rule ("never regenerate goldens or masks to make it pass") nothing was regenerated:

1. `run.log:24` — `Elevation power: code=1.00, phase=0.50` (golden) vs `phase=1.00` (now).  That is
   the `phase_elevation_power` default flip landed 2026-07-31 by a different workstream.  It changes
   the weighting, hence σ̂₀, hence every solve-derived artifact, so the comparison halts before
   reaching any of them.
2. The golden's `network_epoch_status.csv` header is **17 columns**; the current unarmed run writes
   **48**.  The 31 added columns are the P1-1/P1-2/P1-4 frame guards, the redundancy census and the
   prefit census.  The manifest has likewise gained 14 network keys (`fast_clock_*`,
   `state_snapshot_*`, `process_noise`).

The structural half was extracted by hand — and it is the stronger statement:

| | golden (pre-change) | this build, card OFF |
|---|---|---|
| `network_epoch_status.csv` columns | 17 | 48 — **none of them `adm_*`** |
| manifest `network` keys added since golden | — | 14 — **none of them `adm_*`** |

i.e. of the 45 fields other observability cards have added to the *unarmed* artifact since that
baseline, this card added **zero**.  Re-baselining the decomp goldens for the phase-power flip is owed
by whoever landed it; it is a prerequisite for any future OFF-identity gate in this tree and is NOT
this card's to do.

### 9.13 §7 gate 2 — the KNOWN-ANSWER birth gate, AS RUN: **PASS**

`runs/ft63_adm_birth` (armed) and `runs/ft63_adm_birthoff` (same-build control), both from `snap_19` +
`start_offset_hours 18.1125`, `state_snapshot_require: true`, 121 epochs, 63 stations,
`gnss_processor_adm.exe`.  Graded by `python scripts/adm_gate_verdict.py --gate birth`.

```
2313,327030.000,ADMISSION,adm_reject_birth,G07,BSHM00ISR,198170.133294,1000.000000,
  withhold,band=1;d=157468.210;med=-40701.923;n_sats=11
2313,327210.000,ADMISSION,adm_reject_birth,G16,BSHM00ISR, 97536.690175,1000.000000,
  withhold,band=1;d= 56834.126;med=-40702.565;n_sats=12
```

| §7.2 requirement | verdict | measured |
|---|---|---|
| `adm_reject_birth` (BSHM00ISR, G07, C1C) at TOW 327030, z ≈ +1.98e5 | **PASS** | 327030.000, z = **198 170** (0.1 % off) |
| `adm_reject_birth` (BSHM00ISR, G16, C1C) at TOW 327210, z ≈ +9.75e4 | **PASS** | 327210.000, z = **97 537** (0.0 % off) |
| the two arcs never seed | **PASS** | both `withhold`; 62 channel-epochs held over the leg, 124 observables removed |
| G21@BSHM admits normally (the single-band-innocent control) | **PASS** | **0** rejections |
| no station event (one bad channel in ~14 is far under `adm_max_frac`) | **PASS** | 0 |

Both predictions were derived independently, from the products, in a different session, months of
investigation apart from this code.  The screen reproduced them to within a tenth of a percent on its
first armed leg.

Run-end, verbatim: 121 screen passes / 7605 station-epoch blocks / 68 707 LOS-epochs / **173 591 CODE
channels screened**; 18 station-epochs skipped below the 4-satellite median floor, 0 non-finite; **62
channel-epochs over the bound**, run max |z| 198 170 at `BSHM00ISR|G07|1`; 1738 birth screenings →
1736 admitted, **2 rejected at birth**, 0 resume rejections, 0 onsets, 0 heals, 60 held; 62 withheld
removing 124 observables and pruning 62 phase-bias / 62 code-bias / 62 satellite-clock states; **62
funnel insertions** (0 pending-reseeds — this lineage runs `phase_leveling` off, and the two feeds are
counted apart so that reads as "one feed is not armed" rather than "nothing happened"); 6 events
emitted, 56 held by the rate limit.  The warm-resume RESTORE rule ran on the leg's first pass and
rejected nothing, which is right: G07's arc is born at 18:50:30, forty-four minutes into the leg.

**The §2 threshold claim, measured — a four-decade gap with nothing in it.**  The INC-3 census columns
over all 121 epochs (63 stations, 173 591 screened channels), split at the epoch the first corrupt arc
is born:

| | epochs | worst network |z| |
|---|---|---|
| before TOW 327030 (no corrupt channel live) | 87 | **54.3 m** |
| from TOW 327030 (corrupt channels live) | 34 | max **198 170 m**, per-epoch max never below **194 621 m** |

The healthy sea — `adm_sta_worst_z` — sits at **8.5–10.6 m** across every one of the 121 epochs, storm
day included.  So `adm_bound_m = 1000` sits **18× above the worst legitimate |z| anywhere in the
network** and **195× below the corrupt class**, with an empty band of nearly four decades between
54 m and 195 km.  §2 estimated "≥10× above … ≥100× below" from a single station's eleven satellites;
both hold, network-wide, with margin.  That also *predicts* §7 gate 4: any bound in (54 m, 194 621 m)
must produce the identical reject set, so the sweep tests the **implementation**, not the physics.

### 9.13b §7 gate 3 — THE BREAK GATE: **PASS**, and it closes the `dwrat` hunt

`runs/ft63_adm_break` (armed) vs `runs/ft63_adm_breakoff` (same build, nothing armed), both from
`snap_20` + `start_offset_hours 19.1125`, `state_snapshot_require: true`, 121 epochs through the era-2
event.  `a00_tecu` from `network_vtec_sh_health.csv`, with the epoch-status disposition:

| TOW | OFF a00 | ON a00 | OFF disposition | ON disposition |
|---|---|---|---|---|
| 329100 | 35.93 | 35.93 | emitted | emitted |
| 329130 | 35.73 | 35.73 | emitted | emitted |
| **329160** | **3380.57** | **35.30** | **sh_gate_withheld** | **emitted** |
| 329190 | 3040.03 | 35.35 | sh_gate_withheld | emitted |
| 329220 | 2722.59 | 35.37 | sh_gate_withheld | emitted |
| 329400 | 2453.49 | 35.44 | sh_gate_withheld | emitted |
| 329490 | 3072.01 | 36.07 | sh_gate_withheld | emitted |
| 330000 | 1510.93 | 35.80 | sh_gate_withheld | emitted |
| 331500 | 544.78 | 32.31 | sh_gate_withheld | emitted |

Window peak |a00|: **3380.6 → 36.9 (92×)**.  Dispositions over the leg: OFF **83 `sh_gate_withheld` /
38 emitted** (the documented multi-hour era, reproduced); ON **121 emitted, 0 withheld**.  §7.3 asked
for "NO SH engagement at 329160, a00 < 150".  Measured: **no SH engagement at ANY epoch of the leg**,
and the armed arm never leaves the 32–37 TECU band — it does not notice that an event happened.

Response telemetry: 121 screen passes, 173 287 CODE channels screened, 110 channel-epochs withheld
removing 220 observables, 110 phase-bias / 110 code-bias / 109 satellite-clock states pruned, and
**110 funnel insertions** — the unconditional refloat of §9.5 doing its job.  `pending_reseed` is 0
because this lineage runs `phase_leveling` off; the two feeds are counted apart precisely so that
reads as "one feed is not armed".

**The free experiment of §7.3 resolved, and it retires an open hunt.  The 521 TECU residual
VANISHES.**  329190 reads **35.35** on the armed arm.  The residual excursion that survived
`SPEC_deweight_release_hygiene`'s INC-3b — the one that defeated all three of that card's
falsification hypotheses (k-sweep flat, release-timing, row-starvation) and left its §7.3 marked
"era-2 is NOT closed" — was **downstream of the same two BSHM channels**.  Killing them at admission
removes both the primary discharge and the residual.  `runs/ft63_leg19_dwrat` need not be re-run.

*Magnitude caveat, stated rather than glossed*: this OFF control reads a00 = 3380 at the break where
the historical record says 5738.6.  The arms derive from `_ft63_leg19_base.json` with the §7
interaction knobs stripped, and the tree also carries another workstream's `phase_elevation_power`
0.5 → 1.0 default flip, which changes the weighting and hence the magnitude.  The **event** is
unmistakably the same one — engage at exactly TOW 329160, multi-hour withhold era, slow decay — but do
not compare 3380 against 5738.6 as though the two runs were the same configuration.

### 9.13c §7 gate 4 — the bound sweep: **PASS**

`adm_bound_m` ∈ {300, 1000, 3000} on the gate-2 leg.  The rejected channel set is **identical at all
three** — (BSHM00ISR, G07, band 1) and (BSHM00ISR, G16, band 1) — i.e. unchanged across a decade
either side of the default: §2's separation claim made falsifiable and not falsified, exactly as
§9.13's measured gap predicts.  Gate 4's `b1k` arm is the same run as `birth`; the sweep is graded
`b300` / `birth` / `b3k`.

### 9.14 §7 gates 5–7 — status at hand-off

- **Gate 5 (negative + era-1, 24 h with `adm_trace`, screen off)**: not run (~80 min).  The 10-epoch
  smoke already previewed what it is for — it found a **third** corrupt BSHM channel, G17 C1C at
  +165.5 km at TOW 259200, eighteen hours away from the two this card was written around.  The
  full-day census is the arm that would enumerate the rest.
- **Gates 6 (QC flat) and 7 (total_IF product claims)**: not graded.  Both want a run pair the harness
  parses for D5 rates / rx-CB withhold % / σ̂₀, which §17 of the prefit card records as not being
  emitted in a form that harness reads.

### 9.15 Adversarial review record (2026-08-01)

Five-dimension review (seam correctness, standing invariants #1/#4 + state orphaning, OFF
byte-identity, config/resume/fingerprint, traces/events/test adequacy), each finding handed to an
independent refuter instructed to default to REFUTED unless it could trace a concrete path from real
inputs to a wrong output.  **10 findings raised, 8 refuted, 2 confirmed — both fixed**, and both are
recorded where they belong: the station centre was a median over OBSERVABLES rather than satellites
(**§9.0b** — a screen inversion, live on the shipped GPS+Galileo `all`/`all` configs and
arithmetically inert on the GPS-only arms this card validated on, which is why no field gate would
have found it), and the warm-resume RESTORE latch was a single global one-shot (**§9.5** — standing
invariant #1).  The second fix subsumes the first half of the problem by removing the dependency
rather than improving the classifier: the refloat is now unconditional, and the per-station latch
survives only as an event LABEL.  A third change fell out of writing the test for #1 — the station
guard's denominator (§9.5b).

Two unit cases were added for the two defects
(`OneMultiBandSatelliteCannotCaptureTheStationCentre`,
`ResumeRestoreLabelIsPerStationNotAGlobalOneShot`), and
`CorruptChannelRejectedAtBirthWhileHealthySiblingsAdmit` flipped its `refloat_ambs == 0` assertion to
`> 0`.  Suite: **12 cases**, full tree **1088/1089** at the time (1 pre-existing fixture skip; see
§9.9's caveat).

**Every field number in §9.11–§9.13c was re-measured on the FIXED binary**
(`gnss_processor_adm2.exe`, all four leg arms and the smoke trio re-run from fresh snapshot copies).
Every verdict is unchanged and every z value reproduces to the digit — as predicted: fix 1 is
arithmetically inert on GPS-only `triple`/`single` data, fix 3 never had an opportunity (0 station
events on any arm), and fix 2 adds only funnel insertions on epochs already being withheld.  The
gate-1 smoke came back **md5-identical** to the pre-fix run, which is the "refloating a birth is a
no-op" claim demonstrated rather than asserted.  The one visible difference is the intended one:
`refloat_ambs` went 0 → 62 (birth leg) and 0 → 110 (break leg).

---

