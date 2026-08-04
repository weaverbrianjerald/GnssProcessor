# SPEC — product-referenced measurement admission (the corrupt-channel screen)

**Status**: SPEC'd 2026-08-01 (Fable session). Assigned: Opus.
**Ownership**: this card OWNS the measurement-admission question that the
retraction of `docs/SPEC_ingest_structural_guard.md` explicitly reopened. That
card stays retracted (its motivating observation was a parser artifact; do not
revive its parsing-level increments). Its one sound idea —
census-as-observability — is realized here in the modality that actually works:
**absolute, product-referenced**, cross-checked against producer counters per
the retraction's own instruction.
**Memory context**: `project_ft63_fast_tier` (era-2 end-to-end chain),
`docs/SPEC_prefit_census_observability.md` §17 gate 6 + its corrected verdict
(the corrupt-channel finding), `docs/SPEC_deweight_release_hygiene.md` §7,
`docs/SPEC_flicker_gap_integrity.md` (the demotion plumbing this card reuses).
**Scope discipline**: default-OFF, OFF structurally byte-identical, NOT
fingerprinted (forward-only admission policy — RAIM-class; a snapshot's stored
state means the same thing either way).

---

## 1. Problem — the class no differential screen can see

BSHM00ISR emits two corrupt channels on DOY-129: G07 C1C+L1C at **+198.2 km**
and G16 at **+99.9 km** versus SP3+CLK-predicted range (0.661 / 0.333 ms of
light-time, ~2:1, drifting ~−4.8 m/s). Triangulation: the same G07 reads
+8.0 m at DJIG (products fine); eleven other BSHM satellites sit in
−12.5…+19.1 m (station clock and model fine); G21@BSHM — also C1C-only —
reads −10.1 m (single-band alone is innocent). The receiver failure mode is
not determinable from RINEX; the guard must work from internal evidence.

Because the offset rides BOTH code and phase of the channel, it is invisible
to every differential check ever run against this data — TD smoothness,
phase-minus-code, GF, MW all pass by construction. The chain it feeds is now
fully established: the initializer faithfully seeds the corrupt value → on a
single-band channel r ≈ 0, so the error vanishes into the free slant state
where no residual test can reach it (single-band = the AMPLIFIER, not the
cause) → the parked value waits for a topology valve (the 25° tie admission)
→ +5.7k TECU discharge, 76-minute withhold era, 181 m never-healed scar.

Defense-in-depth after this card: **layer 0 (this card, the origin) →
INC-3b admission-validate (the valve) → prefit fuse (symptom bound) →
SH gate (backstop)**, with the redundancy census as the detection net.

## 2. The screen statistic — absolute, coarse, clock-free

Per (station, satellite, band) CODE observable, at every epoch where products
cover the LOS:

    d_i = C_obs − ρ_pred(SP3, t_emit) − c·dtˢ(CLK)          [meters]
    z_i = d_i − med_station(d)        (median over the station's sats, n ≥ 4)

- Deliberately RAW: no tropo/iono/bias modelling. Those terms live inside the
  healthy spread the threshold must tolerate (measured at BSHM: −12.5…+19.1 m
  across eleven sats, storm conditions included). The screen is a structural
  fault detector, not a residual test.
- The station median IS the receiver clock estimate at this precision, so
  ms-scale clock offsets and steps (URUM/INEG take clean 1 ms steps in the
  leg window) cancel exactly — the same centering lesson the census card
  validated with its negative gate.
- ρ_pred and dtˢ are already computed per LOS for the measurement model; the
  screen is a few subtractions per LOS per epoch. There is no cheaper place
  to kill this class.
- CODE only, by principle: a phase-only constant offset is indistinguishable
  from an ambiguity and is therefore gauge, not corruption. Screening code
  flags the CHANNEL; the response covers both observables of that band.
- Threshold `adm_bound_m` (default 1000): ≥10× above the worst legitimate |z|
  (low-elevation storm slant + tropo residual + multipath + ephemeris error
  ≲ 100 m) and ≥100× below the observed corrupt class (10⁵ m). A bound sweep
  belongs in validation (§7 gate 4), k-sweep style.

## 3. Design — three increments

### INC-1: admission screen at arc birth and warm-resume restore
`network.adm_screen` (bool, default false). A (station, sat, band) whose z
fails the bound at the epoch its arc would be BORN — or, on a warm resume, at
the first restored epoch (a restored corrupt arc is NOT a birth; the census
card's INC-3 structurally cannot see it, this card must) — is NOT seeded:
the band is withheld from the model set for that epoch via the
signal-integrity demotion path (the flicker card's band-removal plumbing,
`NetworkSignalIntegrity.cpp` `obs.observed.erase` pattern — reuse, do not
duplicate). No state exists yet, so nothing estimable is lost: this is pure
admission, the coverage-gate / anchor-hygiene hold-out precedent.

### INC-2: mid-arc onset + heal
Same knob (the screen runs every epoch it is armed; INC-2 is the response to
an ESTABLISHED arc's z crossing the bound):
- Onset: withhold the band's rows this epoch (demotion path), flag the arc for
  refloat (reset-over-repair — the channel's history is untrusted; count).
- Heal: after `adm_heal_ep` (int, default 5) consecutive in-bound epochs the
  band is readmitted with a FRESH seed through the normal funnel (counted).
  A rejected band re-tests every epoch; a heal never restores the old states.
- Station-level guard: if more than `adm_max_frac` (double, default 0.5) of a
  station's screened bands fail in one epoch, the MEDIAN is untrusted — treat
  as a station event (hold all, distinct event/counter, mirror the IF-TD
  reference gate's incoherent-station precedent), do not reject channels
  individually off a corrupted center.
- Band-loss policy interaction: withholding one band of a dual-band LOS lands
  in the existing (inconsistent — flicker INC-0) band-loss policy. This card
  REUSES whatever that policy does today and does not fix it; say so in the
  run report when it triggers.

### INC-3: absolute-residual census (the missing scan modality, made permanent)
`network.adm_trace` (bool, default false; armable without the screen —
measure-only). Per epoch: station-worst |z| and network-worst |z| with
identity into `network_epoch_status.csv` (two columns + the argmax cell into
events above `adm_event_sigma`-equivalent bound, rate-limited like PREFIT).
This is the check whose absence let "raw RINEX verified clean" be honestly
wrong for a month of sessions. It also gives era-1 its first absolute scan
(§7 gate 5).

## 4. Wiring sites (verify at implementation)

- Screen + demotion: the station produce path where raw obs, ρ_pred, and dtˢ
  coexist (the `computeMeasurementUpdate` chain — ModelGeometry /
  MeasurementModel); the implementing session locates the single site per the
  fuse-placement precedent. Demotion via `NetworkSignalIntegrity`'s existing
  band-removal helpers.
- Refloat: the standard funnel (`carry_slipped_ambs` ∪
  `StateManager::isAmbiguityPendingReseed` — the anchor-hygiene union
  precedent).
- Events: tier ADMISSION — `adm_reject_birth`, `adm_reject_resume`,
  `adm_onset`, `adm_heal`, `adm_station_event`, all with
  (station, sat, band, z, d, med, n_sats).
- Counters + run-end block: `NetworkServiceCounters` / `NetworkTeardown.cpp`;
  dead-wiring WARN when armed and zero LOS screened (invariant #4).
- Config: `NetworkConfig` fields + loader near the `adm`-adjacent recent
  blocks; arming info lines; `adm_screen` implies the census columns.
- Fingerprint: NOT appended; record the exemption in-place
  (`networkSnapshotFingerprint`), eq34/attrib precedent.
- CMake: manual entry if a new seam .cpp is created (prefer a small
  `filter/NetworkMeasurementAdmission.cpp`).

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

No standard-set entry until §7 completes; ledger + posture row is the
mechanism if promoted (per the no-promotion directive).

## 6. Unit tests

New file `tests/test_measurement_admission.cpp` (one suite): (1) corrupt
channel rejected at birth, healthy siblings admitted; (2) a 1 ms
station-common offset on every sat produces ZERO rejects (median removal);
(3) n < `adm_min_sats` skips and counts; (4) mid-arc onset withholds + flags
refloat; (5) heal readmits after N clean epochs with a fresh seed, never the
old state; (6) resume-restored corrupt arc rejected on the first restored
epoch; (7) station event when > max_frac fail — no individual rejects; 
(8) OFF structurally byte-identical.

## 7. Field validation — KNOWN-ANSWER gates (fixtures on disk)

Interaction discipline: all arms run with deweight-hygiene, fuse, and
redundancy-escalation knobs OFF (census/topology traces may be armed — they
are measure-only and already OFF-identity-proven). Distinct run dirs
(`ft63_adm_*`); per-arm snapshot copies (the census card's §17 trap).

1. Smoke (10 ep) + OFF twin byte-identical.
2. **Birth gate** (leg from `snap_19`, offset 18.1125 — contains both births):
   `adm_reject_birth` for (BSHM00ISR, G07, C1C) at TOW 327030 with
   z ≈ +1.98e5 and (BSHM00ISR, G16, C1C) at 327210 with z ≈ +9.75e4; the two
   arcs never seed. G21@BSHM (z ≈ −10 m) admits normally — the
   single-band-innocent control.
3. **The break gate + a free experiment** (leg through 19:26): with the two
   channels never admitted there is no parked cell and no 25°-admission
   discharge — expect NO SH engagement at 329160, a00 < 150. Then read
   329190: if the 521 TECU residual ALSO vanishes, it was downstream of the
   same two channels (G16 is the natural suspect); if it persists, it is
   independent and the `dwrat` hunt continues with one hypothesis retired.
   Either outcome is decisive — record which.
4. **Bound sweep** `adm_bound_m` ∈ {300, 1000, 3000}: reject set identical
   (the two BSHM channels only) — the separation claim made falsifiable.
5. **Negative + era-1 gates** (24h with `adm_trace`, screen off): zero
   census events at the URUM/INEG clock-step epochs; the full-day absolute
   census enumerates every |z| > bound channel on the day — era-1's window
   finally gets an absolute scan (observational; no claim beyond what it
   shows).
6. QC flat vs control where the screen is armed (D5 ±few %, rx-CB withholds
   ~14–15%, σ̂₀ 1.00); refloat count ≈ onset count.
7. Product claims on total_IF only (promotion-ledger convention).

## 8. Deferral triage (pre-declared)

- RT / consumer-path port (broadcast-ephemeris ~m-scale error is fine at a
  km bound; the port is wiring, not design) — after producer validation.
- External attribution of BSHM's failure mode (station ops / receiver
  firmware) — diagnostics commentary only; the guard stands on internal
  evidence.
- The retracted card's per-arc re-vet increment — revive only if a class
  survives THIS screen (none known does).
  **REVIVED 2026-08-02 as INC-4 (§10).** A class does survive, and it is not the
  one the retraction imagined: the screen's own admission RECORD outlives the
  ARC, so a channel that left the model set and came back inherited its pre-gap
  verdict and was screened as an ESTABLISHED cell. On `runs/ft63_24h_conv`,
  **16 of the 17 mid-arc onsets were re-entries** — there were no genuine
  mid-arc onsets on the day at all.
- The band-loss policy inconsistency (flicker INC-0) — referenced, reused,
  not fixed here.
- Relationship to `SPEC_rinex_zero_sentinel.md`: sibling ingest-hygiene
  family (written-zero sentinels vs corrupt values); no shared code path.

---

## 9. Implementation record (2026-08-01, Opus session)

Built as `filter/NetworkMeasurementAdmission.{hpp,cpp}` +
`tests/test_measurement_admission.cpp` (suite `MeasurementAdmission`, 10 cases).
Unit: **1087/1087 Debug cases pass** (1 pre-existing fixture skip), plus
`gnss_simulator_selftest` 35/35. Deviations from the spec as written are recorded
here rather than in a comment nobody reads, per the eq34-hygiene §8 precedent.

### 9.0b The centre is a median over SATELLITES, not over observables

§2 writes `z_i = d_i − med_station(d)` with the median "over the station's sats,
n ≥ 4". The first cut medianed the **code observables** — which is not the same
thing, and the difference is a screen inversion, not a nicety.

A flat median over observables weights a satellite by how many bands it happens to
be tracked on. Three single-band satellites (the exact BSHM shape — G07, G16, G21
and G17 are all C1C-only) against one four-band satellite puts four of seven
samples on ONE line of sight, so a corrupt four-band satellite **owns the centre**:
it reads z ≈ 0 and is admitted while its three healthy peers read z ≈ |d| and are
each rejected, their observables erased and their bias states pruned. The station
guard cannot save it — with one code per band the guard fires iff healthy bands
outnumber corrupt ones, which is the exact negation of the capture condition, so
in the regime where capture happens the guard is provably silent.

Reachable on the repo's own shipped network configs
(`config/ppp_rtk_network_internal.json`, `ppp_rtk_network_global_sh.json`:
GPS+Galileo, `frequency_mode "all"`, `channel_mode "all"`, 25° mask) at any
station near the satellite floor; `present[i]` carries no minimum-satellite gate,
and the §9.13 leg's own run-end reports 18 station-epochs under the floor on 121
epochs. It is arithmetically **inert** on the GPS-only `triple`/`single` arms this
card validated on (c ≤ 3 < `adm_min_sats` = 4 makes capture impossible), which is
why no field arm would have caught it.

Fixed by a two-level reduction: each satellite is first reduced to the median of
its own code terms, and the station centre is the median over those. The median's
sample size is then the satellite count — so `adm_min_sats` bounds the population
it names, exactly as §2 writes it. Pinned by
`OneMultiBandSatelliteCannotCaptureTheStationCentre`.

### 9.1 The statistic — implemented as `observed − modeled`, not as a product re-query

§2 writes `d = C_obs − ρ_pred(SP3) − c·dtˢ(CLK)`. **`PppObservation` carries no
satellite clock** — `SatGeometry` is a per-satellite function-local in
`PppModel::computeModels` (`PppModel.cpp:538`) and is destroyed each iteration, so
`geom.sat_clock` survives only folded into `modeled[]`. A literal implementation
would have to re-query `GnssCorrections::getSatelliteClockCorrection` through
`st.model->getGeometry().getCorrections()` — a second product interpolation per
LOS per epoch, at a *different* emission time than the model used, so it would not
even reproduce the model's own value.

Implemented instead as `d = observed[code] − modeled[code]`. `modeled` is
`computeBaseModeledValue` (`PppModel.cpp:1466`) =
`range − c·sat_clock − c·relativistic + ZHD·mf_d + antenna + bias + shapiro + otl
+ apl + solid_tide` (+ pole tide + higher-order iono), i.e. **the spec's
subtraction plus the deterministic corrections** — a strictly *tighter* healthy
spread, at zero cost and with no new dependency in the seam.

The load-bearing property is that `modeled` is **state-free**: no receiver clock,
no wet-tropo state, no first-order slant iono, no ambiguity. That is exactly what
lets the screen fire at a cell's **birth**, where a prefit innovation structurally
cannot (a correctly self-seeded birth drives its own innovation to ~0 whatever the
observable says — the reason §17 gate 6 of the census card could only find this
class after the fact). Pinned by a unit case
(`StatisticAndBandIdentityAreWellDefined`).

### 9.2 Seam placement — AFTER `networkSignalIntegrity`, not before

§4 does not fix the order relative to the flicker card. Implemented **after** it
(`ApplicationNetwork.cpp`, immediately following the `networkSignalIntegrity`
call). `modeled` is rebuilt from `computeModels` every epoch, so screening *first*
would make the classifier read every withhold as a band **loss**, a
`band_dropped_perm` after its flicker window, and a **FLICKER-REGAIN** on heal —
manufacturing precisely the events that card's INC-0 census exists to *measure*,
and making its counters incomparable between an admission-armed leg and its
control. The later slot costs nothing (this seam reads only `observed` /
`modeled`, which the classifier does not write) and the ordering window is
otherwise identical. Both cards' refloats compose: the funnel is a set.

### 9.3 Wider orphan-state prune than §4 specifies

§4 names the phase-bias prune. A **code** withhold orphans more, and all three
consumers derive from the STATES rather than the observables:

- `pruneSatPhaseBiasStatesExcept` — the anchor election (later in the epoch) could
  otherwise seat a datum pin on a row this seam removed;
- `pruneSatBiasStatesExcept` — the satellite OSB registration counts *stations
  holding a state* against its ≥2 coverage gate
  (`ApplicationNetworkAssembly.cpp:1634`), so an unpruned state phantom-supports a
  shared column only one station actually feeds: the single-station code column
  that is perfectly confounded with that receiver's clock;
- `pruneSatClockStatesExcept` — applied only when the withhold leaves the
  observation with **no observable at all**, because `registerClock` unions the
  clock states and a satellite that keeps another band is still a real supporter.

This mirrors the two-station observability gate's own "Prune the now-orphaned
shared states" (`ApplicationNetworkAssembly.cpp:1456`). All three counted apart.
The smoke measured 11/11/11 — G17@BSHM is C1C+L1C-only, so every withhold empties
its observation.

### 9.4 Station-guard reading of "hold all"

§3's `(hold all, …, mirror the IF-TD reference gate's incoherent-station
precedent)`, implemented as: hold **every screened band of that station** for the
epoch, emit one `adm_station_event`, and leave **every per-channel admission
record untouched** (no `admitted` flip, no heal progress) so the next epoch
re-tests from evidence. That is the IF-TD gate's `FireNoRebase`
(`IfPhaseSlipDetector.cpp:454`) transposed: act at the family's scope, refuse the
per-member claim.

**Added beyond the spec, and required by standing invariant #1:** the held arcs
**are refloated**. Erasing a station's observables punches a one-epoch hole; the
invariant-DD chord is keyed independently of the `StateManager` and would
otherwise ride straight across it with a stale value. The cost is a station-wide
re-convergence, which is proportionate to the trigger (>50 % of a station's code
channels more than a kilometre from the products) and is called out by a run-end
WARN. Per-channel `adm_census` events are also suppressed during a station event —
naming individual cells from a centre we have just declared untrustworthy would be
the claim the guard exists to refuse; the event's `worst=` field carries the
locator.

### 9.5 Refloat scope: UNCONDITIONAL, and the resume latch is per station

The first cut followed §3 literally — a **birth** rejection did not refloat ("no
state exists yet … pure admission"), while restore / onset / heal / station-guard
rejections did. **That was a defect**, found by the review pass in §9.15 and
confirmed against the code:

"Is this a birth or a restore?" is answered from the seam's own record map, which
is deliberately **not serialized**. On a resumed leg a station that is absent,
thin, or whose satellite the two-station observability gate erased on the epoch
the latch was consumed has no entry in that map — so its restored corrupt arc
presents as a *first sighting* at a later epoch, is labelled a birth, and has its
band erased with the Card-4 invariant-DD chord **left riding straight across the
hole**. That is standing invariant #1 violated, on exactly the class of arc this
card exists to catch.

Fixed by removing the condition rather than by improving the classification: the
refloat now runs on **every** rejection. At a true birth it costs nothing — the
funnel insert erases a DD chord that does not exist and a GM deviation that does
not exist, and `markAmbiguityPendingReseed` on a not-yet-allocated ambiguity is a
set insert PASS B consumes when it allocates. Measured: the gate-1 smoke's
`network_solution.csv` is **byte-identical before and after the change**
(md5 `15629c86…`), which is the no-op claim demonstrated rather than asserted.
Correctness must not ride a classification that can be wrong when the alternative
is free.

The birth/restore **event names** are kept — they are diagnostics, and §7 gate 2
grades on them — so the latch survives, now correctly scoped: `adm_resume_pending`
is a RUN flag (set at the warm-resume site, the `anchor_age_resume_seed` shape)
paired with `ctx.carry.adm_resume_seen`, the set of stations whose first screened
epoch has already happened. Each station gets its own restore epoch whenever it
first appears.

### 9.5b The station guard counts SATELLITES, not bands

§3 writes "more than `adm_max_frac` of a station's screened **bands**". Measured
against a band denominator the guard misfires: a single very bad satellite that
happens to be tracked on many bands (four of a four-satellite station's seven
channels is ONE corrupt Galileo line of sight) trips it, so the whole station is
held and three perfectly sound per-channel verdicts are refused.

The guard's question is "is the CENTRE trustworthy?", and the centre's population
is **satellites** — so that is its denominator: a station event fires when more
than `adm_max_frac` of the station's screened satellites carry a failing band.
That is what §3's own justification says ("the MEDIAN is untrusted"); the "bands"
wording does not survive contact with a multi-band constellation. Both counts are
in the event detail (`fail_sats=1/4;frac=0.250;fail_bands=4/7`) so the reading is
never hidden. Pinned by
`OneMultiBandSatelliteCannotCaptureTheStationCentre`.

### 9.6 INC-3's two columns

§3 says "station-worst |z| and network-worst |z| … two columns", which is
ambiguous on a file with one row per *epoch*. Implemented as
`adm_sta_worst_z` = the **median over screened stations** of each station's worst
|z| (the healthy sea) and `adm_net_worst_z` = the **network maximum** (the
extreme). A single maximum is an extreme, not a population, and §7 gate 4's
separation claim needs both halves to be falsifiable. The argmax cell's identity
goes to the events, exactly as §3 says.

`adm_census` events are emitted for **every** over-bound channel the response did
not already name this epoch (rate-limited per cell at a fixed 20-epoch holdoff —
no knob, because the INC-1/INC-2 events are transitions and self-limiting). §3's
"the argmax cell" would not satisfy §7 gate 5's "enumerates every |z| > bound
channel on the day".

### 9.7 Byte-identity: the two census columns are CONDITIONAL

Unlike the redundancy and prefit censuses — whose unconditional columns changed
every unarmed run's `network_epoch_status.csv` (the deweight card's gate-1 record
had to say so: "29 → 48 → 49 columns over the session") — the two `adm_*` columns
and the six manifest fields are emitted **only when the card is armed**
(`NetworkRunTraces::es_adm_columns_`, captured in `openAll`). This card's OFF
contract is *structural* byte-identity and its own §7 gates are same-build A/B
comparisons of these very files; `goldens/decomp/obs.manifest.json` hashes both.
Verified below.

### 9.8 Not fingerprinted

Recorded in place at `networkSnapshotFingerprint` as the **fifth** instance of the
policy-card exemption (after eq34 hygiene, flicker, deweight release, and the
observability families), plus a banner in `GnssConfigTypes.hpp` and one in the
loader. Forward-only admission policy, RAIM-class: arming it changes which
*observables* enter the model set from here on and re-references no carried
quantity. That is what makes every §7 gate — all of which replay from a shared
`snap_19`/`snap_20` seed with `state_snapshot_require: true` — legal at all.

### 9.9 Also landed

- `scripts/adm_make_arms.py` — the arm generator (smoke + leg families), with the
  §7 interaction discipline (no deweight-hygiene / fuse / redundancy-escalation
  knob on any arm) enforced in code rather than by hand.
- `pyviz` `_SHAPE_RULES`: `adm_heal` → `^`, `adm_*` → `v`. Without them the whole
  family rendered as the default circle and a rejection was indistinguishable from
  a readmit. The ADMISSION **tier colour** is deliberately left at the unknown-tier
  grey: extending `_TIERS` re-walks the distinguishable-colour list and would shift
  every existing tier's colour (18 tiers are already missing from it — a separate
  cleanup).
- `CLAUDE.md` test-suite figures refreshed (904/138/55 → 1087/172/71; the
  one-file-per-suite invariant re-verified, still holds).

### 9.10 Known gaps in this implementation

- **`sync_skip` epoch-status rows carry blank census columns.** The two columns are
  stamped from `NetworkStashStatus` (the emit path) and from `NetworkSolveGlue`'s
  `emitEpochStatus` lambda (which covers `datum_failed` / `gate_withheld` /
  `solve_failed` / `sigma0_withheld` — the four post-screen QC dispositions). The
  `sync_skip` sites in `ApplicationNetworkAssembly.cpp` run before the screen for
  that epoch, so blank is correct there — but the TOW-stamp guard means it is also
  *silent* rather than explicit.
- **`network.write_run_manifest`'s toggle dump is hand-enumerated**, and the whole
  prefit-census / redundancy / signal-integrity family is still absent from it.
  This card added its own six fields; the manifest's "full toggle set" claim
  remains overstated for its neighbours.
- **`scripts/config_posture_check.py --verify-loader` is broken in this tree**
  (it points at `GnssConfigLoadNetwork.cpp` for `logNetworkPosture_`, which moved
  to `GnssConfigNetworkPosture.cpp` in the 2026-08-01 split), so the
  ledger↔loader coverage check currently verifies nothing. Not this card's to fix,
  but worth knowing before trusting a "no missing rows" result. No POSTURE row was
  added here, per the §5 no-promotion rule and the split rule in that file's banner.

### 9.11 §7 gate 1 — smoke, AS RUN

Build: same-build Release, renamed exe (`gnss_processor_adm.exe`) per the
concurrent-session lock rule. Arms from `python scripts/adm_make_arms.py`
(cold start, 10 epochs, 63 stations, DOY-129 00:00): `ft63_adm_off` (nothing
armed), `ft63_adm_census` (`adm_trace` only), `ft63_adm_screen`
(`adm_screen` + `adm_trace`).

| check | verdict | evidence |
|---|---|---|
| knobs parse, arming + clamp lines print | **PASS** | both info lines and the solve-affecting WARN in `run.log`; the two sink-dependency warns fire when `trace_events` / `trace_epoch_status` are off |
| OFF twin schema unchanged | **PASS** | `ft63_adm_off/logs/network_epoch_status.csv` header ends at `pc_sh_n1k` — no `adm_*` columns; its manifest carries no `adm_*` field |
| **measure-only is solve-inert** | **PASS** | `census` vs `off`: `network_solution.csv` **md5-identical** (`0458c09a…`, 123 696 B) and `network_trace.csv` md5-identical. This is the arm that makes the census legal to leave armed anywhere. |
| the screen acts | **PASS** | `screen` differs from `off` in `network_solution.csv` (as it must) |
| counters, invariant #4 | **PASS** | 11 screen passes / 693 station-epoch blocks / 6458 LOS-epochs / **15 935 CODE channels screened**, 0 thin skips, 0 non-finite skips |

**The smoke found a third corrupt BSHM channel, on a different satellite, at a
different time of day.** At TOW 259200 (00:00:00) — eighteen hours before the
G07/G16 births §1 is written around — `BSHM00ISR` reports **G17 C1C at
z = +165 499 m** (d = +124 788.5 m against a station median of −40 710.8 m over
10 satellites), drifting −0.82 m/s over the ten epochs. Like G07 it is a
**C1C+L1C-only** line of sight; the withhold removed both observables.

Against it, the healthy sea in the same rows: `adm_sta_worst_z` = **9.1–9.9 m**
across all ten epochs. That is a **17 000× separation** between the corrupt
channel and the median station's worst channel — the §2 threshold claim,
measured, two decades clear on each side of the 1000 m bound.

Run-end (screen arm), verbatim numbers: 1482 birth screenings → 1481 admitted,
**1 rejected at birth**, 0 resume rejections, 0 onsets, 0 heals, 10 channel-epochs
held while already rejected, **0 station events**; 11 channel-epochs withheld
removing 22 observables and pruning 11 phase-bias / 11 code-bias / 11
satellite-clock states; 0 funnel insertions (correct — every withhold was a BIRTH,
which by design has nothing to refloat, and the dead-wiring WARN correctly stayed
silent because its trigger is `onsets + reject_resume + station_events > 0`);
2 events emitted, 9 suppressed by the holdoff.

Event stream, verbatim:

```
2313,259200.000,ADMISSION,adm_reject_birth,G17,BSHM00ISR,165499.308846,1000.000000,
  withhold,band=1;d=124788.500;med=-40710.808;n_sats=10
2313,259230.000,ADMISSION,adm_census,G17,BSHM00ISR,165252.472049,1000.000000,
  held,band=1;d=124541.320;med=-40711.152;n_sats=10
```

i.e. the channel was refused at the epoch its arc would have been born, and the
census then carried its identity through the hold at the rate limit — which is
exactly the INC-1 + INC-3 contract.

**Caveat stated plainly:** this is a 10-epoch cold smoke at a window that contains
neither of §1's named channels. It establishes the mechanism, the separation and
the OFF/measure-only identity. It establishes nothing about §7 gates 2–7, which
need the leg replays.

### 9.12 §7 gate 1b — the OFF byte-identity gate CANNOT PASS in this tree

The `goldens/decomp` matrix is the repo's real OFF-identity harness and its `obs`
arm exercises exactly the writers this card touched (`network_epoch_status.csv`,
`network_events.csv`, `network_run_manifest.json`). Run against
`goldens/decomp/obs.manifest.json` with a same-build Release exe, it **FAILS — on
two divergences that both predate this card**, and per the harness's own rule
("never regenerate goldens or masks to make it pass") nothing was regenerated:

1. `run.log:24` — `Elevation power: code=1.00, phase=0.50` (golden) vs `phase=1.00`
   (now). That is the `phase_elevation_power` default flip landed 2026-07-31 by a
   different workstream. It changes the weighting, hence σ̂₀, hence every
   solve-derived artifact, so the comparison halts before reaching any of them.
2. The golden's `network_epoch_status.csv` header is **17 columns**; the current
   unarmed run writes **48**. The 31 added columns are the P1-1/P1-2/P1-4 frame
   guards, the redundancy census and the prefit census. The manifest has likewise
   gained 14 network keys (`fast_clock_*`, `state_snapshot_*`, `process_noise`).

The same comparison is what makes this card's own claim checkable, so the
structural half was extracted by hand — and it is the stronger statement:

| | golden (pre-change) | this build, card OFF |
|---|---|---|
| `network_epoch_status.csv` columns | 17 | 48 — **none of them `adm_*`** |
| manifest `network` keys added since golden | — | 14 — **none of them `adm_*`** |

i.e. of the 45 fields other observability cards have added to the *unarmed*
artifact since that baseline, this card added **zero**. Re-baselining the decomp
goldens for the phase-power flip is owed by whoever landed it; it is a
prerequisite for any future OFF-identity gate in this tree and is NOT this card's
to do.

### 9.13 §7 gate 2 — the KNOWN-ANSWER birth gate, AS RUN: **PASS**

`runs/ft63_adm_birth` (armed) and `runs/ft63_adm_birthoff` (same-build control),
both from `snap_19` + `start_offset_hours 18.1125`, `state_snapshot_require:
true`, 121 epochs, 63 stations, `gnss_processor_adm.exe`. Graded by
`python scripts/adm_gate_verdict.py --gate birth`.

The two rejections the spec predicted, at the two epochs it predicted, with the
two magnitudes it predicted — verbatim from `network_events.csv`:

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

Both predictions were derived independently, from the products, in a different
session, months of investigation apart from this code. The screen reproduced them
to within a tenth of a percent on its first armed leg.

Run-end, verbatim: 121 screen passes / 7605 station-epoch blocks / 68 707
LOS-epochs / **173 591 CODE channels screened**; 18 station-epochs skipped below
the 4-satellite median floor, 0 non-finite; **62 channel-epochs over the bound**,
run max |z| 198 170 at `BSHM00ISR|G07|1`; 1738 birth screenings → 1736 admitted,
**2 rejected at birth**, 0 resume rejections, 0 onsets, 0 heals, 60 held; 62
withheld removing 124 observables and pruning 62 phase-bias / 62 code-bias / 62
satellite-clock states; **62 funnel insertions** (0 pending-reseeds — this lineage
runs `phase_leveling` off, and the two feeds are counted apart so that reads as
"one feed is not armed" rather than "nothing happened"); 6 events emitted, 56 held
by the rate limit.

The warm-resume RESTORE rule ran on the leg's first pass and rejected nothing,
which is right: G07's arc is born at 18:50:30, forty-four minutes into the leg.

**The §2 threshold claim, measured — and it is a four-decade gap with nothing in
it.** The INC-3 census columns over all 121 epochs of this leg (63 stations,
173 591 screened channels), split at the epoch the first corrupt arc is born:

| | epochs | worst network |z| |
|---|---|---|
| before TOW 327030 (no corrupt channel live) | 87 | **54.3 m** |
| from TOW 327030 (corrupt channels live) | 34 | max **198 170 m**, and the per-epoch max never falls below **194 621 m** |

The healthy sea — `adm_sta_worst_z`, the median over stations of each station's
worst channel — sits at **8.5–10.6 m** across every one of the 121 epochs, storm
day included. So `adm_bound_m = 1000` sits **18× above the worst legitimate |z|
anywhere in the network** and **195× below the corrupt class**, with an empty
band of nearly four decades between 54 m and 195 km. §2 estimated "≥10× above …
≥100× below" from a single station's eleven satellites; both hold, network-wide,
with margin.

That also *predicts* §7 gate 4: any bound in (54 m, 194 621 m) must produce the
identical reject set, so 300 / 1000 / 3000 cannot disagree unless something other
than magnitude is driving the decision. The sweep is therefore a real test of the
implementation, not a re-measurement of the physics.

### 9.13b §7 gate 3 — THE BREAK GATE: **PASS**, and it closes the `dwrat` hunt

`runs/ft63_adm_break` (armed) vs `runs/ft63_adm_breakoff` (same build, nothing
armed), both from `snap_20` + `start_offset_hours 19.1125`,
`state_snapshot_require: true`, 121 epochs through the era-2 event.

`a00_tecu` from `network_vtec_sh_health.csv`, with the epoch-status disposition:

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

Window peak |a00|: **3380.6 → 36.9 (92×)**. Dispositions over the 121-epoch leg:
OFF **83 `sh_gate_withheld` / 38 emitted** (the documented multi-hour era,
reproduced); ON **121 emitted, 0 withheld**. §7.3 asked for "NO SH engagement at
329160, a00 < 150". Measured: **no SH engagement at ANY epoch of the leg**, and
the armed arm never leaves the 32–37 TECU band — it does not notice that an event
happened.

Response telemetry on this leg: 121 screen passes, 173 287 CODE channels screened,
110 channel-epochs withheld removing 220 observables, 110 phase-bias / 110
code-bias / 109 satellite-clock states pruned, and **110 funnel insertions** —
the unconditional refloat of §9.5 doing its job. `pending_reseed` is 0 because
this lineage runs with `phase_leveling` off; the two feeds are counted apart
precisely so that reads as "one feed is not armed" rather than "nothing
happened".

**The free experiment resolved, and it retires an open hunt.** §7.3: "read 329190:
if the 521 TECU residual ALSO vanishes, it was downstream of the same two
channels; if it persists, it is independent and the `dwrat` hunt continues.
Either outcome is decisive — record which."

**It vanishes.** 329190 reads **35.35** on the armed arm. The residual excursion
that survived `SPEC_deweight_release_hygiene`'s INC-3b — the one that defeated all
three of that card's falsification hypotheses (k-sweep flat, release-timing,
row-starvation) and left its §7.3 marked "era-2 is NOT closed" — was **downstream
of the same two BSHM channels**. Killing them at admission removes both the
primary discharge and the residual. `runs/ft63_leg19_dwrat` need not be re-run.

*Magnitude caveat, stated rather than glossed*: this OFF control reads a00 = 3380
at the break where the historical record says 5738.6. The arms derive from
`_ft63_leg19_base.json` with the §7 interaction knobs stripped, and the tree also
carries another workstream's `phase_elevation_power` 0.5 → 1.0 default flip, which
changes the weighting and hence the magnitude. The **event** is unmistakably the
same one — engage at exactly TOW 329160, multi-hour withhold era, slow decay — but
do not compare 3380 against 5738.6 as though the two runs were the same
configuration.

### 9.13c §7 gate 4 — the bound sweep: **PASS**

`adm_bound_m` ∈ {300, 1000, 3000} on the gate-2 leg. Rejected channel set:

| bound | rejected channels |
|---|---|
| 300 | (BSHM00ISR, G07, band 1), (BSHM00ISR, G16, band 1) |
| 1000 | *identical* |
| 3000 | *identical* |

Identical across a decade either side of the default — the separation claim of
§2 made falsifiable and not falsified, and consistent with §9.13's measured gap
(nothing at all between 54.3 m and 194 621 m). Note gate 4's `b1k` arm is the same
run as `birth`; the sweep is graded `b300` / `birth` / `b3k`.

### 9.14 §7 gates 5–7 — status at hand-off

- **Gate 5 (negative + era-1, 24 h with `adm_trace`, screen off)**: not run
  (~80 min). The 10-epoch smoke already delivered a preview of what it is for —
  it found a **third** corrupt BSHM channel, G17 C1C at +165.5 km at TOW 259200,
  eighteen hours away from the two this card was written around. The full-day
  census is the arm that would enumerate the rest.
- **Gates 6 (QC flat) and 7 (total_IF product claims)**: not graded. Both want a
  run pair the harness parses for D5 rates / rx-CB withhold % / σ̂₀, which §17 of
  the prefit card records as not being emitted in a form that harness reads.

### 9.15 Adversarial review record (2026-08-01)

Five-dimension review (seam correctness, standing invariants #1/#4 + state
orphaning, OFF byte-identity, config/resume/fingerprint, traces/events/test
adequacy), each finding then handed to an independent refuter instructed to
default to REFUTED unless it could trace a concrete path from real inputs to a
wrong output. **10 findings raised, 8 refuted, 2 confirmed — both fixed:**

1. **The station centre was a median over OBSERVABLES, not satellites** (§9.0b).
   A screen inversion: one multi-band corrupt satellite could own the centre and
   be admitted while its healthy single-band peers were rejected, with the station
   guard provably unable to fire in exactly that regime. Live on the repo's shipped
   GPS+Galileo `all`/`all` configs; arithmetically inert on the GPS-only
   `triple`/`single` arms this card validated on, which is why no field gate would
   have found it.
2. **The warm-resume RESTORE latch was a single global one-shot** (§9.5). A station
   absent, thin, or satellite-pruned on the epoch that consumed it would have every
   later first sighting read as a birth — so a restored corrupt arc's Card-4 chord
   rode across the withhold hole unrepaired. Standing invariant #1.

The second fix subsumes the first half of the problem by removing the dependency
rather than improving the classifier: the refloat is now unconditional, and the
per-station latch survives only as an event LABEL. A third change fell out of
writing the test for #1 — the station guard's denominator (§9.5b).

Two unit cases were added for the two defects
(`OneMultiBandSatelliteCannotCaptureTheStationCentre`,
`ResumeRestoreLabelIsPerStationNotAGlobalOneShot`), and
`CorruptChannelRejectedAtBirthWhileHealthySiblingsAdmit` flipped its
`refloat_ambs == 0` assertion to `> 0`. Suite: **12 cases**, full tree
**1088/1089** (1 pre-existing fixture skip).

**Every field number in §9.11–§9.13c was re-measured on the FIXED binary**
(`gnss_processor_adm2.exe`, all four leg arms and the smoke trio re-run from fresh
snapshot copies). Every verdict is unchanged and every z value reproduces to the
digit — as predicted: fix 1 is arithmetically inert on GPS-only `triple`/`single`
data, fix 3 never had an opportunity (0 station events on any arm), and fix 2 adds
only funnel insertions on epochs that were already being withheld. The gate-1
smoke came back **md5-identical** to the pre-fix run, which is the "refloating a
birth is a no-op" claim demonstrated rather than asserted. The one visible
difference is the intended one: `refloat_ambs` went 0 → 62 (birth leg) and
0 → 110 (break leg).

---

## 10. INC-4 — the arc RE-ENTRY vet (2026-08-02, Opus session)

Revives the §8 deferral "the retracted card's per-arc re-vet increment — revive
only if a class survives THIS screen". One does. It is not the class the
retraction imagined, and it was hiding inside this card's own onset counter.

### 10.1 The finding — there were no mid-arc onsets

`MeasurementAdmissionArc` records are kept for `kAdmArcForgetEp` = 2880 epochs
(one day at 30 s), which is far longer than an arc. So a channel that leaves the
model set and returns is still carrying `admitted = true` from **before** the
gap, and the seam's `else if (a.admitted)` branch dispatched it down the
ESTABLISHED path (INC-2 onset) rather than the birth path (INC-1). The verdict
being inherited was made about a channel that is not necessarily the one that
came back.

Measured on `runs/ft63_24h_conv` (63 stations, 24 h, 2880/2880 emitted, this card
armed): **16 of the 17 `adm_onset` events are preceded by a `T0_gap arc_resume`
on the same cell within five minutes.** There were no genuine mid-arc onsets on
the day at all. The 17th (G07@BSHM, TOW 329640, band 2) is the second band of a
channel whose band 1 had already been refused.

The class splits cleanly in two, and the halves need different things:

| | n | shape | cost (a00) |
|---|---|---|---|
| **A — mislabel only** | 12 | screen fires at the SAME epoch as the resume, after gaps of 3–2025 epochs | none — the response was already right |
| **B — real exposure** | 4 | channel re-admitted, crosses the bound 2–8 epochs later | the day's two largest iono excursions |

**Class A is the more embarrassing half.** One of its twelve is G16@BSHM at TOW
327210 with `gap_ep=568` — the satellite had been gone 4.7 hours. That is the
*exact* channel §1 of this card is written around, and §9.13's gate-2 leg replay
calls it `adm_reject_birth` for the same cell at the same epoch with the same
z = 97 537, purely because a leg's record map starts empty. G07@BSHM at 327030
(`gap_ep=2025`) is the same story. A classification that depends on how long the
run has been going is not a classification.

**Class B is where the cost is**, and it is in the exposure window rather than at
the onset epoch, which is the opposite of where one would look:

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

Against that, the day's **eight `adm_reject_birth` events move a00 by a median of
−0.11 TECU (max +0.32)**. That asymmetry is the whole argument for the increment
and it is exactly standing invariant #7's two edges: refusing at ADMISSION is
free, because nothing was ever coupled; amputating an ESTABLISHED arc discharges
whatever tension it accumulated while it was trusted.

**And the census already knew.** All four class-B cells carry a
`prefit_exceedance` before this card's bound reaches them — at return + 1, +3, +4
and +1 epochs respectively. The G16 row, verbatim:

```
2313,307770.000,PREFIT,prefit_exceedance,G16,BSHM00ISR,1439.600121,1000.000000,
  flag,class=obs_code row=12 y=975.544 m=16.5467 w=2.25346
```

Raw `y` = 975.5 m — *under* this card's 1000 m bound — but the census statistic is
standardized (`|y − m|·√w`), so it reads s = 1439.6 against its own threshold of
1000. The two instruments genuinely see different things, which is what makes one
useful to the other. §1 always called the census "the detection net"; INC-4 is the
wire from it to the response.

### 10.2 Design

`network.adm_revet` (default false, requires `adm_screen`). A channel absent for
at least `adm_revet_gap_ep` **screened** epochs is re-screened as a BIRTH instead
of inheriting its pre-gap verdict, and then carries an open VET WINDOW for
`adm_revet_ep` epochs during which a PREFIT-census `obs_code` exceedance on the
line of sight (previous epoch) refuses as surely as the |z| bound does.

Four properties are load-bearing:

1. **Class A costs nothing to fix.** A birth screen admits a clean channel
   immediately, so a healthy re-entry gets no withhold, no refloat and no solve
   change. The fix for the twelve is a relabel with an identical response — which
   is why it can ship armed without a cost argument.
2. **The gap is measured in epochs THIS SEAM SCREENED.** A station under
   `adm_min_sats` or a satellite the two-station observability gate erased both
   read as absent. That is the conservative direction (it re-screens), and it
   keeps the clock the same one `last_ep` and `kAdmArcForgetEp` already use.
3. **A heal opens a vet window too.** A heal is a re-entry in all but name — the
   same arc returning with a fresh seed after an absence. Without this, the one
   path that puts a previously-REJECTED channel back into the solve would be the
   only one that puts it back unvetted.
4. **Refusing re-arms the window**, so a channel that fails inside its vet serves
   a full window again rather than the remainder of an old one.

`network.adm_revet_hold` (default **false**) is the strict posture: withhold a
clean re-entry until the ordinary heal path readmits it. It is the only posture
that closes class B's exposure window outright — see §10.4, where that is stated
as a limitation rather than glossed. It is default-OFF on the prefer-fallback
directive and on the flicker card's INC-3 deviation 9: it would withhold every
re-entry (~4650 on a 63-station day, 1.6/epoch into a funnel the Iono-2 v3k erase
policy is tuned on), and the population it delays is dominated by legitimately
RISING satellites. A producer that admits every rising satellite 2.5 minutes late
across 63 stations is paying an observability cost to catch a class the relabel
already refuses on arrival whenever the fault is present on arrival.

### 10.3 Deviations and things worth knowing

**The census flag is per LINE OF SIGHT, not per band.** The census's worst is
taken over a satellite's code rows and there is no row → band map to resolve it
further, so a flagged satellite refuses whichever of its bands is currently
vetting. The flicker card's demotion map made the identical trade for the
identical reason (its deviation 6). Conservative, bounded by the vet window, and
`revet_census_refusals` is counted apart so the trade is auditable.

**The flag is written from the EXCEEDANCE, upstream of `census_on`, the
event-class mask and the per-cell holdoff.** The census card's own principle is
that a detection must not depend on whether anyone is listening on the other
side; a trigger throttled by a diagnostic rate limiter would go quiet exactly when
a cell is persistently bad. It is written whenever the prefit WALK runs
(`trace_prefit_census` ∪ `seed_birth_check_sigma` ∪ `trace_topology`), not only
under the census knob.

**CODE rows only**, matching this card's own CODE-only-by-principle rule. A phase
row's prefit innovation is arc-specific and its station median is not a common
mode — the census card measured 608 of 665 phase events on one leg as
non-findings, and a phase-driven trigger wired to an admission verdict would be a
false-positive generator.

**`revet_census_reads` exists because `revet_window_ep` is not its denominator.**
The return epoch is inside the window but takes the birth-equivalent path, which
asks the census nothing — and could not usefully, because a re-entering arc's
ambiguity is fresh, so the census defers it as a BIRTH on exactly that epoch. So
`reads = window_ep − reentries` identically, and the dead-wiring warn keys on
`absent == reads` rather than on `absent == window_ep`, which would never be true.
This is measured, not asserted: the §10.5 smoke reports 67 window epochs, 12
re-entries and 55 consultations.

**How much the census can be worth, and why it is bounded.** Its flags are read
one loop epoch late (the walk runs *after* this seam in the epoch) and it cannot
flag the return epoch at all (birth deferral), so the earliest readable flag is
return + 2. On the four class-B cells the flags land at return + 1, +3, +4 and +1
— readable at +2, +4, +5, +2 — against bound crossings at return + 2, +5, +8 and
+2. So it pre-empts on G13 (+5 → +4) and G22 (+8 → +5) and **ties on G16 and
G22-first** (+2 → +2). Measured in §10.7: exactly those two refusals, worth
4.33 TECU off G13's peak epoch. A real gain on half the class and no loss on the
rest — but not the mechanism that CLOSES the exposure, because a cell whose ramp
crosses the bound within two epochs of its return can never be pre-empted by an
instrument that is structurally two epochs behind. That is `adm_revet_hold`.

**`adm_revet_ep` default 8, derived not picked.** Max measured census-flag lag is
return + 4, +1 for the read latency = return + 5, and coverage runs to
return + (`adm_revet_ep` − 1). 8 gives three epochs of margin. A window that is
too LONG costs nothing on a healthy channel (the census simply never flags it), so
the asymmetry is deliberate.

**Not fingerprinted** — the SIXTH instance of the exemption, recorded in place at
`networkSnapshotFingerprint`. Worth stating for INC-4 specifically because it
reads a cross-epoch record and that looks fingerprint-relevant at a glance. It is
not: `vet_left` and `last_ep` live in the same unserialized record map as
`admitted`, so a resumed leg opens no vet windows and every channel reads as a
first sighting — the RESTORE rule's case, already handled by §9.5. This is what
makes the INC-4 OFF/ON pair on one shared snapshot legal, which is how §10.6 is run.

### 10.4 What this increment does NOT do — stated, not glossed

**It does not fully close class B's exposure window at the shipped defaults.**
With `adm_revet_hold` off, a corrupt channel that is *inside the bound on arrival*
is still admitted and still seeds. Measured (§10.7): the census pre-empts the
bound by 1 and 3 epochs on two of the three class-B cells these legs contain,
worth 4.33 TECU off G13's peak epoch — but it ties on G16, where the whole +19.2
TECU excursion survives. The full save needs `adm_revet_hold` (§10.8).

**A tighter vet bound was not shipped in this increment**, because at the time it
would have been a threshold invented from two extrapolated samples — what
`feedback_mw_thresholds_empirical` says not to do, with a failure mode
(discarding freshly-risen low-elevation satellites) that
`feedback_prefer_fallback_over_drop` says not to accept. **That measurement has
since been made: §10.10.** It supports a vet bound, and it also shows exactly
which cell an absolute bound cannot reach.

**The band-loss policy interaction of §3 is unchanged** — INC-4 withholds through
the same path INC-1/INC-2 do.

### 10.5 Gate 1 — smoke + OFF byte-identity, AS RUN: **PASS**

Same-build Release, renamed exe (`gnss_processor_revet.exe`). Cold start, 10
epochs, 63 stations, DOY-129 00:00, from `python scripts/adm_make_arms.py`.

The control is `rvsmokeoff`, **not** `screen`: `screen` lacks
`trace_prefit_census`, whose own epoch-status columns would land in the diff and
be misread as an INC-4 effect. Both arms carry the census; the arms differ in
`adm_revet` and nothing else.

| artifact | `rvsmokelbl` vs `rvsmokeoff` | md5 |
|---|---|---|
| `network_solution.csv` | **IDENTICAL** | `15629c86…` |
| `network_trace.csv` | **IDENTICAL** | `948ffbab…` |
| `network_epoch_status.csv` | **IDENTICAL** | `ff54584f…` |
| `network_events.csv` | **IDENTICAL** | `63ce6174…` |
| `network_vtec_sh_health.csv` | **IDENTICAL** | `076aaeb2…` |

`network_solution.csv` is also md5-identical to the §9.5 / §9.15 record's
`15629c86…`, i.e. to the pre-INC-4 binary — so the increment is byte-inert on
this window against two independent baselines.

**`rvsmoke` — the arm carrying the SHIPPED defaults, 200 m vet bound included —
is byte-identical to both** (`network_solution.csv` and `network_events.csv` both
match `rvsmokelbl` exactly). All twelve of the smoke's re-entries sit under 200 m,
so the tighter bound refuses nothing there either. That is a third window in which
the vet bound does not touch a healthy re-entry, and it is why gate 1 passes for
the default configuration and not merely for the label-only one.

Counters, verbatim: **12 re-entry screenings → 12 admitted, 0 refused; 67
channel-epochs inside a vet window, 4 windows run to completion; census trigger
on → 0 of 55 consultations refused, 0 found no fresh flag to read.**

That is the reading the increment wants on clean data, and every part of it is a
denominator rather than an absence: the vet ran 12 times, the cross-seam census
wire was consulted 55 times and was FRESH on all 55 (proving the one-epoch stamp
handshake works in the field, not just in the rig), and it refused nothing because
nothing deserved refusing. `67 − 12 = 55` confirms the `reads`/`window_ep`
relationship of §10.3 on real data.

### 10.6 Gate 2 — the class-B known-answer legs

ON/OFF pairs on ONE shared snapshot (legal: the family is out of the fingerprint),
each window chosen to CONTAIN measured class-B re-entries:

- `rv04lbl` / `rv04off` — `snap_04` + offset 3.1125 (TOW 270405), 4500 s: contains
  **G13@BSHM** (resume 273240 gap 1 → onset 273390, prefit flag 273330) and
  **G22@BSHM** (resume 273450 gap 9 → onset 273690, prefit flag 273570).
- `rv14lbl` / `rv14off` — `snap_14` + offset 13.1125 (TOW 306405), 3600 s: contains
  **G16@BSHM** (resume 307740 gap 3 → onset 307800, prefit flag 307770).

Both arms carry `trace_prefit_census`.

**The OFF arms reproduce the control's class-B events to the digit**, which is
what makes these legs known-answer gates rather than new measurements:

| leg | event, verbatim from the OFF arm | matches `ft63_24h_conv` |
|---|---|---|
| `rv14off` | `307800 adm_onset G16 BSHM00ISR 1233.810182 … d=-39459.265;med=-40693.075;n_sats=8` | **exact, every field** |
| `rv04off` | `273390 adm_onset G13 BSHM00ISR 1198.329938 … d=-39510.103;med=-40708.433;n_sats=9` | **exact** |
| `rv04off` | `273690 adm_onset G22 BSHM00ISR 1102.758610 … d=-39602.092;med=-40704.851;n_sats=9` | **exact** |

and each carries its PREFIT precursor in the leg too — G16 s=1458.8 at 307770
(y=982.8), G13 s=1351.6 at 273330 (y=761.1), G22 s=1009.5 at 273570 (y=693.4).

### 10.7 Gate 2 result — the census trigger fires, and it fires UNDER the bound

**`rv04lbl` vs `rv04off`: two `adm_revet_refuse` events, both on evidence the
|z| bound could not have produced.**

```
2313,273360.000,ADMISSION,adm_revet_refuse,G13,BSHM00ISR, 975.277688,1000.000000,
  withhold,band=1;d=-39730.416;med=-40705.694;n_sats=9;trigger=prefit_census;vet_left=8
2313,273600.000,ADMISSION,adm_revet_refuse,G22,BSHM00ISR, 782.240601,1000.000000,
  withhold,band=1;d=-39922.234;med=-40704.475;n_sats=9;trigger=prefit_census;vet_left=8
```

|z| = **975.3** and **782.2 m** against a 1000 m bound. Attribution is
unambiguous — the bound was not reached at either epoch, and the OFF arm's own
`adm_onset` for the same cells lands at 273390 (z = 1198.3) and 273690
(z = 1102.8). **One epoch early on G13, three on G22**, precisely the §10.3
prediction from the flag lags.

And it is worth what it costs:

```
G13   (resume 273240, OFF onset 273390)
TOW        273180   273240   273270   273300   273330   273360   273390   273420   273480
rv04off     32.88    33.78    36.29    39.14    41.94    43.56    40.52    38.27    36.34
rv04lbl      32.88    33.78    36.29    39.14    41.94    39.23    37.34    36.12    34.94
                                                          ▲ refusal lands here
```

Identical through 273330 — the arms cannot differ before the refusal — then the
peak epoch drops **43.56 → 39.23, a 4.33 TECU cut**, and the leg stays ~1.4 TECU
lower for its remainder (G22's whole window inherits it: 34.99 → 34.11 at the
refusal epoch, 34.44 → 33.77 by 273840). Peak excursion over the 32.88 pre-event
level: **+10.68 → +9.06**.

`rv04lbl` also carries a second class-A relabel — G15@BSHM at 274140,
`adm_onset` → `adm_reject_reentry` with `gap_ep=3`, response byte-identical.

Run-end: **141 re-entries → 140 admitted, 1 refused on the return epoch; 786
vet-window channel-epochs; 2 of 624 census consultations refused, 0 absent.**
Two refusals out of 624 consultations is the shape a good trigger should have —
it is not a second robustness tier, it is a tripwire that fires on the class the
bound is structurally one epoch behind.

### 10.7b …but it does NOT close the class where the bound ties

**`rv14lbl` vs `rv14off`: a00 is identical at every epoch of the exposure window.**

```
TOW        307650   307680   307710   307740   307770   307800   307830   307920
rv14off     39.71    39.67    39.56    56.62    58.77    49.12    47.05    46.71
rv14lbl      39.71    39.67    39.56    56.62    58.77    49.12    47.05    46.71
```

The two arms differ by exactly ONE line in `network_events.csv`, and it is a
class-A gain, not a class-B one:

```
2313,307590.000,ADMISSION,adm_reject_reentry,G21,BSHM00ISR,63411.873777,
  1000.000000,withhold,band=1;d=22719.880;med=-40691.993;n_sats=8;gap_ep=12
```

G21@BSHM had already been refused at birth (306480), went away for 12 epochs, and
came back still corrupt. In the OFF arm that epoch fell down the
`rejects_held` path and emitted **nothing**; INC-4 names it. Same response, new
visibility — the increment recovering a re-entry the instrument could not
previously see, which is the same shape as the topology ledger's 13 silent
re-coupling edges under invariant #7.

**G16 stays an `adm_onset` at 307800, and §10.3 predicted exactly this.** Its
re-entry at 307740 was *inside the bound*, so a birth screen admits it; the census
flag written at 307770 (s = 1458.8, y = 982.8 — present in the leg, verified) is
readable at 307800, the same epoch the bound itself fires. A tie.
`revet_census_refusals` = 0 of 369 consultations, 0 absent: the wire was live and
fresh on every one, it simply had nothing to add that the bound had not already
reached.

So the default posture's class-B record across the three measured cells is
**2 pre-empted (G13 by 1 epoch, G22 by 3), 1 tied (G16)**. It is a partial fix,
and which half a cell lands in is decided by whether its ramp puts the census
flag more than one epoch ahead of the bound crossing — a property of the fault,
not of the configuration. That is why the posture below matters.

### 10.8 The decisive arm — `adm_revet_hold` erases the event: **PASS**

`rv14hold` = `rv14lbl` + `adm_revet_hold`. Same snapshot, same build.

```
TOW        307650   307680   307710   307740   307770   307800   307830   307920
rv14off     39.71    39.67    39.56    56.62    58.77    49.12    47.05    46.71
rv14hold    39.72    39.48    39.70    39.51    39.35    39.30    39.21    39.23
```

**Peak excursion +19.2 TECU → 0.0.** The armed arm never leaves the 39.2–39.7 band
— it does not notice that an event happened, the same shape §9.13b's break gate
produced. G16 at 307800 is no longer an onset at all but an `adm_census … held`
row: it was refused on arrival at 307740 and never entered the model set.

**And the control's "recovery" was never a recovery.** OFF decays 58.77 → 46.71
and stays there — a permanent **+7.1 TECU scar** against a 39.6 pre-event level.
`rv14hold` ends at 39.23. The withhold that lands at 307800 in the control arms
stops the excursion growing; it does not undo what was already absorbed. That is
standing invariant #7's decoupling edge — the amputation bounds the future, not
the past — and it is the strongest argument in this card for refusing at
admission rather than responding later.

**Measured cost of the strict posture on this leg:**

| | `rv14off` / `rv14lbl` | `rv14hold` |
|---|---|---|
| CODE channel-epochs screened | 169 663 | 169 663 |
| channel-epochs withheld | 184 (0.108 %) | 474 (0.279 %) |
| …of which clean re-entries held | — | 71 |
| epochs emitted | 121 / 121 | **121 / 121** |
| vet-window channel-epochs | 458 | 654 |
| census consultations / absent | 369 / **0** | 299 / **0** |

So the posture that closes the class costs **+0.17 percentage points** of the
screened channel population and withholds no epoch. That is a far smaller price
than the §10.2 argument assumed, and it is the number the promotion decision
should be made on.

**It stays default-OFF anyway, and the reason is what this leg cannot show.** 121
epochs measures the EVENT cost; it cannot measure the convergence cost of
admitting every rising satellite `adm_heal_ep` epochs late across a full day and
305 stations, which is the actual objection. `σ̂₀` would be the QC arbiter and it
is **not gradeable here** — the `sigma0` column reads 0.0000 on all three arms,
the same gap §9.14 records against gates 6/7. Promotion needs the 24 h arm and a
σ̂₀ source, not another leg.

### 10.11 Gate 3 — the tighter vet bound, AS RUN: **PASS**

`rv14on` / `rv04on` = the gate-2 legs at the SHIPPED defaults (`adm_revet_bound_scale`
0.2, a 200 m vet bound). The `rv*lbl` arms pin the scale to 1.0 so they keep
testing the relabel + census only.

**Arm-naming note, because it is a trap this repo has paid for before.** These
arms were originally `rv*vb` with `rv*on` pinned to 1.0 — i.e. an arm named `on`
that did NOT carry the shipped configuration. That is the same shape as `pcarry`
being silently forced off and `innov_refold` parsing inert: the next session
grades the wrong thing and never learns it did. Renamed so `on` always means
"what a user gets", with `lbl` for the label-only variant; the run directories
were migrated with the tags so the evidence and the names cannot drift apart.

**G16@BSHM is refused on its return epoch, at the |z| §10.10 predicted:**

```
2313,307740.000,ADMISSION,adm_reject_reentry,G16,BSHM00ISR,670.281206,1000.000000,
  withhold,band=1;d=-40022.233;med=-40692.515;n_sats=8;gap_ep=2;trigger=vet_bound
```

670.281206 m against §10.10's measured 670.3. The 307800 onset never happens —
that epoch is now an `adm_census … held` row on a channel that never entered the
model set.

```
TOW        307710   307740   307770   307800   307830   307920
rv14off     39.56    56.62    58.77    49.12    47.05    46.71
rv14on      39.56    39.46    39.30    39.34    39.25    39.31
```

Peak excursion **+19.2 → 0.0 TECU**, and the +7.1 TECU permanent scar with it —
the `rv14hold` outcome of §10.8, but with **70 of 72 re-entries still admitted**
instead of 0 of 72.

**And on the other leg, the whole class-B window flattens:**

```
TOW        273180  273270  273330  273360  273450  273600  273690  273840  274200
rv04off     32.88   36.29   41.94   43.56   36.68   34.99   34.64   34.44   33.81
rv04lbl      32.88   36.29   41.94   39.23   35.02   34.11   33.87   33.77   33.42
rv04on      32.89   33.37   32.79   32.76   32.60   32.89   32.83   32.95   32.94
```

`rv04on` never leaves 32.6–33.4 against a 32.88 pre-event level. Both the +10.7
excursion and the residual level shift are gone; the OFF arm is still 0.9 TECU
above baseline at 274200, the vet-bound arm is at baseline.

Refusals, all four attributed to the vet bound and none to the outer one:

| TOW | cell | \|z\| | lag | trigger |
|---|---|---|---|---|
| 272340 | G14@BSHM | 218.5 | `gap_ep=10` | `vet_bound` |
| 273270 | G13@BSHM | 308.5 | resume+1 | `vet_bound` |
| 273450 | G22@BSHM | 347.6 | resume+0 | `vet_bound` |
| 307740 | G16@BSHM | 670.3 | resume+0 | `vet_bound` |

G14 is a cell no earlier pass had catalogued. Cost: 153 vs 140 channel-epochs
withheld (**+9.3 %**), 121/121 epochs emitted.

**The census contributed nothing once the bound was armed: 0 refusals of 616
consultations (rv04on), 0 of 367 (rv14on).** §10.10's disjoint-coverage claim is
retracted — see the correction there. The bound reaches G13 at resume+1 where the
census reached it at resume+4, and G22 at resume+0 where the census reached it at
resume+5.

#### The defect this gate found, and the fix

The first `rv04on` pass produced a **flicker** the unit suite had no case for:

```
273450  adm_reject_reentry  G22  z=347.6  trigger=vet_bound
273600  adm_heal            G22  z=782.2  readmit            <- readmitted at 782 m
273630  adm_revet_refuse    G22  z=888.5  trigger=vet_bound
```

The heal path tested `fail` — the **absolute** 1000 m bound — so a channel held
out at 200 m accumulated "clean" epochs at anything under 1000 and was readmitted
*above the bound that had just refused it*. The bad readmission was visible in
a00 (32.60 → **37.54** → 34.23). An admission bound that lets a channel back in
above itself is not a bound.

Two-part fix:

1. the re-test uses the same bound that made the refusal —
   `fail || (vet_left > 0 && fail_vet)`;
2. **a withheld epoch no longer spends vet budget.** `vet_left` ticks down only on
   epochs the channel was ADMITTED. Otherwise a channel being held out serves its
   window in absentia and returns unvetted, which is the state INC-4 exists to
   abolish.

After the fix G22 is refused at 273450 and stays out; there is no heal, no
re-refusal, and a00 across 273600 reads 32.89 instead of 37.54. Counters move as
predicted: vet-bound refusals 4 → 3 (the spurious re-refusal gone),
`revet_window_ep` 784 → 753 (held epochs no longer counted), windows completed
74 → 71. Pinned by
`VetBoundRefusalHealsAgainstTheVetBoundNotTheOuterBound`; suite now **25 cases**.

#### The separation regime is NOT the parent card's

Stated plainly because §2's rhetoric does not transfer. `adm_bound_m` enjoys four
empty decades (§9.13: nothing between 54.3 m and 194 621 m). The **vet** bound
does not: healthy maximum 61.9 m, lowest refusal G14 at 218.5 m — a **3.5×**
separation, with the corrupt population extending down toward the threshold
rather than sitting four decades above it. G14 clears 200 m by only 1.09×. That
is a materially different risk posture and it is the main reason this knob should
not be promoted on two legs.

### 10.9 Status at hand-off

**Built and gated**: knobs + loader (with the INERT-guard and the
prefit-walk-absent warn), the cross-seam census flag, the seam verdict, eight
counters, two events (`adm_reject_reentry`, `adm_revet_refuse`), run-end block
with three invariant-#4 warns, five manifest fields (conditional on `adm_revet`
alone, so an unarmed-vet run's manifest is unchanged), and ten unit cases taking
the suite to 22 — including `DisarmedVetStillInheritsTheStaleVerdictAsAnOnset`,
which pins the DEFECT so the OFF path stays honest.

Unit: **1098/1099** Debug (the one pre-existing fixture skip), 172 suites, the
one-file-per-suite invariant intact.

**Verdict by class**, at the shipped defaults (`adm_revet_bound_scale` 0.2):

| | label-only (scale 1.0) | **shipped (200 m vet bound)** | `adm_revet_hold` |
|---|---|---|---|
| Class A (12 mislabels) | CLOSED — relabelled, response byte-identical, zero cost | **CLOSED** | closed |
| Class B (4 exposed) | PARTIAL — 2 of 3 pre-empted by 1 and 3 epochs; G16 ties | **CLOSED** — every cell refused at `resume+0`/`+1`; a00 flat on both legs | CLOSED |
| cost (channel-epochs withheld, rv04) | 144 (+2.9 %) | **153 (+9.3 %)** | — |
| cost (rv14, re-entries admitted) | 72 / 72 | **70 / 72** | 0 / 72 |

The shipped posture reaches the `adm_revet_hold` outcome at a small fraction of
its withholding, which retires `hold` as the recommended answer and leaves it as
a backstop for a run with no census AND a fault below the vet bound.

**Owed, in priority order:**

1. ~~The re-entry |z| measurement~~ — **DONE, §10.10**; ~~build
   `adm_revet_bound_scale`~~ — **DONE and gated, §10.11.**
2. **A 24 h arm at the shipped defaults against `runs/ft63_24h_conv`.** This is now
   the single highest-value owed run, and the reason is §10.11's separation
   result rather than the a00 result: the vet bound operates at **3.5×** over the
   healthy maximum (61.9 m vs G14@BSHM's 218.5 m), not the four empty decades
   `adm_bound_m` enjoys. Two legs of 272 epochs cannot bound a false-positive rate
   in that regime. The headline to read is the run-end split line — the control
   scored 0 re-entry refusals vs 17 onsets, of which 16 were re-entries in all but
   the label. A 24 h `adm_revet_hold` arm is NO LONGER worth its own run: §10.11
   reaches the same a00 outcome at a fraction of the withholding.
3. **A 24 h measure-only arm at `adm_bound_m` 50** (solve-inert, so it can ride
   along with anything) to extend §10.10's `n = 0` healthy-re-entry result from
   272 epochs to a full day. It is the direct evidence for the bound the previous
   item is really testing.
4. **σ̂₀ for gate 6.** The `sigma0` epoch-status column reads 0.0000 on every arm
   here, the same gap §9.14 records against gates 6/7. Until it has a source, no
   INC-4 arm can be QC-graded, and no promotion decision should be made on a00
   alone.
5. **The 17th onset.** G07@BSHM at TOW 329640 band 2 is the one mid-arc onset
   with no `arc_resume` within five minutes — it is the second band of a channel
   whose band 1 had already been refused. Benign on this reading, but it is the
   only member of the population this increment does not explain, and it should
   be confirmed rather than assumed.

No POSTURE row and no ledger entry were added, per §5's no-promotion rule.

### 10.10 The re-entry |z| measurement (§10.4's owed run): **DONE**

`runs/ft63_adm_bz14` / `bz04` — MEASURE-ONLY (`adm_screen` off) with
`adm_bound_m` walked down to **50 m**, on the same two snapshots as the gate-2
legs. 272 leg-epochs, 63 stations, 49 `arc_resume` cells. Re-entry attribution is
done offline against the `T0_gap arc_resume` stream — the same evidence §10.1
used — so the measurement stays independent of the mechanism it parameterizes.

**Reading a measure-only arm as though it were a screened one is legal, and it is
demonstrated rather than argued.** The statistic is state-free
(`d = observed − modeled`, and `modeled` carries range, product satellite clock
and the deterministic corrections with no filter state in it), so the same cell
must read the same z under any solve. G21@BSHM at TOW 306480:

| arm | event | z |
|---|---|---|
| `rv14off` (screened) | `adm_reject_birth` | **75334.778708** |
| `rv14lbl` (screened + vet) | `adm_reject_birth` | **75334.778708** |
| `rv14hold` (screened + strict hold — a materially different solve) | `adm_reject_birth` | **75334.778708** |
| `bz14` (measure-only) | `adm_census` | **75334.778708** |

Nine significant digits across four arms, one of which withholds 2.6× as many
channels. That is the state-freedom claim of §9.1 measured end-to-end, and it is
what makes this a cheap measurement rather than an expensive one.

#### The result: the healthy re-entry population does not reach 50 m

**Not one healthy station's re-entry cell cleared 50 m in either leg.** Every
>50 m re-entry cell is BSHM00ISR, and the picture is much sharper than expected —
**three of the five are corrupt at `resume+0`, i.e. on arrival**:

| cell | |z| at re-entry | lag | reached by |
|---|---|---|---|
| G16 band 1 (TOW 307740) | **670.3 m** | resume+0 | nothing today — this is the +19.2 TECU cell |
| G22 band 1 (TOW 273450) | **347.6 m** | resume+0 | census, at resume+5 |
| G13 band 1 (TOW 273240) | **81.5 m** | resume+0 | census, at resume+4 |
| G15 band 1 (TOW 274140) | 300 649.8 m | resume+0 | the 1000 m bound already |
| G21 band 1 (TOW 307680) | 62 496.4 m | resume+3 | the 1000 m bound already |

G16's 670.3 m is the number §10.4 extrapolated at "~715 m". The extrapolation was
right, and it is now a measurement.

Against that, the healthy tail — every non-BSHM cell over 50 m in either leg, all
of them steady-state, none at a re-entry:

```
61.9  COCO00AUS G24     55.2  NIUM00NIU G03     50.5  COCO00AUS G23
58.1  COCO00AUS G24     54.1  DARW00AUS G23     50.4  COCO00AUS G24
57.1  COCO00AUS G24     50.9  COCO00AUS G24     50.3  NIUM00NIU G03
55.7  COCO00AUS G24     50.7  NIUM00NIU G03     50.1  STHL00GBR G08
```

A coherent population — tropical / low-elevation stations, repeat offenders,
**topping out at 61.9 m**. The per-epoch `adm_sta_worst_z` column agrees: p50
13.6 / 11.7 m, p99 17.1 / 13.8 m across all 272 epochs.

#### Verdict

| vet bound | margin over healthy max (61.9 m) | measured false positives | class-B cells refused at `resume+0` |
|---|---|---|---|
| 100 m | 1.6× | **0** | G16, G22 |
| **200 m** | **3.2×** | **0** | **G16, G22** |
| 300 m | 4.8× | **0** | G16, G22 |
| 1000 m (today) | 16× | 0 | none |

**~200 m is the supported choice** and it closes the case that mattered: G16 at
670.3 m is refused *on its return epoch*, so the +19.2 TECU excursion and the
+7.1 TECU scar never start — the `adm_revet_hold` result of §10.8, without
withholding a single healthy re-entry.

**A "disjoint coverage" conclusion was drawn here and it is WRONG — see §10.11.**
The reasoning was: G13 arrives at only 81.5 m (1.3× the healthy maximum, so no
absolute bound can separate it) but ramps slowly enough for the census, while G16
arrives large but ramps too fast for the census — therefore the two mechanisms
cover disjoint failure shapes and neither subsumes the other.

The error is that it reads |z| **at `resume+0` only**. G13 is at 81.5 m on
arrival and at **308.5 m one epoch later**, so a vet bound that stays live across
the window — which is how it was built — catches it at `resume+1`, three epochs
*before* the census could. Measured in §10.11: with the vet bound armed the
census made **0 refusals out of 616 consultations**. On this data the bound
strictly dominates. The claim is retracted; what survives of it is only that the
census remains the sole cover for a cell that stays under the vet bound for its
entire window, which nothing in this sample does.

#### Caveats, stated

- The 50 m floor means the healthy re-entry distribution **below** 50 m is not
  observed. That is fine for setting a bound above 62 m and useless for setting
  one below it.
- The per-cell 20-epoch event holdoff can suppress a repeat exceedance (it hid
  G16's own 307800 row, 2 epochs after its 307740 one). A cell that was quiet
  before its gap and loud on return is always emitted, which is the case this
  measurement is about — but a *chronically* over-50 m cell that also re-enters
  could be undercounted. Those are not clean re-entries by construction.
- Two legs, 272 epochs, one day, one network. `n = 0` healthy re-entries over
  50 m is a strong result on this sample; it is not "never". A 24 h measure-only
  arm at the same 50 m bound would cost one run and settle it — and unlike the
  screened arms it is solve-inert, so it can ride along with anything.
