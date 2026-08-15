# HANDOFF — W6 code-bias admission census + station-local quarantine ("CB-ADM")

**Status:** INC-1 + INC-2 + phase co-gate IMPLEMENTED, default-OFF, uncommitted
(for user review).  Implements `docs/SPEC_cb_admission_card.md`.  Dates: built
2026-07-21; validated + diagnosed 2026-07-22.

**TL;DR for the reviewer.**  The census + station-local quarantine + per-epoch
guard + the phase co-gate all work and are default-OFF / byte-identical OFF /
unit-tested (12 tests, suite green).  BUT the card's efficacy verdict is
NEGATIVE-then-PARTIAL on the real 305 BeiDou storm: code-only quarantine ALONE
*worsens* the storm (22→43 near-null dirs) by orphaning same-signal phase biases;
the phase co-gate (withhold the quarantined signal's phase too, at state
allocation) fixes that orphan class (43→33) but a deeper **weak-but-real BeiDou
datum layer** (~1e-3 families the single rank-gate cutoff miscounts) still
withholds every epoch.  The BeiDou W6 storm is bigger than this one card — it
needs a datum/gate follow-up (§3).  The co-gate + census are correct keepers; the
verdict here is a diagnosis, not a shipped fix.

---

## 1. What was built

A run-latched startup census replaces the per-epoch instantaneous ≥2-station
admission gate on W6 satellite code-bias (OSB) columns.  Each (sat,signal) code
pair is classified ONCE, immutable for the run, into:

- **ADMITTED** — a shared border column, datumed + emitted (as today).
- **QUARANTINED** — measurements kept, per-station sat-bias state kept and never
  pruned, never registered ⇒ a station-local sponge (Schur-eliminated
  station-side; never enters the shared normals / spectrum / rank gate).

Default class is QUARANTINED.  Default-OFF (`network.cb_admission=false`); the
OFF path is byte-identical (every change is flag-guarded; the registration OFF
branch is the verbatim legacy logic re-scoped).

### Files (the entire CB-ADM footprint)

| File | Change |
|---|---|
| `include/filter/NetworkBiasAdmission.hpp` | NEW — POD result + `runCensus` static (pure) |
| `src/core/filter/NetworkBiasAdmission.cpp` | NEW — the census scan |
| `tests/test_network_bias_admission.cpp` | NEW — 12 unit tests |
| `include/config/GnssConfig.hpp` | 3 getters + 3 members; fixed stale "GPS+Galileo" W6 doc |
| `src/core/config/GnssConfigLoadNetwork.cpp` | parse + validate + ON-log + INERT-warn; 2 stale-doc fixes |
| `include/app/NetworkRunContext.hpp` | `admission` member + 3 counters |
| `src/core/app/ApplicationNetwork.cpp` | pre-loop census call site + table log + run-end telemetry + fixed zero-epoch HINT |
| `src/core/app/ApplicationNetworkAssembly.cpp` | registration-site swap (OFF verbatim / ON latched) |
| `CMakeLists.txt` | +NetworkBiasAdmission.cpp (gnss_core), +test (gnss_tests) |

### Config keys (section `network`)

```
cb_admission           : bool, default false   (master enable)
cb_admission_min_stations   : int, default 4
cb_admission_census_epochs  : int, default 15
```

### Census algorithm (`NetworkBiasAdmission::runCensus`)

Pure scan over `[i0, i0+K)` of the synced-epoch vector (`i0` = first epoch the
loop processes, post `start_offset`).  Per (sat,signal) raw-code pair:

- **tier 1 (count):** a station *supports* the pair iff it supplies a usable raw
  code obs (3-char, `C`, finite nonzero) in ≥ ⌈K_eff/2⌉ census epochs; admitted-
  candidate iff #supporting ≥ `effective_min = max(2, min(min_stations, n_census_sta))`.
- **tier 2 (IF-pin):** per satellite, its tier-1 candidates must span ≥ 2
  distinct carrier bands (`classifySignalForSat().band`) — else ALL demoted to
  QUARANTINED (the datum-orphan class killed at the source).

GLONASS satellites excluded (no OSB scope).  Deterministic (std::map order).

---

## 2. Gate results

### INC-1/INC-2 unit + suite (Debug)

- 12 new `NetworkBiasAdmissionTest` cases green (sustained-support floor,
  effective_min clamp incl. 3-station roster, tier-2 demotion, same-band
  co-channel non-distinctness, late-seen default, K_eff clamp, GLONASS/phase/
  zero exclusion, i0 offset, determinism, empty-window inactive, registration-
  swap contract).
- Full suite **839 / 838 passed / 1 skipped** (pre-card baseline 827/826/1 +
  12 new; ZERO regressions).
- `gnss_core` + `gnss_tests` build clean.

### Census wiring (50-sta smoke, Release) — MECHANISM CONFIRMED

`_cbadm_50sta_15ep_gpsgalbds.json` (first 50 of the 305 roster, GPS+Gal+BDS,
full carry/iono-SH stack), `cb_admission` ON:

- Census fired: **939 (sat,signal) pairs seen, 734 admitted, 205 quarantined**
  (K_eff=15 × 50 stations, effective_min=4, sustained_floor=8).
- Per-constellation: BeiDou 355 pairs → 250 admitted / 105 low-count;
  Galileo 269 → 245/24; GPS 228 → 189/39; QZSS 56 → 36/20; SBAS 31 → 14/10 + **7
  tier-2 no-IF-pin demotions**.
- **Anti-inert PASS** (205 quarantined > 0).
- **`n_cb_late_quarantined = 0`** — census keys byte-match the sat-bias state
  keys exactly (no format mismatch; the single-digit "C 1" space-padding is
  consistent across the raw scan and the state keys).
- Admitted MEO BeiDou (C19–C48) registered as shared columns (`CB|C19`…`CB|C48`
  in the estimability dump); quarantined single-digit GEO/IGSO BeiDou
  (C01–C07) did NOT register.  ⇒ the swap is correct.

### 50-sta efficacy A/B (control = legacy gate vs CB-ADM ON) — CAVEAT

Both arms withheld all 6 dumped epochs (0 emitted).  Below-cutoff near-null
directions, per-epoch average, classified by dominant families:

| class | control | CB-ADM ON |
|---|---|---|
| pure_CB | 2.8 | 2.5 |
| mixed_CB↔PB | 11.5 | 19.5 |
| pure_PB | 1.0 | 5.0 |
| pure_CLK | 3.0 | 2.8 |

At this **sparse 50-station GLOBAL scatter** (default `min_stations=4`), CB-ADM
did NOT clear the storm and INCREASED the mixed CB↔PB near-null count.  Direct
evidence: CB-ADM registered CB| columns for **24 BeiDou sats vs the legacy
gate's 15** — 9 more sparse sats that the legacy per-epoch gate pruned.  Root
cause: an admitted sat (sustained-window support ≥ 4 stations) can still be
tracked by only ONE station in a GIVEN epoch; the anti-flap design (SPEC §2:
register at ≥1 present station) registers it anyway, and the code datum cannot
pin the resulting single-station CB↔PB↔CLK confounding (fundamental — a
1-station code column is inseparable from that sat's clock).

This is exactly the "never validate on global geometry / dense data first"
geometry the standing directives warn against — the 50-sta config with
`min_stations=4` admits borderline BeiDou that never have 2 simultaneous
stations.  Levers under evaluation: (a) raise `min_stations` so only
well-covered BeiDou are admitted (a tuning knob, no design change); (b) a
per-epoch ≥2-station registration guard on the latched admitted set (a §2
deviation — separates run-level eligibility from per-epoch registration
coverage, mirroring `phase_datum_min_stations`).  The SPEC's proof config is the
DENSE 305-station storm repro, now too slow for this dev cycle (≤10 ep cap).

### 50-sta min_stations sweep (10-ep) — the config is NOT a valid efficacy test

Raising `min_stations` 4 → 10 (285 admitted / 654 quarantined) DID drop the
pure-CB share (2.8 → 1.0/epoch) but **exploded the phase side**: ev<cutoff went
**19 (control) → 540 (min=10)** per epoch, dominated by ~476 pure-PB
directions.  Cause: the W6 satellite CODE bias is LOAD-BEARING for the
phase↔clock split — it anchors the sat clock so the phase bias can separate.
Quarantining a sat's code bias while its phase bias + clock stay shared columns
breaks that separation.  At the default min=4 this is mostly benign (pure_PB ≈ 5;
the quarantined sats are truly sparse and their phase is also gated), but the
sweep proves the 50-station GLOBAL scatter is degenerate for BeiDou and cannot
demonstrate efficacy either way.

### 305-sta storm A/B (the SPEC proof config) — NOT RUN (too slow this cycle)

The 305-station run was cancelled by the user as too slow (~3 min/epoch × 305
sta).  Fast-dev cap is ≤10 epochs; the storm is a LARGE-sparse-network
phenomenon that cannot be faithfully reproduced at small station counts (a small
dense region has no storm; a small global scatter is degenerate).  Efficacy
validation therefore remains OPEN pending a dense-network run.  The A/B configs
are staged: `_maxnet_305_gpsgalbds_d15_5ep_{ctl,cbadm}.json`.

### Conclusion of the fast-config validation

CB-ADM is correctly implemented and the census MECHANISM is fully confirmed, but
its EFFICACY (clearing the storm) is neither demonstrated nor refuted on fast
configs — the only geometry that reproduces the storm is a dense large network.
Two structural facts constrain the card as specified:

1. CB-ADM gates ONLY code-bias columns.  Satellite CLOCKS (registered for every
   sat with a usable obs — no coverage gate) and PHASE BIASES
   (`phase_datum_min_stations`) are untouched, so a sparse BeiDou's CLK|/PB|
   near-null directions survive CB-ADM (PB| is explicitly out of scope, §4; CLK|
   has no gate at all).
2. The code bias is load-bearing for the phase↔clock split, so quarantine must
   stay ALIGNED with per-epoch phase-bias registration (the default min=4 keeps
   it aligned; aggressive thresholds do not).

---

### 120-sta 10-ep A/B, WITH the phase heal armed — the storm is not CB-ADM's

Prior configs had the phase-side machinery OFF (`phase_datum_min_stations=1`,
`phase_datum_prioritized` unset ⇒ `healNearNullPhaseBias` early-outs).  Arming it
(`phase_datum_prioritized=true`, `phase_datum_min_stations=2`) on a 120-sta
10-ep A/B still withholds all 11 epochs in BOTH arms:

| below-cutoff / epoch | legacy + heal | CB-ADM + heal |
|---|---|---|
| ev<cutoff (declared=3) | 23 | 26 |
| pure_CB | 3.2 | 5.5 |
| mixed_CB↔PB | 16.3 | 16.7 |
| pure_PB | 0.0 | 3.0 |
| pure_CLK (= the 3 gauges) | 3.0 | 3.0 |

Findings:
- The `CLK|` near-nulls are per-constellation COMMON-MODE = the 3 DECLARED clock
  gauges (`CLK|E…` , `CLK|C…` , `CLK|G…`, equal weights) — EXPECTED, not the
  storm.  **A satellite-CLOCK coverage gate is misdirected** — the clocks are
  already gauged; there is no single-station `CLK|` near-null (grep found none).
- The storm is **mixed CB↔PB ≈ 16/epoch, present in BOTH arms** — these are
  ≥2-station satellites (registered by the legacy per-epoch gate too), whose
  code↔phase are confounded for reasons UNRELATED to the sparse-code-bias leak.
  The phase heal cannot fix the `pure_CB` share (it only anchors PB columns).
- CB-ADM is marginally WORSE here (26 vs 23) — its window-latched admission
  registers a few more per-epoch-degenerate columns than the legacy per-epoch
  gate.  The truly-sparse BeiDou CB-ADM quarantines are ALSO excluded by the
  legacy gate, so the two are near-equivalent on these configs.
- **Fast global subsets test the WRONG storm.**  50/120-sta are subsets of the
  305 GLOBAL roster ⇒ ALL constellations are sparse ⇒ a generic
  under-determination CB-ADM does not target.  The real BeiDou W6 storm needs
  GPS/Gal DENSE and only BeiDou sparse — i.e. a large network (the 305 config).
  That cannot be shrunk to a fast config without losing the storm or the
  density.  Efficacy is therefore only testable on the 305 (too slow this cycle).

### 305 CONTROL storm characterization (the definitive test) — VALIDATES the premise

One 305 control arm (`_maxnet_305_gpsgalbds_d15_5ep_ctl.json`, legacy gate,
`trace_estimability`), reproduced the known Jul-17 storm: 0/8 epochs, 22
ev<cutoff every epoch (declared_dim=3).  Characterized the 135 below-cutoff
directions across dumps (`scripts`-style analyzer, `characterize_storm.py`):

- **Constellation: BeiDou 123 / 135 directions** (Galileo 6, GPS 6).  The 305
  config ISOLATES the BeiDou storm (GPS/Gal dense) — the fast global subsets did
  NOT (all-constellation under-determination).  This is why fast configs were
  the wrong test.
- **Top culprit satellites** (appearances in below-cutoff dirs): C06 (71),
  C13 (68), C29 (53), C16 (49), C34 (43), C36 (32), C38 (31)… — dominated by
  the BeiDou GEO/IGSO (C06/C13/C16), the regionally-tracked birds a GLOBAL
  network sees from few stations = exactly what CB-ADM quarantines.
- **Family signature:** CB+PB 98, pure-CLK 16 (the gauges), CB+CLK+PB 13,
  pure-CB 7, CLK+PB 1.  **118 / 135 directions involve a `CB|` component** — so
  CB-ADM's code-bias quarantine directly targets the storm.

Conclusion: CB-ADM addresses the `CB|` share (necessary), but the storm is
mostly CB↔PB mixed, so clearing it to emit ALSO needs (a) the phase coverage
gate `phase_datum_min_stations` ≥ 2 (EXISTS; off in the maxnet config) to gate
the same sparse BeiDou's phase biases, and (b) a satellite-level / clock gate for
the ~13 single-station CB+CLK+PB triples (the clock IS in those near-null
directions — the one place a clock gate applies).  The 16 pure-`CLK|` remain the
expected declared gauges.

### 305 CB-ADM arm (min=4) vs control — CB-ADM ALONE IS COUNTERPRODUCTIVE

Same 305 config + `cb_admission` (census: 903 admitted / 134 quarantined / 14
late).  Result: **22 → 43 ev<cutoff per epoch** (near-null directions across
dumps 135 → 258).  Full family delta:

| directions | control | CB-ADM |
|---|---|---|
| pure CB | 7 | **0** |
| CB+CLK+PB | 13 | **0** |
| CB+PB | 98 | 151 |
| pure PB | **0** | **89** |
| pure CLK (gauges) | 16 | 18 |
| total | 135 | 258 |

CB-ADM did its job on the code side (eliminated every pure-`CB|` and
`CB+CLK+PB` direction — the sparse-BeiDou code share).  But it CREATED 89 pure-
`PB|` directions from zero.  Root cause (measured, consistent with the 50-sta
min=10 sweep): **the W6 satellite code bias is load-bearing for the phase↔clock
split** — it anchors the sat clock so the phase bias can separate.  Quarantining
a sat's code bias while its phase bias stays a shared column (registered by the
phase datum) breaks that separation → the code-null becomes a phase-null.

**This refutes SPEC §4's premise** that the phase side is independent and PB|
"just survives".  CB-ADM's code-only quarantine ACTIVELY DESTABILIZES the phase
side.  The code bias cannot be quarantined in isolation.

**Implied fix (needs a ruling):** co-gate — remove the SAME sparse satellites
from both code and phase, i.e. set `phase_datum_min_stations` =
`cb_admission_min_stations` (both 4), so a quarantined sat's phase bias is also
withheld and there is no orphaned phase↔clock split.  (The clock stays — the
clock common-mode is gauged; a single sat's clock rides the constellation
gauge.)  Confirmation run pending user go-ahead.

### 305 CB-ADM + per-epoch ≥2 guard + phase co-gate — NO-OP; the storm is a datum coupling

Implemented the principled fix (register an admitted code pair only when ≥2
present stations hold it THIS epoch; co-gate phase to code-eligible sats).
Re-ran the 305 arm: **byte-identical to the un-guarded CB-ADM run** (43
ev<cutoff, same 258 dirs, same culprits C16=61/C13=60/C06=60, census 903/134/14,
n_cb_late_quarantined=14).  The fix never triggered because:

- At 305 density EVERY admitted satellite already has ≥2 present stations, so the
  per-epoch guard is a no-op.
- Quarantine is per-(sat,SIGNAL); almost every quarantined sat still has one
  admitted signal ⇒ code-eligible ⇒ the phase co-gate withholds nothing.

So the storm is NOT a per-epoch coverage problem.  Distinct near-null `PB|`
satellites: **control 20 → CB-ADM 35** (+15 ADMITTED, well-covered BeiDou).  The
`+89 pure-PB` come from removing the SPARSE satellites' code biases from the
shared **s2z code-bias datum** (sum-to-zero per constellation-signal), which
destabilizes the code↔phase↔clock separation of the REMAINING admitted sats.
This is a datum-coupling effect that coverage gates cannot fix.

**Bottom line on efficacy:** CB-ADM's code-bias quarantine, as specified,
WORSENS the real 305 BeiDou storm (22→43 near-null dirs), and the coverage
refinement does not help.  The card correctly removes the pure-`CB|` share but
the net is negative because quarantine perturbs the s2z datum onto the admitted
sats' phases.  The BeiDou storm's real driver is the code↔phase↔clock
confounding of the GEO/IGSO birds (C06/C13/C16), which is a DATUM problem, not a
sparse-code-bias leak.  This is a fundamental finding about the card's premise —
reserved to the user (§8) before any further redesign.

### ROOT CAUSE (deep-dive, high confidence) + the correct fix location

A 5-agent adversarial deep-dive (buildCodeBiasDatumRows / rank gate / code↔phase
coupling / GIM-iono / phase-election) converged, and the smoking-gun evidence is
decisive:
- CONTROL: `ev=2.19e-11 [PB|C29|L2X=0.65, CB|C29|C2X=0.60]` — code+phase COUPLED lump.
- CB-ADM: `ev=6.75e-11 [PB|C29|L2X=0.99]` — code quarantined ⇒ PURE-PB orphan.

The pure-`PB|` signals (L2X/L7X/L1I) are the exact phase counterparts of the
quarantined sparse code pairs (C2X/C7X/C1I), each tracked by ~2 stations.  β_g^s
(phase bias) on such a signal is observable ONLY via its same-signal code
companion b_g^s through the shared clock; quarantining the code collapses the
mixed lump onto β_g^s alone → an isolated pure-PB gauge (ev~1e-11).  It is NOT an
s2z/IF-pin rank deficiency (refuted at the row-construction level: IF pin emitted
per registered sat, s2z per channel to 1 member), NOT iono/GIM (own-SH-VTEC run
identical), NOT the clock gauge (the 3 declared gauges sit cleanly at ~1e-9).  It
is a code↔phase REGISTRATION-GRANULARITY asymmetry.

**The fix (confirmed): co-gate the phase side per-(sat,signal) — symmetric with
the code quarantine.**  A sparse signal whose code is quarantined must ALSO leave
the shared system on the phase side, so the 2-station signal contributes nothing
(code sponge + phase folded into float ambiguity), removing the near-null on both
families rather than converting it.

**Correct LEVEL (learned the hard way):** the co-gate must be applied at the
`shared_phase_keys` ELECTION / phase-bias STATE ALLOCATION, NOT at the column
registration site.  Withholding a `registerPhaseBias` for a key still present in
`shared_phase_keys` breaks the "every shared_phase_key is a registered column"
invariant that eq34 / carry / snapshot rely on → a hard crash (confirmed: a
registration-site co-gate crashed the ON path; neutering it restored the run).
`shared_phase_keys` (legacy path) is built from `getSatPhaseBiasStates()`
(ApplicationNetwork.cpp:1417-1420), which is gated by `allowed_phase_keys` in
`MeasurementModel::ensureNetworkSharedStates` (:1183).  So the clean implementation
is: compute `allowed_phase_keys = { phase key : its same-signal code companion is
census-ADMITTED }` and pass it into `ensureNetworkSharedStates`; the quarantined
signals then never allocate a phase-bias state, and `shared_phase_keys` excludes
them consistently everywhere.  (Current tree: the registration-site co-gate is
NEUTERED with a pointer to this note; per-epoch ≥2 code guard retained.)

### RESIDUAL after the co-gate (adversarial split, unresolved)

Two of four investigators say the remaining directions after the co-gate are
GENUINELY null (ev 1e-10..1e-11 — a real datum deficiency needing the co-gate);
one says a layer is "weak-but-real" (~1e-3) miscounted by the single relative
cutoff and would need gate calibration (lower `kNullSpaceTol` into the spectral
gap) + a Kaula prior on admitted code biases.  The co-gate run to resolve this
was blocked by the crash; it needs a re-run once the co-gate lives at the
election level.

### STRATEGIC CONTEXT — an existing satellite-drop pre-filter

HEAD already contains a **cross-station observability pre-filter**
(`ApplicationNetworkAssembly.cpp:747`, `kMinStationsPerSat = 2`) that drops a
SATELLITE seen by < 2 stations ENTIRELY (clock + phase + observations) — the
user's "remove poorly-observed sats" intuition, at SATELLITE granularity.  It
does NOT catch this storm's driver, which is SIGNAL-level sparsity (C06/C13/C16
are well-observed on C2I/C6I but 2-station on C2X/C7X).  The per-(sat,signal)
co-gate is the signal-granularity complement.  Raising `kMinStationsPerSat` would
not help (the culprit sats have many stations on their dense signals).

### 305 CB-ADM + STATE-ALLOC phase co-gate — WORKS; removes the orphan class

Implemented the co-gate at the phase-bias STATE ALLOCATION
(`MeasurementModel::ensureNetworkSharedStates`, new `cbadm_admitted_code_keys`
param fed from `networkPassAHead_`/`networkPassASlipScan_`; a quarantined
signal's phase bias is not allocated ⇒ `shared_phase_keys` excludes it
consistently).  Crash-free (12-sta smoke completes; 305 completes).  Measured on
the 305 arm:

| ev<cutoff / dump | control | broken CB-ADM | + state-alloc co-gate |
|---|---|---|---|
| total | 22 | 43 | **33** |
| pure-PB | 3 | 89 | **24** |
| CB+PB | 98 | 151 | 129 |
| pure-CB | 7 | 0 | 21 |

The co-gate collapsed the pure-PB ORPHAN class (89→24) and shrank the shared
system (1805→1729 cols) — confirming the root-cause fix.  But it does NOT clear
the storm; a residual remains (still 0 epochs emitted).

**Residual is WEAK-BUT-REAL, not genuinely null.**  Eigenvalue histogram of the
below-cutoff set (first dump): **21 at ev∈[1e-3, cutoff)**, 6 at [1e-6,1e-3),
3 at [1e-9,1e-6), 5 deep (<1e-9 — the 3 declared gauges + ~2 residual).  The 21
dominant ones are distributed across a BeiDou-3 MEO sat's many signals, e.g.
`ev=4.41e-3 [PB|C26|L7D=0.18, CB|C26|C2I=0.18, PB|C26|L5P=0.18, PB|C26|L1P=0.18,
CB|C26|C8X=0.18]` — the admitted BeiDou families ARE observable but WEAKLY
(~1e-3), and the single relative cutoff `kNullSpaceTol·ev_max` = 6e-3 can't span
BeiDou's dynamic range (weak-real ~1e-3 vs true gauge ~1e-9; a clean spectral
gap sits between).  This resolves the dd_2↔dd_4 split IN FAVOUR of dd_4: the
residual is a GATE-CALIBRATION / datum-regularization issue, not a genuine
deficiency.

**Path to emit (two candidates, both a separate decision):**
- Gate calibration — lower `kNullSpaceTol` from 1e-9 into the spectral gap
  (≈1e-12 ⇒ cutoff ≈ 1e-5) so the weak-real families pass while the ~1e-9 gauges
  are still caught.  Small change but SHARED-PATH (affects every network config's
  rank gate — byte-identity gate + re-baseline required).
- Datum regularization — a loose Kaula/soft prior on each ADMITTED satellite code
  bias in `buildCodeBiasDatumRows` (physically justified: OSBs are bounded), which
  lifts the weak directions above the cutoff without touching the gate.  More
  targeted; also needs the declared-dimension accounting updated.

Either is a genuine follow-up; the co-gate itself is a confirmed keeper.

## 3. Open items for the user (SPEC §8)

Consolidated verdict + the follow-up (user chose "stop; consolidate", 2026-07-22):

1. **cb_admission (code-only quarantine) alone is NET-NEGATIVE on the real 305
   storm** (22→43) — do NOT ship it without the phase co-gate.  The two must go
   together (quarantine a sparse signal on BOTH bias families).  With the co-gate
   it is net-positive-but-incomplete (43→33, still withholds).

2. **The residual is a BeiDou-datum problem beyond this card** — a follow-up
   ("BeiDou W6 datum") is the real fix:
   - weak-but-real families (~1e-3, 21 dirs) → gate calibration (lower
     `kNullSpaceTol` into the 1e-3↔1e-9 spectral gap; SHARED-PATH, byte-id
     re-baseline) OR a Kaula/soft prior on ADMITTED satellite code biases in
     `buildCodeBiasDatumRows` (targeted, physically justified; update
     declared-dim accounting).  The Kaula prior is the cleaner first try.
   - the residual pure-PB (24, admitted 2-station signals with no reachable
     ambiguity anchor) → the phase-datum anchor election (SPEC §8-reserved
     pdp-threading) or arming `phase_datum_prioritized` + the heal.

3. **An existing satellite-drop pre-filter is complementary, not a substitute.**
   `kMinStationsPerSat=2` (ApplicationNetworkAssembly.cpp:747, in HEAD) drops
   single-station SATELLITES; this storm is SIGNAL-level sparsity on multi-station
   sats, so the two operate at different granularities.

4. **Ship decision reserved to the user.** The census + quarantine + per-epoch
   guard + co-gate are correct, default-OFF, byte-identical-OFF, unit-tested
   components.  Whether they ship as-is (a documented partial), fold into the
   BeiDou-datum follow-up, or are superseded by a satellite/signal-drop
   reframing, is the user's call.  Nothing here is committed.

---

## 4. ERRATUM — units in the §2 A/B tables (added 2026-07-22 by Card H3)

**Every A/B table in §2 above mixes two units without labelling them, and they
are not interchangeable.** The estimability dump is BUDGET-LATCHED: the producer
records only the first `spectrum_diag_budget` gate evaluations of a run (6
today, `NetworkRunContext.hpp`). So a 305-station arm yields exactly 6 dumps,
and the same measurement can be quoted two ways:

| §2 phrasing | actual unit | 305 control arm |
|---|---|---|
| "22 ev<cutoff per epoch", "22 → 43 → 33" | **per dump** | 22.5 |
| "135 below-cutoff directions", "135 → 258", the family tables (CB+PB 98, pure-CLK 16, …) | **across dumps** (6) | 135 |

The two differ by the factor 6. Concretely: the control arm's `pure_CLK 16`
across dumps is 2.7 per dump — i.e. the **3 declared clock gauges**, one per
constellation, minus the dumps where the gate dropped one; it is *not* 16
unexplained gauge directions. Read every §2 family/culprit table as
**across-dumps** and every "ev<cutoff" figure as **per-dump**.

Two further reading rules the §2 tables do not state:

- **Never compare the dump's `N ev<cutoff` with a withhold line's `observed M`.**
  The withhold message reports the POST-heal re-gate, a different quantity on
  the same epoch (measured, control arm TOW 259200: dump 22, withhold 14,
  identical cutoff). They are never summed or cross-validated.
- **Never judge an arm by its raw below-cutoff total.** The population is four
  classes with four owners, and a *larger* total is an improvement when it
  trades genuinely-null directions for weak-but-real ones a calibrated cutoff
  can clear — which is exactly what control (22/dump) → co-gated (33/dump) did.

The canonical, unit-labelled presentation is the four-class reconciliation table
in `docs/SPEC_beidou_w6_datum.md` §0.1, and every number above is now
re-derivable from the archived logs with
`python scripts/characterize_storm.py <log> --cross-check` (Card AN), which
reproduces the §2 control figures exactly: 135 directions across 6 dumps,
BeiDou 123 / Galileo 6 / GPS 6, CB+PB 98 / pure_CLK 16 / CB+CLK+PB 13 /
pure_CB 7 / CLK+PB 1, 118 of 135 involving a `CB|` component, culprits C06 71 /
C13 68 / C29 53 / C16 49 — **18 of 18 oracle values PASS**.
