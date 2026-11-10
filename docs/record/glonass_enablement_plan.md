# GLONASS Enablement Plan — Network Producer + User PPP-RTK

**Written:** 2026-07-02, from a 23-agent tree-verified audit (every file:line below was
confirmed against the working tree by an independent adversarial verifier; the working tree has
extensive uncommitted changes — trust these anchors over git history).
⚠ **ANCHOR AGE (noted 2026-08-14): those anchors were verified against a 2026-07-02 tree and are
now six weeks and two decompositions old** (network INC-0..12 and the RealTimeEngine seam cut
both relocated large bodies of code). **Re-grep by SYMBOL before acting on any `file:line`
below** — the function names proved stable, the line numbers did not. The plan is still live:
its blocking card GLO-0.1 is open.
**Audience:** an Opus session executing one card per session, ordered by dependency, each with
explicit pass criteria. Two items are FABLE-tier (deep design work) — do not attempt them here.

**Goal:** GLONASS (FDMA) contributing *accurately* in (a) the PPP-RTK network producer
(`ProcessingMode::PPP_RTK_Network`) and (b) the user models (post-proc PPP, PPP-RTK BSSD, the
undifferenced closed-loop consumer, the RT engine) — float ambiguities only (FDMA: SD
ambiguities are non-integer without per-slot calibration), correct per-FCN wavelengths and iono
coefficients everywhere, per-FCN-slot receiver code IFB, GLONASS network clock consumed
closed-loop, and **zero degradation of GPS+GAL** (byte-identical off; A/B-validated on).

---

## 1. Ground truth (what already exists — do not rebuild)

The infrastructure is ~80 % built and mostly correct.

**FDMA plumbing (correct end-to-end):**
- FCN sources: RINEX header `GLONASS SLOT / FRQ #` → `RinexParser::glonass_freq_slots_`
  (RinexParser.cpp:566-587); RTCM 1020 `freq_channel` (RtcmNavDecoder.cpp:284-336); MSM7/MSM5
  DF419 (RtcmMsmDecoder.cpp:427-429, 523-525). RT re-seeds per 1020 frame via
  `RealTimeEngine::refreshGlonassSlots()` (RealTimeEngine.cpp:55-62).
- Per-satellite frequency/wavelength: `PppUtils::getFrequency/getWavelength(sig, sys, sat)`
  applies `GLONASS_G{1,2}_BASE_FREQ + k·CHANNEL_OFFSET` (PppUtils.cpp:187, 313;
  Constants.hpp:143-156). Iono-free/iono-α coefficients use per-sat frequency
  (PppModel.cpp:1085,1494; MeasurementModel.cpp:1308).
- **Ambiguities are stored in METERS** (StateManager.hpp:587,649) — wavelength enters only at AR
  candidate assembly, which uses per-sat λ (AmbiguityResolver.cpp:448-450, 990-992). This is why
  most of the pipeline is FDMA-safe by construction.
- All slip detectors use per-sat λ/f: MW (MwSlipDetector.cpp:347-361), GF (GfSlipDetector.cpp:125),
  GRAPHIC (GraphicSlipDetector.cpp:122), EWL (EwlSlipDetector.cpp:173), TurboEdit
  (TurboEditResolver.cpp:160). MW/TurboEdit correctly EXCLUDE GLONASS from the
  cross-constellation global sigma tier (MwSlipDetector.cpp:840, TurboEditResolver.cpp:287-288).
  SoftLliBridge hard-excludes GLONASS (`sat_id[0]=='R'` → false, SoftLliBridge.cpp:61-62).

**User BSSD layer (built, flag-gated, NOT yet data-validated):**
- Gate: `between_satellite_sd.enable_glonass` → `enableGlonassBssd()`, default **false**. It is
  read at TWO sites — MeasurementModel.cpp:245-247 and the pivot-candidate loop
  `PivotManager.cpp:41` (`if (obs.system == System::GLONASS && !config_.enableGlonassBssd())
  continue;`, re-grepped 2026-08-15) — and at both, false means **EXCLUDED**, not "float" (§5
  trap 1).
- Per-FCN-slot receiver code IFB: keys `GLONASS_IFB<band>_k<±slot>` (StateKeySchema.hpp:416-459),
  allocated in `StateInitializer::resolveReceiverBias` when
  `enable_glonass_ifb && estimate_ifb && uncombined`, FDMA bands 1/2 only, k=0 the held datum (no
  state; StateInitializer.cpp:118-131). Survives BSSD column zeroing (BssdTransform.cpp:261-263),
  keeps process noise under BSSD (StateManager.cpp:703), traced per epoch via
  `TraceLogger::logReceiverCodeBiases` (TraceLogger.cpp:340-365).

**Network producer (built, validated at 3 stations, BROKEN at 305 — see §3):**
- `network.estimate_glonass` (default false, GnssConfigLoad.cpp:2806-2807). When true: forces
  `estimate_ifb=true` + `enable_glonass_ifb=true` (:2840-2841); hard-errors unless GLONASS ∈
  active_systems (:2809-2816) AND signal_priorities has a GLONASS entry (:2826-2833) AND
  uncombined (:2835-2838); hard-errors if `estimate_sat_code_bias` is also on (:2853-2858, V1
  exclusivity — GLONASS admission skips the GPS/GAL receiver DCB that sat-code-bias estimation
  needs).
- Scope = **code-only**: per-sat GLONASS CLOCK emitted (registered like any sat,
  ApplicationNetwork.cpp:2238-2241; per-constellation clock pin incl. 'R', NetworkSbasis.cpp:29-54
  → datum dim 3 for G+E+R). NO satellite phase bias (MeasurementModel.cpp:1074-1076) and NO
  satellite code bias (:1096-1098) for GLONASS — deliberate FDMA physics, documented at
  GnssConfig.hpp:1202-1220. Per-slot receiver IFB is a Schur-eliminated local nuisance; loose
  zero-mean anchors (σ 1 m) on GLONASS per-slot IFB + GLONASS per-sat iono kill the intrinsic
  IFB↔iono↔ambiguity null (ApplicationNetwork.cpp:2443-2481). In network mode the receiver-bias
  alloc loop skips everything EXCEPT GLONASS per-slot IFB keys (MeasurementModel.cpp:771-772) —
  the load-bearing fix for GPS/GAL rank; do not touch it casually.
- GLONASS excluded from network AR (`if (cst == "R") continue;` ApplicationNetwork.cpp:4007) and
  phase biases truthfully marked `signal_integer=false` for 'R' (NetworkEstimator.cpp:372;
  test_network_estimator.cpp:240-264).
- Network QC (IF-phase TD slip detector, post-fit RAIM, per-sat clock monitor, 2-state clock
  model) is constellation-uniform with per-sat frequencies via PppUtils; no R-specific gates and
  none obviously needed. (§5 trap 10: the IF-phase detector's STAGED header banner lies — it is
  compiled and instantiated for the network path.)

**Product contract / consumer / RT:**
- Emitter/store/IO are constellation-agnostic ('R' via `systemFromSat`,
  NetworkProductEmitter.cpp:20; push APIs keyed by sat string). NetworkProductIO round-trips R
  records structurally — but has ZERO GLONASS tests.
- RT: MSM7 DF419 → `obs.glonass_freq_channel`; SSR 1063-1068 parsed and pushed system-agnostically
  (RealTimeEngine.cpp:370-374); 1020 → LiveEphemerisStore → refreshGlonassSlots. (§5 traps 6-7:
  MSM4 carries no FCN, and `obs.glonass_freq_channel` is write-only today.)
- Fixtures: `data/2024-129/` has 120+ global stations with GLONASS obs + FCN headers; COD CLK
  carries R01-R26 clocks; COD BIA carries R-sat C1P/C2P OSB — offline validation is fully
  possible. Configs in tree: `config/ppp_rtk_network_glonass.json` (3-station, validated), the
  `_net305_3sys_*` family (the failing scale case), `config/test_suite/manifest_glonass_check.json`
  (silent-drop A/B). ⚠ **Path correction (2026-08-14): of the 305-station 3-system family only
  `config/test_suite/_net305_3sys_smoke.json` is still under `config/test_suite/`** —
  `_net305_3sys_3h.json`, `_net305_3sys_fdma_1h.json`, `_net305_3sys_noeclexcl_1h.json` and
  `_net305_3sys_dualsingle_*` **moved to `config/archive/` on 2026-07-29**; the files are intact,
  only the path in this doc (and in GLO-N2) was stale.

**Memory to read before any card:** `project_glonass_ifb_network.md` (design + the 305-station
failure analysis), `project_realtime_glonass.md`, `project_multignss_signal_priorities_drop.md`,
`feedback_test_cadence_smoke_first.md`, `feedback_concurrent_build_locks.md`,
`feedback_wer_zombie_wedge.md`.

---

## 2. Locked design decisions (do not relitigate in a card)

1. **GLONASS is float-only, everywhere, in V1.** FDMA SD ambiguities are non-integer without
   per-slot phase calibration. No GLONASS integers in MLAMBDA, no GLONASS WL.
2. **Network GLONASS product = satellite clock only.** A shared per-(sat,signal) phase OSB is
   unphysical for FDMA (receiver group-delay-vs-frequency is per-receiver); satellite code OSB
   deferred to V2 (GLO-C1).
3. **Receiver FDMA group delay = per-FCN-slot code IFB state, k=0 datum**, FDMA bands G1/G2 only
   (G3/G4/G6 CDMA go through the ordinary per-band path later).
4. **Phase receiver IFB is absorbed by float ambiguities** (quasi-linear in FCN; no explicit
   state in V1).
5. **Slip repair for GLONASS stays per-sat-λ integer repair** — valid because ambiguities are
   meters and a slip is integer×λ_sat of that satellite's own wavelength. Do NOT convert to
   reset-on-slip preemptively; validate on data first (GLO-V2).
6. **GPS+GAL protection is non-negotiable:** every card keeps default-off configs byte-identical,
   and armed configs must not regress the proven GPS+GAL network (datum dim 2, stable 121/121) or
   user baselines.

---

## 3. The blocker you must respect: 305-station Galileo conditioning collapse

(Full forensics in memory `project_glonass_ifb_network.md` ⚠️ block — read it.)

Enabling `network.estimate_glonass` at 305-station scale destabilizes the **Galileo** clock block
~38 min in (tow 261510): clock-Δ rms steps 2.8 m → 7-13 km, all 21 Galileo clocks simultaneously,
observability gate stays green (`gate_ok=1`, declared dim 3). GPS and GLONASS itself stay clean;
the same network GPS+GAL-only is rock-stable through the same window. Established: NOT a
key/allocation bug (the :771-772 skip is complete), NOT eclipse-membership, NOT GLONASS band-3,
NOT the 1 ms gate. It is a **continuous conditioning-threshold crossing** — the GLONASS-forced
parameterization (extra per-slot IFB columns in the shared normals) leaves the Galileo clock block
marginally conditioned, and evolving geometry pushes it over a near-singular cliff the SVD gate is
blind to (gate tol catches machine-zero gauges, not σ~1e-6 near-nulls). The exact null vector is
UNPROVEN — instrumentation first, fix second (GLO-N1 → GLO-N2).

Until GLO-N2 passes, **network GLONASS is validated only at regional scale** (3-50 stations). Do
not ship a global GLONASS network config.

---

## 4. Cards

Execute in order within a phase; phases 0/1 and the 2-prefix instrumentation can interleave. One
card per session. Every card ends with: a full build **via `scripts/vs2026_build.ps1` (CLAUDE.md
§Build — this plan predates the VS-2026 toolchain migration of 2026-08-12; VS 2022 is RETIRED)**,
`gnss_tests.exe` green — ⚠ **the "419+ tests" bar written here in July is roughly a THIRD of the
current suite and would pass a badly regressed run; the current count is CLAUDE.md's 1360 cases /
196 suites / 89 files with one fixture-gated skip** — and the byte-identical check for default-off
configs where code paths are shared.

**Label note:** the design decision **D8** cited from `include/products/RinexNavParser.hpp`
("eph-anchoring is deferred per this plan / plan D8") **originates in
`docs/rt_service_conventions.md` §6 D8, not here** — this plan's cards are `GLO-*`. D8's GLONASS
ephemeris-anchoring work lands in **Phase 2 / GLO-N3** and the FCN-integrity card **GLO-0.4**.

### Phase 0 — Correctness foundations (user + shared)

**GLO-0.1 — signal_priorities: per-constellation defaults + fail-loud.** The silent-drop killer: a
constellation in `active_systems` with no `signal_priorities` entry contributes exactly nothing
(PppModel.cpp:456-457 `continue`), bit-identical to absent, no warning. The network loader fail-louds
(GnssConfigLoad.cpp:2826-2833); the user path does not. Add a per-constellation default priority map
populated when the JSON omits an entry for an active system, mirroring the default frequency-bands map
at GnssConfigLoad.cpp:~580 — seeds verified against JPLM 2024-129 attrs: GPS `"CWPQX"` (existing
behavior; derive GPS/GAL defaults from the shipped configs, don't invent), GLONASS `"CPQ"`, BeiDou
`"IPQX"`, Galileo `"CQX"`; defaults fill ONLY missing constellations, explicit JSON always wins.
Independently, WARN loudly at the PppModel.cpp:457 skip (once per constellation, not per epoch). Tests
extend the `test_glonass_network_config.cpp` pattern: GLONASS active with no priorities now processes
GLONASS (or at minimum warns); existing explicit-priority configs byte-identical.
**Pass:** `manifest_glonass_check.json` gr_nopri matches gr_withpri, or the run log carries the loud
warning if warn-only is chosen — defaults are the better fix per `feedback_prefer_fallback_over_drop`,
warn-only is the fallback if defaults regress a shipped config.

**GLO-0.2 — StateInitializer un-seeded PppUtils** (latent accuracy bug: wrong GLONASS frequency in
residual seeding and code-bias init). `StateInitializer` owns a private `ppp_utils_` never seeded with
the session's GLONASS slots; the per-slot IFB branch was routed through `PppModel::getGlonassChannel`
(seeded) as a spot fix, but `computeRawResidual` (~:175/211) and `initializeCodeBiases` still resolve
GLONASS frequency (→ iono-α) from the compiled default FCN table — wrong α for any satellite whose
real slot differs. Fix structurally (`feedback_correctness_over_expedient`): seed StateInitializer's
`ppp_utils_` once from the same source as PppModel/MeasurementModel (`setGlonassSlots` passthrough at
construction/reset in Application + ApplicationNetwork + RealTimeEngine), or route every frequency
query through the seeded owner — prefer the seed, it fixes all call sites in the class. Grep for other
unseeded instances: `grep -rn "PppUtils ppp_utils_\|PppUtils\s\+ppp_utils" include/ src/`
(MeasurementModel is seeded via setGlonassSlots — ApplicationNetwork.cpp:602; PppModel at
PppModel.cpp:308). **Pass:** unit test with a synthetic slot table where a sat's k differs from the
default table → residual/iono-α uses the seeded k; GPS/GAL byte-identical.

**GLO-0.3 — AR hard-guard: GLONASS can never enter integer fixing.** The only practical gates today
are `ar_active_systems` (empty default = ALL systems admitted, AmbiguityResolver.cpp:208-215 + header
:66-71) and `isAmbiguityUnresolvable`; a config that omits `ar_active_systems` feeds non-integer
GLONASS SD floats into MLAMBDA. Must be structural, and must cover BOTH the BSSD resolver and the
undifferenced consumer direct per-band path (AmbiguityResolver.cpp:~644): add an intrinsic
constellation gate in DD-pair formation skipping pairs whose satellite is GLONASS (System or 'R'
prefix) unless a future `ar_allow_glonass_experimental` flag (default false, undocumented in shipped
configs) is set, sited where pairs are built so both entry paths inherit it. Do NOT rely on marking
states unresolvable at allocation — that flag also drives BIA/ATX-coverage semantics and would
conflate causes. **Pass:** resolver unit test (GLONASS ambiguities present, empty `ar_active_systems`
→ zero R pairs, GPS/GAL unaffected); a closed-loop run with GLONASS active logs 0 R candidates.

**GLO-0.4 — FCN integrity hardening** (silent wrong-wavelength paths). Three verified holes:
1. `PppUtils::getGlonassChannel` falls back to the compiled `DEFAULT_GLONASS_CHANNELS` table silently
   when the seeded map lacks the sat (PppUtils.cpp:65-83 warns only at the -999 sentinel) → log
   once-per-sat at warn when the DEFAULT table answers.
2. Post-proc never cross-checks RINEX-header FCN vs broadcast 1020/DF419
   (`RinexObservation.glonass_freq_channel` is written by RTCM decoders, RtcmMsmDecoder.cpp:525, and
   NEVER read downstream — dead field) → add a once-per-sat-per-session check where observations meet
   the model: populated obs field ≠ seeded slot ⇒ error-level log + prefer the broadcast value via
   re-seed. Detection is the deliverable; live re-plumbing is not.
3. RT MSM4-only GLONASS: `honour_ext_sat_info=false` → FCN never decoded → `glonassWavelength` returns
   0.0 and breaks silently (RtcmMsmDecoder.cpp:115, 254-270) → resolve from LiveEphemerisStore's 1020
   channel map (LiveEphemerisStore.hpp:139-145); if still unknown, drop the observation WITH a
   rate-limited warning instead of emitting λ=0.
**Pass:** unit tests for each — default-table warn fires; mismatch detected+re-seeded; MSM4 GLONASS
with a prior 1020 produces correct λ, without 1020 drops loudly.

### Phase 1 — User PPP / PPP-RTK GLONASS armed and validated

**GLO-U1 — Config contract: one documented way to turn GLONASS on (user side).** The full incantation
today (all defaults false / missing):
```json
"processing": { "active_systems": ["GPS", "Galileo", "GLONASS"],
                "signal_priorities": { "GLONASS": "CPQ", ... } },
"estimation": { "estimate_ifb": true, "enable_glonass_ifb": true },
"between_satellite_sd": { "enabled": true, "enable_glonass": true }
```
plus uncombined mode. Two verified foot-guns to close: (a) `enable_glonass=true` (BSSD) with
`enable_glonass_ifb=false` puts un-modeled per-slot ICB (~meters) into SD rows silently — loader rule:
BSSD + GLONASS admitted + uncombined + `!enable_glonass_ifb` → loud warning, or force it on mirroring
the network loader at :2840-2841 (prefer force + info log, the established network pattern); (b) a
startup-summary line per active constellation with its effective gates (in BSSD? IFB on? in AR?) — the
audit found GLONASS can be silently absent at five distinct decision points. Ship
`config/ppp_rtk_glonass_user.json` (JPLM or CIT-class single station, 2024-129) as the reference armed
config — ⚠ **it does not exist yet; it is a DELIVERABLE of this card, not an existing reference** — and
document the contract in the `enableGlonassBssd()` Doxygen. **Pass:** armed config runs with R-sats
contributing (sat tally + residual trace); default configs byte-identical.

**GLO-U2 — Decision card: GLONASS satellite code OSB (.BIA) consumption.**
`GnssCorrections.cpp:1710-1714` skips ALL GLONASS code-bias BIA entries (`is_code && sat[0]=='R'`), on
the rationale (:1689-1697) that FDMA receiver-side delay can't be a single per-(sat,signal) value.
That conflates receiver-side IFB (true, handled by per-slot states) with **satellite-side** OSB, which
is a perfectly physical single value per (sat,signal) and is present in the bundled COD BIA (R01-R26
C1P/C2P). Consequence: GLONASS code rows either lean on the sat-bias state fallback (needs
`estimate_sat_biases=true`; watch the collinearity gate vs per-slot IFB at MeasurementModel.cpp:~929-933)
or carry the full satellite bias into iono/clock. Implement: allow BIA OSB application for GLONASS code
when per-slot IFB is active (receiver side then separately modeled); keep the skip when
`enable_glonass_ifb=false`; respect the existing sat-bias-fallback gating so we don't double-model.
A/B on real data (GLO-V1 config) over GLONASS code residuals, iono states, position — **keep only if
residuals tighten and position does not degrade**; if ambiguous, revert to skip and record the numbers
in the memory file. **Pass:** A/B table in the session summary; flag-off byte-identical.

**GLO-V1 — Armed user validation (the Phase-1 gate).** Staged cadence per
`feedback_test_cadence_smoke_first`: 10-min smoke → 1 h, JPLM 2024-129 (RINEX-3, GPS+GAL+GLO), both
plain PPP (float) and PPP-RTK BSSD armed configs. Pre-kill WER zombies; expect concurrent build locks.
A/B GPS+GAL vs GPS+GAL+GLO, recording 3D RMS + convergence time (expect the float-PPP pattern from the
manifest_glonass_check study: ~1-2 cm 3D improvement, minutes-faster convergence; PPP-RTK expectation:
no AR regression, R code residuals few-dm, R phase residuals mm-cm), per-slot IFB estimates from the
receiver-code-bias trace (stable, ~meters, zero-mean-ish across slots), no GPS/GAL residual inflation.
Inspect `logs/` traces across ALL epochs, not the tail. **Pass (record in memory):** GPS/GAL solution
statistically unchanged or better; R residuals bounded and slip-free arcs clean; per-slot IFB converged
and stable; no AR fix-rate regression. FAIL → stop and diagnose; do not proceed to Phase 3 with a sick
user model.

### Phase 2 — Network producer at scale

**GLO-N1 — Conditioning instrumentation (prerequisite; do not attempt the fix blind).** The
estimability one-shot fires only at epoch 1 (ApplicationNetwork.cpp:1424), so the tow 261510 collapse
is gate-silent. Add a per-epoch, per-constellation diagnostic on the reduced shared normals: smallest
singular value / condition number of each constellation's clock sub-block (and optionally the projected
null vector below a configurable trip), written to a new CSV alongside `network_solution.csv`, gated
behind `network.trace_conditioning` (default off, byte-identical off). An SVD of the shared block per
epoch at 305 stations is affordable relative to the 559 ms/epoch solve, but keep it
constellation-sub-block-only. Run `_net305_3sys` (GLONASS on) across 261420→261600; capture the Galileo
σ_min trajectory and the null vector at onset. **Pass:** the diagnostic NAMES the collapsing direction
(which states, which constellation) — a written verdict + memory update. EVIDENCE, not a fix.

**GLO-N2 — Conditioning fix (direction chosen by GLO-N1's evidence).** Candidates in preference order:
(a) per-constellation soft sum-to-zero gauge row on each clock block (small, product-safe — consensus
level shift only, same class as the existing GLONASS soft anchors); (b) hold eclipse-/membership-thinned
satellites' clocks with a decaying prior instead of letting the block drift toward the cliff;
(c) tighten the null-space gate to catch near-singularity (σ-ratio trip → withhold) — a safety net, not
a cure, but **do (c) regardless: the gate being blind to a 10-decade conditioning collapse is its own
defect.** Verify against the existing decisive A/B set: the `_net305_3sys_*` variants (fdma-only,
noeclexcl, gpsgal_ifb) + GPS+GAL-only control — **now under `config/archive/` except
`_net305_3sys_smoke.json`; see §1.** **Pass:** 305-station GPS+GAL+GLO 1-h run with Galileo clock-Δ rms
in its stable band (~2 m) through tow 261510 and the full hour; GPS+GAL-only byte-identical with the new
gauge off (flag-gate it — `network.clock_gauge_softsum` or similar, default off until proven, then
flipped in the GLONASS configs only).

**GLO-N3 — Staged network validation ladder + R clock quality.** With N2 in: 3-station (existing
validated fixture — regression: 121/121, datum dim 3, R03 pin, ~6 R clocks) → ~50-station regional →
305-station global, smoke → 1 h each. Add the GLONASS clock-quality check the 3-station run never had:
compare emitted R clocks datum-aligned (difference twice w.r.t. the pinned sat, then vs COD CLK R
records), per-sat rms — expect meter-level at 3 stations, sub-meter improving with station count. Also
record IF-TD slip detector R hit-rates, RAIM exclusion counts for R, and clock-monitor NIS per
constellation (R should not be an outlier class). **Pass:** all three rungs stable full-hour; R clock
rms documented; GPS/GAL clock rms unchanged vs the GPS+GAL-only control at every rung.

### Phase 3 — Product contract + closed-loop consumer + RT

**GLO-P1 — Product contract: R records first-class + IODE semantics + optional FCN.**
`test_network_product_io.cpp` has ZERO R coverage (makeEpoch is G01/G02) — add R records (clock-only
sat: no phase/code bias, exactly what the producer emits) to the round-trip + replay tests, and an R
clock record to `test_network_emit.cpp` incl. disc-counter behavior. FCN in the contract: biases are
meters and GLONASS is clock-only, so the consumer does NOT need FCN from the product in V1 — but a
consumer on a different FCN table than the producer is a silent frame risk for everything per-slot
later, so add an optional per-sat FCN annotation record (new tagged line, ignored by old readers), emit
it, and have consumers log a mismatch. IODE: `iodeMatches` returns true when either side is <0
(SsrCorrectionStore.hpp:326-329) and GLONASS broadcast iode is -1 ⇒ always-match, desync masked —
define the R contract as tb-derived IODE (1020 tb interval index, the RTCM SSR GLO convention) when
a-priori is broadcast-anchored, with post-proc precise-anchored records keeping -1 (a-priori identity is
carried by product basenames per the RT queue's RT0 cards — don't duplicate, don't conflict).
**Pass:** R round-trip byte-exact incl. replay disc counters; FCN record round-trips; mismatch warning
fires in a synthetic test.

**GLO-P2 — Closed-loop consumer with GLONASS (the Phase-3 gate).** The closed-loop harness
(ApplicationNetwork driver + `deriveClosedLoopConsumerConfig`) has never run a GLONASS-consuming user.
`deriveClosedLoopConsumerConfig` must carry the GLO-U1 user contract into the derived config when
GLONASS is active in the producer config and closed_loop wants it: `enable_glonass_ifb + estimate_ifb`
(+ `enable_glonass` under BSSD mode; undifferenced needs no BSSD flag). AR stays GPS(+GAL) — GLO-0.3
guarantees R never enters MLAMBDA regardless. The consumer consumes network R CLOCKS (+ precise orbits
as today); R phase rows see network clock quality directly (between-SAT differencing does NOT cancel a
per-sat clock error), so at 3-station product quality expect R weakly weighted or net-neutral — the A/B
is the point. A/B matrix (1 h, JPLM held-out or CIT1-successor RINEX-3 station): BASELINE (precise, no
store) G+E vs G+E+R; FULL/consume-clock G+E vs G+E+R; metrics position median/max, AR ratio + fix rate
(must not regress), R residuals. **Pass:** G+E+R consumer ≥ G+E on position, AR unchanged. Neutral
R-consuming at 3-station density is acceptable — the bar is "no harm + plumbing proven"; the accuracy
payoff scales with N2/N3.

**GLO-P3 — RT path: replay validation + seams tests.** `test_realtime_seams.cpp` has zero R coverage:
add a 1020-fed R-sat case (fit window 0.5 h path, RtcmNavDecoder → LiveEphemerisStore → PV) and an
MSM4-fallback case (GLO-0.4 item 3). SBF replay (data/sbf/covl1452) armed re-run as regression (R phase
residuals 1-4 mm per `project_realtime_glonass` — reproduce), plus an RTCM replay with SSR 1063-1068 if
a capture exists; verify R SSR records land in the store and apply (watch the GLO-P1 iode contract and
the RT queue's iode{0} fix — coordinate, don't collide). **Pass:** new tests green; replay metrics match
the recorded baselines.

### Phase 4 — Deferred / flagged (do NOT execute from this plan)

- **GLO-C1 (FABLE-tier):** GLONASS satellite code OSB as a network product + coexistence with
  `estimate_sat_code_bias` (requires re-deriving the receiver-bias parameterization that the V1
  exclusivity hard-error protects; the :771-772 skip and the two documented nulls are the constraint set).
- **GLO-C2 (FABLE-tier):** per-station linear-in-k phase IFB model → estimable GLONASS satellite phase
  bias → FDMA user AR. Research-grade (Banville-style); depends on GLO-C1 and dense networks.
- **GLO-C3:** per-constellation Allan clock-model Q for R sats (clock_model is currently
  constellation-uniform); revisit after N3 NIS statistics exist.
- **GLO-C4:** Smoother audit for per-slot IFB across BSSD pivot handovers (Smoother.cpp:137 intersects
  code_biases with no FDMA partition; handover frame prep :193-203 excludes only iono/sat-bias). Likely
  benign (per-slot IFB is pivot-invariant by construction — it survives SD untransformed) but unproven;
  write the test.
- **GLO-C5:** G3/G4/G6 CDMA bands through the ordinary per-band IFB path.

---

## 5. Traps appendix (verified; keep open while working)

| Trap | Anchor |
|---|---|
| `between_satellite_sd.enable_glonass=false` means GLONASS **EXCLUDED** under BSSD, not "float" | MeasurementModel.cpp:245-247 and PivotManager.cpp:41 |
| Missing `signal_priorities` entry = constellation silently inert (user path) | PppModel.cpp:456-457 |
| Empty `ar_active_systems` admits ALL systems to MLAMBDA (until GLO-0.3) | AmbiguityResolver.cpp:208-215 |
| `estimate_glonass` ⊕ `estimate_sat_code_bias` — hard error, V1 by design | GnssConfigLoad.cpp:2853-2858 |
| Network alloc-loop skip is load-bearing for GPS/GAL rank — touching it re-opens a ~40-dim null | MeasurementModel.cpp:771-772 |
| MSM4 GLONASS carries no FCN; λ silently 0.0 (until GLO-0.4) | RtcmMsmDecoder.cpp:115,254-270 |
| `obs.glonass_freq_channel` is write-only today — don't assume it's consumed | RtcmMsmDecoder.cpp:525 |
| GLONASS BIA code OSB skipped wholesale (until GLO-U2 decision) | GnssCorrections.cpp:1710-1714 |
| `iodeMatches` permissive on -1 → GLONASS clock desync masked | SsrCorrectionStore.hpp:326-329 |
| IfPhaseSlipDetector header banner says STAGED — it lies; it's compiled + instantiated for network | CMakeLists.txt:114, ApplicationNetwork.cpp:628 |
| 11-epoch smokes hide the 305-station collapse; always run the 1-h rung | memory: feedback_test_cadence_smoke_first |
| GLONASS-hostile comment "nominal per-band wavelength is satellite-independent" guards the network-AR export — keep R excluded there or fix the assumption | ApplicationNetwork.cpp:4007,4024-4027 |
| Concurrent sessions collide on MeasurementModel.cpp / ApplicationNetwork.cpp / GnssConfigLoad.cpp — re-read before editing, retry LNK1104 | memory: feedback_concurrent_build_locks |

## 6. Definition of done (whole plan)

1. Phase-0 cards merged; default configs byte-identical (hash-verified).
2. GLO-V1 recorded: user GPS+GAL+GLO ≥ GPS+GAL on real data, both PPP and PPP-RTK.
3. GLO-N2/N3 recorded: 305-station GPS+GAL+GLO stable full-hour, Galileo unharmed, R clocks
   quality-quantified.
4. GLO-P2 recorded: closed-loop consumer with R clocks, no AR/position regression.
5. All new toggles default-off documented in headers; startup summary lists constellation gates;
   memory files updated per card (results, not just "done").
