# MPAGES-BACKLOG: enhancement backlog harvested from NGS M-PAGES v1.1 and v1.2

**STATUS**

- **id:** MPAGES-BACKLOG
- **topic:** queues
- **status:** reference
- **summary:** Backlog harvested on 2026-06-24 from NGS M-PAGES v1.1 (v1.2 beside it): the landed items (default off), the open items with their proposed flags and the M-PAGES source anchors.
- **dates in the text:** 2026-06-24 to 2026-10-07
- **source:** agent memory note `project_mpages_enhancement_backlog`, copied 2026-10-24

**Outcome.** Ten items landed default off; the v1.2 items listed as queued (pivot election by coverage, field ocean tide loading, noon-turn attitude, wind-up through gaps, seasonal term, L5 R-1b) were built afterwards (cards PIV, OTL, NTA, WGB, SEA, L5B). The open items keep their proposed flag names and M-PAGES source anchors below. M-PAGES lives outside the repository (reference_software on the D: drive) and is never referenced from CMake.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: M-PAGES v1.1 enhancement backlog (landed items, open items, source anchors)

These results were recorded only in the agent memory note `project_mpages_enhancement_backlog` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: the v1.2 items (pivot by coverage, field OTL, noon turns, wind-up through gaps, seasonal term, L5 R-1b) are built (OPEN_ITEMS, Queued after NOM; cards PIV, OTL, NTA, WGB, SEA, L5B), so the statements below that noon turns are unmodelled, that the `computeBeta` sign is unchecked (validated in card NTA) and that periodic extrapolation is missing are stale. The SIF OTL path is gated by `model_corrections.otl_from_sif`, not only by `enable_station_info`.

Harvested 2026-06-24 from NGS M-PAGES v1.1, now at `D:/GnssProcessorSupport/reference_software/mpages/M-PAGES_v1.1`
(v1.2 beside it; moved off C: 2026-09-19; never reference it from CMake, vendor instead). Codebase notes:
memory note `reference_mpages_codebase`. Re-verified 2026-10-07: every landed flag resolves in `src/`, all
four open flags are absent. New flags follow the repo toggle convention, default false, OFF
byte-identical; state through StateManager/StateKeySchema; add a pure-physics test only where an
external reference vector exists.

## LANDED (default OFF: do not rebuild, do not flip blind)
#1 `ntrip_use_tls` / `ntrip_tls_insecure` (vcpkg libcurl is Schannel, no CA bundle; never tested
against a live CDDIS:443 caster) · #2 the network joint adjuster (now the network producer) · #3
`model_corrections.enable_ocean_pole_tide` + `files.opt_grid_path` · #4 `estimation.glonass_ifb.enable`
· #5 `receiver.enable_antenna_orientation` / `enable_antenna_substitution` · #8
`estimation.ambiguity_resolution.enable_partial_ar` · #9 `estimation.{enable_variance_factor,
enable_data_snooping, data_snooping_critical=3.29}` · #12 `estimation.enable_cio_frame` +
`receiver.{enable_itrf_velocity, reference_velocity_ecef, reference_position_epoch_year}` · #13
`estimation.enable_full_r_kernel` + `processing.enable_neq_export` · #14
`processing.{enable_trig_sp3_interp, enable_orbit_normal_attitude}`.

Gotchas from those builds:
- ⚠ `project(GNSSProcessor CXX C)` (CMakeLists.txt line 11 on 2026-10-07) must stay: SOFA
  (`third_party/sofa`, the one sanctioned glob) is ANSI C. Adding C to a configured CXX-only build
  dir fails ("cannot determine linker language for target: sofa"): delete `CMakeCache.txt`.
- #3: pole tide runs on `PppModel::gnss_corrections_`, not `ModelGeometry::corrections_` (OTL, solid
  tide); load the OPT grid in the PppModel ctor.
- #9 + #13A: `Sr = R - H*P+*H^T` feeds correlation-aware redundancy `Red = Sr*R^-1`, dof = tr(Red).
  The variance factor rescales only a REPORTING sigma_pos; the propagated P is untouched.
- #13B NEQ: `x = N^-1*u` is the Gauss-Newton INCREMENT about per-epoch operating points (absolute =
  final_state + x); epoch-local states Schur-eliminated, AMB labels arc-stamped; ZWD/TGRAD are
  approximated session-constant (exact only for short static).
- #14 is UNVALIDATED with the flag ON: the sawtooth benefit at 15-min SP3 boundaries is unmeasured
  and the `computeBeta` sign is unchecked against any external attitude product.
  M-PAGES uses raw signed beta thresholds (BDS >= 4 deg, QZSS >= 20 deg). Its uniform per-satellite
  trig-fit weight cancels in the normals: do not add a per-record-SD weighting path.
- #8 partial AR: greedy backward elimination in ORIGINAL DD space, each subset re-validated by the
  full gate battery, so a wrong order costs fixes, never integrity.

## OPEN (each P1, medium size)
- #6 startup bias-source consistency: a GIM vs .BIA analysis-center or ABSOLUTE/RELATIVE mismatch is a
  silent metre-level code error; today only a log line advises a same-AC GIM (`Application.cpp`). `ProductParser::parseBia` drops the SINEX AC and BIAS_MODE;
  `IonexParser` never reads the AC. Plan: AC into `IonexData` + a `BiasMetadata` POD
  (`ProductTypes.hpp`, carried through `ProductCache`), validate once in `loadInputData` behind
  `processing.enable_bias_consistency_check` (GIM active, not RealTime), normalize COD/CODE, warn by
  default (optional `bias_consistency_hard_fail`). M-PAGES `Sinex.cpp:176`, `BiasSinex.cpp:154-202, 370-413`.
- #7 measurement-domain clock-jump / time-tag repair. ⚠ 2026-10-08: NO TARGET on the network path,
  recommend closing it there: a DOY-129 scan found ms jumps on 31 of 311 stations (10 golden roster
  stations, ~310/day), every one consistent in code and phase, none code-only, no off-grid tags; the
  network receiver clock is a free per-epoch column that absorbs a consistent jump exactly, and
  RepairClockJumps itself repairs only code-only jumps. Still relevant for the user side only.
  Corrected: `Application::handleWlsSeeding` re-seeds at 50 us (`kClockJumpThreshSec`), user-side
  only; the network never calls it. Original plan: raw obs are never corrected (no integer-ms phase-minus-range repair, no
  non-integer time-tag fix). New `slip/ClockJumpRepair` with `repairEpoch(vector<RinexObservation>&,
  GnssTime&)`, called after `collectEpochObservations` and before `gnss_rate_s` decimation and WLS;
  guard RealTime; interval >= 1 s. Re-align phase to code, never touch code (it carries the clock).
  Flags `processing.enable_clock_jump_repair` (+ `estimation.enable_clock_jump_poly_gapfit`). M-PAGES
  `RepairClockJumps.cpp` (hasJump: median pmr-diff / (c*0.001), integer-ms + MAD; checkForObsJumps,
  checkTimeTag, repairJumps), `DbEdit.cpp` fitGap/simeq for the optional gap-fit.
- #10 exact differenced covariance OP*R*OP^T: BSSD (`BssdTransform.cpp`) and baseline DD
  (`MeasurementModel.cpp`) assume diagonal undifferenced R, dropping correlated R (shared atmosphere
  pre-correction, correlated multipath); the KF already takes dense R. One kernel
  `differencedCovariance(posneg, R_undiff)` = R(p_i,p_j) - R(p_i,n_j) - R(n_i,p_j) + R(n_i,n_j), used
  by both behind `estimation.enable_exact_dd_covariance`; a second
  flag `enable_batch_pivot_schedule` ports `ArcInfo.cpp` getReferenceSatellites/reviseArcs as a
  post-processing precompute feeding PivotManager (which stays the authority).
- #11 RINEX-met + a-priori ZWD: no met parser; `Troposphere` has ZHD only and the wet state seeds flat
  (~1-5 cm height transient, up to 10-20 cm humid cold start). Add `RinexMetParser` + Saastamoinen
  ZWD = 0.0022768*((1255/Tk)+0.05)*e/phi into the wet SEED MEAN only (no new StateKeySchema key; M-PAGES `Tropo.cpp` wetsaas/phiFun;
  guard the file load vs RealTime); flags `estimation.enable_rinex_met`
  (+ path), optional `enable_operational_vmf`. Blocked in practice by memory note `project_vmf3_gpt3_pathbug`.

## Geodesy-physics gaps (relativity audited CLEAN)
- Post-seismic deformation: the fixed a priori is linear r0 + v*dt only (5-50 mm in an active
  relaxation window); the SIF path carries PSD (memory note `project_sif_integration`). ⚠ Not sub-mm on the
  field roster: MIZU00JPN sat 1.6 m from its data under linear ITRF2020 propagation with no
  post-seismic model (card SCU 2026-09-22, attribution not verified numerically; station dropped;
  census truth coordinates come from the same resolver, ANMG00MYS the next suspect).
- OTL: 11 major tides only (hard `j < 11` loops, `OceanTideModel.cpp`); M-PAGES splines ~342 minors
  (~1-2 mm vertical RMS, 3-5 mm coastal). The EOT20 turnkey path is radial-only (horizontals zeroed,
  ~1-2 mm). OTL is OFF on the refound goldens (2026-10-07). 2026-10-08: the 3-component SIF FES2014b
  path exists (`ModelGeometry.cpp:156-188`) but is gated on `processing.enable_station_info` (not
  armed; it also overrides the initial position in `Application.cpp:417` and the ocean pole tide
  source in `PppModel.cpp:271`); 50 of 58 roster stations have SIF OTL, roster vertical RMS median
  9.7 mm, 24 above 10 mm (MAJU 31.5, HOLB 28.1, PALM 22.9); census sites KOKB00USA 17.5, POHN00FSM
  21.6 mm. No external-oracle OTL test; validate against M-PAGES `Lib/IersHardisp` before arming
  (field only; the sim fixture has no OTL). Periodic station motion: our resolver drops the seasonal
  term past a station's last SIF segment (`StationInfoFile.cpp:479-492`; 8 roster stations, CHUR00CAN
  6.4 mm) while M-PAGES v1.2 extrapolates it (`Periodic.cpp:148-200`).
- Solid-tide Step-2 diurnal EAST term has the PRE-2007 `+sin` sign (`GnssCorrections.cpp`,
  `de += (r[7]*ct + r[8]*st) * sPhi`, line 1671 on 2026-10-07; IERS post-2007 is `-sin`, inherited
  from POINT). Under 0.1 mm east-only; the dehanttideinel test passes with it. One-character fix.
- JPL DE ephemeris: analytical `Planetary::sunMoonPosEci` only, under 0.1 mm. Do NOT prioritize; if
  ever wanted, use vendored SOFA `iauEpv00` / `iauPlan94`.
- Shared blind spots: S1/S2 atmospheric tide loading is implemented (`enable_atmospheric_loading`)
  with PROVISIONAL constants (pin to a real Ray and Ponte CM grid first); non-tidal APL (~2-10 mm,
  to ~20 mm synoptic, vertical), the real grid and its fetch deferred. S1/S2 is sun-synchronous and
  does NOT average out in static daily PPP (it aliases into height and ZWD). Periodic station motion
  (~4-15 mm vertical) lands as a fixed Up offset at a seasonal extremum.

## cm-level sleepers (outrank every mm item)
1. Satellite PCO/PCV vs clock/SSR datum vs attitude frame (z-PCO vs IF-clock datum). TOP follow-up.
2. Eclipse yaw, PARTLY audited 2026-10-08: umbral exclusion armed on both goldens; sunlit NOON TURNS
   unmodelled (`SatAttitude.cpp:45-51` always YawSteering; the wind-up uses its own inline yaw copy,
   `GnssCorrections.cpp:1920-1960`, not SatAttitude). DOY 129: IIF G25 (beta -0.7) and G26 (+2.4) in
   the noon-turn regime, IIF x-PCO 394 mm (up to ~13 cm station-dependent LOS change), noons 08:40/20:40
   and 11:20/23:20 UT: zero effect on 00-06 UT goldens, live in 24 h / RT / MDR late windows; Galileo
   FOC noon turns fall inside 00-06 UT (card OWD O7, owed before Galileo adoption). M-PAGES has nominal
   yaw only. (The wind-up SIGN defect was fixed 2026-08-21, `4f49fa2`; M-PAGES fixed the same mirror
   defect in v1.2, memory note `project_phase_windup_sign_defect`.)

## 2026-10-08 v1.2 study (network producer), ranked
Few new techniques (v1.2 moved toward our held-basis datum); value is where SD cancellation hides a
term that lands in our undifferenced satellite products. (1) L5R R-1b: estimate the receiver band-5
code delay (M-PAGES keeps a constant per non-reference obsCode); the drop's comment
(`ApplicationNetworkAssembly.cpp:145-167`) is GIM-era; declare the datum against B5D's sum-to-zero.
Before R-2: GPS IIF L1/L2-vs-L5 inter-frequency clock variation (decimetre, periodic, literature) vs
`sat_phase_bias_q` 1e-12 and a sim fixture with none (AB2:61). (2) Clock pivot by coverage/health/
shadow (v1.2 `getSegmentDatum` rule) vs lexical `electClockPivot` (G01: unhealthy, fewest stations,
eclipse handover; PAC). (3) OTL on the field (above). (4) Noon-turn attitude, one model for PCO and
wind-up. (5) Measure the (1,1) MW-blind slip on the float vehicle (existing injection ladder). Small:
wind-up bridged through gaps (no 300 s restart), periodic extrapolation, close #7 on the network,
the RF-2 run-end line logs ERROR on every golden (138 cold-release erases vs a pre-RF-2d "must be 0").
3. Loading frame CF vs CM: OTL and APL must share one frame (CM); check before non-tidal APL.
