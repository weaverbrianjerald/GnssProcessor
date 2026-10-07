# meas admission: the product-referenced admission screen (INC-1 to INC-3 spec)

**STATUS**

- **id:** meas admission
- **topic:** raim-qc
- **status:** closed
- **summary:** Spec of an absolute code screen against SP3 and CLK range (bound 1000 m) refusing corrupt channels at arc birth; birth, break and sweep gates PASS; `adm_screen` is ARMED on the goldens.
- **dates in the text:** 2026-08-01 to 2026-08-12
- **knobs / posture:** `network.adm_screen`, `network.adm_trace`, `network.adm_bound_m`, `network.adm_min_sats`, `network.adm_heal_ep`, `network.adm_max_frac`; INC-4: `network.adm_revet`, `network.adm_revet_gap_ep`, `network.adm_revet_ep`, `network.adm_revet_census`, `network.adm_revet_hold`. `adm_screen` and `adm_trace` are true in `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/ft63_24h_conv`
- **source:** `docs/archive/SPEC_measurement_admission.md` preamble and sections 1 to 8; build and gates in [meas admission impl](../raim-qc/MEAS-ADMISSION-IMPL.md) (section 9), the re-entry increment in [INC-4](../raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) (section 10)

**Outcome.** The question: how to refuse a channel whose code and phase share one offset of 100 to 200 km (BSHM00ISR G07 and G16 on DOY-129), invisible to every differential check by construction. The [implementation record](../raim-qc/MEAS-ADMISSION-IMPL.md) ran gates 2 to 4: both channels refused at birth, the era-2 SH withhold era gone (window peak |a00| 3380.6 to 36.9 TECU), the same reject set at 300 and 3000 m. Gates 5 to 7 are not recorded as graded. The section 8 re-vet deferral became [INC-4](../raim-qc/MEAS-ADMISSION-INC4-REENTRY.md); [AB2](../../refound/AB2_l5_third_frequency_2026-09-20.md) section 1 lists `adm_screen` as already armed.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_measurement_admission.md -->
# SPEC — product-referenced measurement admission (the corrupt-channel screen)

**Status**: SPEC'd 2026-08-01; built + field-gated 2026-08-01/02 (§9, §10).
**Ownership**: this card OWNS the measurement-admission question that the retraction of
`docs/SPEC_rinex_zero_sentinel.md §7` explicitly reopened.  That card stays retracted (its motivating
observation was a parser artifact; do not revive its parsing-level increments).  Its one sound idea —
census-as-observability — is realized here in the modality that works: **absolute,
product-referenced**, cross-checked against producer counters per the retraction's own instruction.
**Memory context**: `project_ft63_fast_tier` (era-2 end-to-end chain),
`docs/SPEC_prefit_census_observability.md` §17 gate 6 + its corrected verdict (the corrupt-channel
finding), `docs/SPEC_deweight_release_hygiene.md` §7, `docs/SPEC_flicker_gap_integrity.md` (the
demotion plumbing this card reuses).
**Scope discipline**: default-OFF, OFF structurally byte-identical, NOT fingerprinted (forward-only
admission policy — RAIM-class; a snapshot's stored state means the same thing either way).
**Provenance**: every `runs/<name>` path resolves under `D:\GnssProcessor_archive\2026-08-12\runs\`.

---

## 1. Problem — the class no differential screen can see

BSHM00ISR emits two corrupt channels on DOY-129: G07 C1C+L1C at **+198.2 km** and G16 at **+99.9 km**
versus SP3+CLK-predicted range (0.661 / 0.333 ms of light-time, ~2:1, drifting ~−4.8 m/s).
Triangulation: the same G07 reads +8.0 m at DJIG (products fine); eleven other BSHM satellites sit in
−12.5…+19.1 m (station clock and model fine); G21@BSHM — also C1C-only — reads −10.1 m (single-band
alone is innocent).  The receiver failure mode is not determinable from RINEX; the guard must work
from internal evidence.

Because the offset rides BOTH code and phase of the channel, it is invisible to every differential
check ever run against this data — TD smoothness, phase-minus-code, GF, MW all pass by construction.
The chain: the initializer faithfully seeds the corrupt value → on a single-band channel r ≈ 0, so
the error vanishes into the free slant state where no residual test can reach it (single-band = the
AMPLIFIER, not the cause) → the parked value waits for a topology valve (the 25° tie admission) →
+5.7k TECU discharge, 76-minute withhold era, 181 m never-healed scar.

Defense-in-depth after this card: **layer 0 (this card, the origin) → INC-3b admission-validate (the
valve) → prefit fuse (symptom bound) → SH gate (backstop)**, with the redundancy census as the
detection net.

## 2. The screen statistic — absolute, coarse, clock-free

Per (station, satellite, band) CODE observable, at every epoch where products cover the LOS:

    d_i = C_obs − ρ_pred(SP3, t_emit) − c·dtˢ(CLK)          [meters]
    z_i = d_i − med_station(d)        (median over the station's sats, n ≥ 4)

- Deliberately RAW: no tropo/iono/bias modelling.  Those terms live inside the healthy spread the
  threshold must tolerate (measured at BSHM: −12.5…+19.1 m across eleven sats, storm conditions
  included).  A structural fault detector, not a residual test.
- The station median IS the receiver clock estimate at this precision, so ms-scale clock offsets and
  steps (URUM/INEG take clean 1 ms steps in the leg window) cancel exactly — the same centring lesson
  the census card validated with its negative gate.
- ρ_pred and dtˢ are already computed per LOS for the measurement model; the screen is a few
  subtractions per LOS per epoch.  There is no cheaper place to kill this class.
- CODE only, by principle: a phase-only constant offset is indistinguishable from an ambiguity and is
  therefore gauge, not corruption.  Screening code flags the CHANNEL; the response covers both
  observables of that band.
- Threshold `adm_bound_m` (default 1000): ≥10× above the worst legitimate |z| (low-elevation storm
  slant + tropo residual + multipath + ephemeris error ≲ 100 m) and ≥100× below the observed corrupt
  class (10⁵ m).  A bound sweep belongs in validation (§7 gate 4), k-sweep style.

## 3. Design — three increments

### INC-1: admission screen at arc birth and warm-resume restore
`network.adm_screen` (bool, default false).  A (station, sat, band) whose z fails the bound at the
epoch its arc would be BORN — or, on a warm resume, at the first restored epoch (a restored corrupt
arc is NOT a birth; the census card's INC-3 structurally cannot see it, this card must) — is NOT
seeded: the band is withheld from the model set for that epoch via the signal-integrity demotion path
(the flicker card's band-removal plumbing, `NetworkSignalIntegrity.cpp` `obs.observed.erase` pattern
— reuse, do not duplicate).  No state exists yet, so nothing estimable is lost: pure admission, the
coverage-gate / anchor-hygiene hold-out precedent.

### INC-2: mid-arc onset + heal
Same knob (the screen runs every epoch it is armed; INC-2 is the response to an ESTABLISHED arc's z
crossing the bound):
- Onset: withhold the band's rows this epoch (demotion path), flag the arc for refloat
  (reset-over-repair — the channel's history is untrusted; count).
- Heal: after `adm_heal_ep` (int, default 5) consecutive in-bound epochs the band is readmitted with
  a FRESH seed through the normal funnel (counted).  A rejected band re-tests every epoch; a heal
  never restores the old states.
- Station-level guard: if more than `adm_max_frac` (double, default 0.5) of a station's screened
  bands fail in one epoch, the MEDIAN is untrusted — treat as a station event (hold all, distinct
  event/counter, mirror the IF-TD reference gate's incoherent-station precedent), do not reject
  channels individually off a corrupted centre.
- Band-loss policy interaction: withholding one band of a dual-band LOS lands in the existing
  (inconsistent — flicker INC-0) band-loss policy.  This card REUSES whatever that policy does today
  and does not fix it; say so in the run report when it triggers.

### INC-3: absolute-residual census (the missing scan modality, made permanent)
`network.adm_trace` (bool, default false; armable without the screen — measure-only).  Per epoch:
station-worst |z| and network-worst |z| with identity into `network_epoch_status.csv` (two columns +
the argmax cell into events above `adm_event_sigma`-equivalent bound, rate-limited like PREFIT).
This is the check whose absence let "raw RINEX verified clean" be honestly wrong for a month of
sessions.  It also gives era-1 its first absolute scan (§7 gate 5).

## 4. Wiring sites (verify at implementation)

- Screen + demotion: the station produce path where raw obs, ρ_pred and dtˢ coexist (the
  `computeMeasurementUpdate` chain — ModelGeometry / MeasurementModel); the implementing session
  locates the single site per the fuse-placement precedent.  Demotion via `NetworkSignalIntegrity`'s
  existing band-removal helpers.
- Refloat: the standard funnel (`carry_slipped_ambs` ∪ `StateManager::isAmbiguityPendingReseed` — the
  anchor-hygiene union precedent).
- Events: tier ADMISSION — `adm_reject_birth`, `adm_reject_resume`, `adm_onset`, `adm_heal`,
  `adm_station_event`, all with (station, sat, band, z, d, med, n_sats).
- Counters + run-end block: `NetworkServiceCounters` / `NetworkTeardown.cpp`; dead-wiring WARN when
  armed and zero LOS screened (invariant #4).
- Config: `NetworkConfig` fields + loader near the `adm`-adjacent recent blocks; arming info lines;
  `adm_screen` implies the census columns.
- Fingerprint: NOT appended; record the exemption in-place (`networkSnapshotFingerprint`),
  eq34/attrib precedent.
- CMake: manual entry if a new seam .cpp is created (prefer `filter/NetworkMeasurementAdmission.cpp`).

## 5. Knobs

| knob | type | default | effect |
|---|---|---|---|
| `network.adm_screen` | bool | false | INC-1/INC-2 screen + response |
| `network.adm_bound_m` | double | 1000 | |z| rejection bound |
| `network.adm_min_sats` | int | 4 | median validity floor (skip + count below) |
| `network.adm_heal_ep` | int | 5 | consecutive in-bound epochs to readmit |
| `network.adm_max_frac` | double | 0.5 | station-event guard on the median |
| `network.adm_trace` | bool | false | INC-3 census columns/events (measure-only) |
| `network.adm_revet` | bool | false | INC-4 re-entry vet; requires `adm_screen` (INERT-guard) |
| `network.adm_revet_gap_ep` | int | 1 | absent SCREENED epochs before a return is a re-entry |
| `network.adm_revet_ep` | int | 8 | vet-window length; coverage = return … return + this − 1 |
| `network.adm_revet_census` | bool | true | consult the previous epoch's PREFIT `obs_code` flags inside the window |
| `network.adm_revet_hold` | bool | false | strict posture: withhold a CLEAN re-entry until the heal path readmits |

No standard-set entry until §7 completes; ledger + posture row is the mechanism if promoted (per the
no-promotion directive).

## 6. Unit tests

New file `tests/test_measurement_admission.cpp` (one suite): (1) corrupt channel rejected at birth,
healthy siblings admitted; (2) a 1 ms station-common offset on every sat produces ZERO rejects
(median removal); (3) n < `adm_min_sats` skips and counts; (4) mid-arc onset withholds + flags
refloat; (5) heal readmits after N clean epochs with a fresh seed, never the old state; (6)
resume-restored corrupt arc rejected on the first restored epoch; (7) station event when > max_frac
fail — no individual rejects; (8) OFF structurally byte-identical.

## 7. Field validation — KNOWN-ANSWER gates (fixtures on disk)

Interaction discipline: all arms run with deweight-hygiene, fuse and redundancy-escalation knobs OFF
(census/topology traces may be armed — measure-only and already OFF-identity-proven).  Distinct run
dirs (`ft63_adm_*`); per-arm snapshot copies (the census card's §17 trap).

1. Smoke (10 ep) + OFF twin byte-identical.
2. **Birth gate** (leg from `snap_19`, offset 18.1125 — contains both births): `adm_reject_birth` for
   (BSHM00ISR, G07, C1C) at TOW 327030 with z ≈ +1.98e5 and (BSHM00ISR, G16, C1C) at 327210 with
   z ≈ +9.75e4; the two arcs never seed.  G21@BSHM (z ≈ −10 m) admits normally — the
   single-band-innocent control.
3. **The break gate + a free experiment** (leg through 19:26): with the two channels never admitted
   there is no parked cell and no 25°-admission discharge — expect NO SH engagement at 329160,
   a00 < 150.  Then read 329190: if the 521 TECU residual ALSO vanishes it was downstream of the same
   two channels (G16 is the natural suspect); if it persists it is independent and the `dwrat` hunt
   continues with one hypothesis retired.  Either outcome is decisive — record which.
4. **Bound sweep** `adm_bound_m` ∈ {300, 1000, 3000}: reject set identical (the two BSHM channels
   only) — the separation claim made falsifiable.
5. **Negative + era-1 gates** (24h with `adm_trace`, screen off): zero census events at the URUM/INEG
   clock-step epochs; the full-day absolute census enumerates every |z| > bound channel on the day —
   era-1's window finally gets an absolute scan (observational; no claim beyond what it shows).
6. QC flat vs control where the screen is armed (D5 ±few %, rx-CB withholds ~14–15%, σ̂₀ 1.00);
   refloat count ≈ onset count.
7. Product claims on total_IF only (promotion-ledger convention).

## 8. Deferral triage (pre-declared)

- RT / consumer-path port (broadcast-ephemeris ~m-scale error is fine at a km bound; the port is
  wiring, not design) — after producer validation.
- External attribution of BSHM's failure mode (station ops / receiver firmware) — diagnostics
  commentary only; the guard stands on internal evidence.
- The retracted card's per-arc re-vet increment — revive only if a class survives THIS screen.
  **REVIVED 2026-08-02 as INC-4 (§10).**  A class does survive, and it is not the one the retraction
  imagined: the screen's own admission RECORD outlives the ARC, so a channel that left the model set
  and came back inherited its pre-gap verdict and was screened as an ESTABLISHED cell.  On
  `runs/ft63_24h_conv`, **16 of the 17 mid-arc onsets were re-entries** — there were no genuine
  mid-arc onsets on the day at all.
- The band-loss policy inconsistency (flicker INC-0) — referenced, reused, not fixed here.
- Relationship to `SPEC_rinex_zero_sentinel.md`: sibling ingest-hygiene family (written-zero sentinels
  vs corrupt values); no shared code path.

---

