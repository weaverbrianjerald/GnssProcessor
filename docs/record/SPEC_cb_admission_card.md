# SPEC — W6 code-bias admission census + station-local quarantine ("CB-ADM")

**Status:** SPEC Jul-21 2026 (Fable).  **IMPLEMENTED (INC-1 + INC-2 + the
state-allocation phase co-gate) and COMMITTED** — `NetworkBiasAdmission.{hpp,cpp}`,
the `ensureNetworkSharedStates` `cbadm_admitted_code_keys` param and
`network.cb_kaula_prior` are all in HEAD.  Default-OFF; OFF path byte-identical.
Targets the BeiDou network blocker in its Jul-17 form (bias-datum sparsity:
whole-epoch withholds, `PB/CB|C##` null directions).
As-built §9 · efficacy ladder §10 · root cause §11 · verdict §12 · units erratum §13.

> **⚠ SUPERSEDED-BY for the blocker itself: `docs/SPEC_beidou_w6_datum.md`.**
> CB-ADM code-only is NET-NEGATIVE on the real 305 storm and must never ship without
> the phase co-gate (§12).  The storm now emits 6/6 dumps via a FOUR-piece chain in
> which each piece is necessary and none alone emits.  Product QUALITY remains
> UNCLAIMED.

*Absorbed 2026-08-15: `docs/HANDOFF_cb_admission_card.md` (now
`docs/attic/HANDOFF_cb_admission_card.md`) — its §1 → §9, §2 → §10, root-cause and
co-gate material → §11, §3 → §12, §4 erratum → §13.  Run-directory paths (`runs/…`,
`build/x64-Release/run_…`) resolve under `D:\GnssProcessor_archive\2026-08-12\`.*

**One sentence:** replace the per-epoch instantaneous ≥2-station admission gate on W6
satellite code-bias (OSB) columns with a **run-latched startup census** per
(sat, signal) — admitted pairs become shared border columns exactly as today, while
sub-threshold pairs keep their measurements and keep their per-station sat-bias states
as **station-local sponge states** (never registered, never datumed, never emitted), so
sparse BDS signals stop leaking their OSBs into per-LOS iono AND stop injecting
near-null directions that withhold entire epochs.

**Line-number caveat:** anchors in §0–§8 are as of Jul-21 (tree mid-decomposition,
INC-0..8 landed) and have drifted.  Anchors in §9–§13 were re-grepped 2026-08-15.
Verify by symbol, not line.  Anchor-drift ledger (older notes → current):
`ApplicationNetwork.cpp:1417-1420` → `:1628-1630` (`shared_phase_keys` build);
`ApplicationNetworkAssembly.cpp:747` → `:1523` (`kMinStationsPerSat`).

---

## 0. Decision trail — do not relitigate

Design question (user): gate BDS bias-state creation on a minimum station count, or
restrict BDS to well-tracked bands — and won't unmodeled biases leak into the ionosphere
if we estimate code biases only for well-observed states while keeping the signals?
Analysis answered yes; the user chose the quarantine variant.  Rejected alternatives, do
not re-propose:

- **Keep measurement without any bias state** — the unmodeled OSB cannot sit in the
  residual; the other signals' network-shared states are pinned by the rest of the
  network, so the per-pair sponges are slant iono (μ-slope) and the receiver bias (common
  mode).  Iono share ~ d/Δμ: BDS OSB spreads of tens of ns (1 ns ≈ 0.3 m) against
  Δμ ≈ 0.25–0.78 across B1I/B3I/B2a/B1C ⇒ **meters** of slant-iono error, re-entering
  phase with −μⱼ and wrecking that satellite's PB fractionals.  This is what the current
  code does for sub-threshold pairs (§1.2) — at BDS scale not a "local nuisance".
- **Deweight instead of quarantine** — the OSB is deterministic, not noise; weight scales
  the leak linearly but never removes it, and weakens estimability if the state exists.
- **Per-epoch (instantaneous) gating** — count flapping ⇒ prune/re-register/re-seed
  cycles ⇒ chain-datum churn.  The gate must be latched.
- **Static per-constellation band whitelist** — mishandles the BDS-2/BDS-3 split
  (C2I/C6I/C7I birds vs C2I/C6I/C1P/C5P birds); the per-(sat,signal) census subsumes it.
- **Shared-column quarantine** (register, exclude from datum/spectrum/emission, hold with
  a loose SharedPrior) — a loose prior's info (1/σ² ~ 1e-3 at σ=30 m) sits numerically AT
  the rank-gate cutoff (`kNullSpaceTol = 1e-9`·σmax, σmax ~1e6), so it either trips the
  gate or survives by coincidence; making it structural means carving a checked-subset
  concept into the spectrum machinery.  The station-local variant (§2) needs none of it —
  local columns are Schur-eliminated and never enter the shared spectrum.

## 1. Motivation — current semantics, measured facts

### 1.1 The existing gate

`Application::networkAssemblyRegisterColumns_`,
`src/core/app/ApplicationNetworkAssembly.cpp` ~854–900, per epoch when
`network.estimate_sat_code_bias` is on: (1) `cb_station_count["sat|sig"]` = number of
**present** stations whose StateManager holds that sat-bias state (~873–882);
(2) `keep_keys` = count ≥ 2; (3) present stations register `keep_keys ∩ SM` into the
shared border (`est.registerCodeBias`) and accumulate `registered_code_keys` (first-seen
order — a **per-epoch local** of the run TU, `ApplicationNetwork.cpp` ~2157);
(4) `sm.pruneSatBiasStatesExcept(station_keep)` deletes the rest (warm-cached:
`StateManager.cpp` ~1886–1921, value+variance stashed for warm restore).

The design comment at ~861–872 states the sub-threshold fate: a single-station code bias
"stays a local nuisance (its sat-bias state is pruned below, leaving its raw code row to
fold into the receiver clock / slant iono — the pre-W6 path)".  That is §0's leak, by
design — tolerable for rare one-off GPS channels, poison at BDS scale where a large
fraction of (sat,signal) pairs sit at 1–2 stations and **flap**.

### 1.2 The BDS failure chain

- Flapping across cnt≥2 ⇒ alternating {registered+datumed} / {pruned+leaking} epochs,
  with prune/warm-restore and Card-2 datum re-seed churn.
- Registered pairs can still be datum-orphaned: a BDS sat whose OTHER signals stayed
  sub-threshold has <2 distinct-frequency registered signals ⇒ `selectIfPinPair` cannot
  form its IF pin ⇒ its CB columns hang on the sum-to-zero channel row alone ⇒ near-null.
- The null-space rank gate withholds the **whole epoch** on any near-null direction
  (`ApplicationNetworkAssembly.cpp` ~147, ~1230; counter `ctx.counters.n_gate_withheld`,
  `ApplicationNetwork.cpp` ~423) — a handful of sparse BDS states has epoch-global blast
  radius.  The Jul-17 symptom: withholds every epoch, `PB/CB|C##` nulls named by the
  estimability dump.

### 1.3 Why the quarantine is nearly free (existing infrastructure)

- The shared scatter already has the local fallback: an SM sat-bias state whose key is
  NOT registered hits `est.codeBiasColumn` → throw → `catch { continue; }`
  (`ApplicationNetworkAssembly.cpp` ~130–138) and **stays a station-local column**,
  Schur-eliminated station-side, never in the shared NEQ / spectrum / rank gate.
- Sat-bias states are born as sponges: WLS birth-seed absorbs the current residual
  (`seedSatBias`, `MeasurementModel.cpp` ~884; "the small (10 m) initial std lets future
  epochs refine the rest"), initial σ = `initial_std.sat_bias`
  (`StateManager::addSatBiasState`, `StateManager.cpp` ~1733), process noise
  `sat_code_bias_q` = 1e-10 m²/s (`GnssConfigTypes.hpp` ~1606–1616 — hardware delays
  drift ~mm/day).
- Everything downstream keys off `registered_code_keys` / the registered border: datum
  rows (`NetworkSbasis::buildCodeBiasDatumRows`, `NetworkSbasis.hpp` ~298, call site
  `ApplicationNetwork.cpp` ~3759–3775), Card-2 carry, estimability dump `col_desc`, the
  solution trace, snapshot emission (~7346–7349), carry stash (~8210–8212).  **Not
  registering a key excludes it from all of them** — no per-consumer edits.

⇒ the whole behavioral change is ONE membership-test swap at the registration site plus
a census that computes the latched set.

## 2. Design (decisions, not options)

- **Admission unit:** the (sat, signal) code pair.  **ADMITTED** = shared border column,
  datumed, emitted (identical to today's registered pairs).  **QUARANTINED** =
  measurements kept, per-station SM sat-bias states kept and NEVER pruned while the flag
  is on, never registered ⇒ station-local sponge.  Default class QUARANTINED — pairs
  first seen after the census window quarantine automatically.
- **Census:** a pure scan over the first K epochs the run loop will process (the synced
  vector is fully in memory — `MultiRinexSync::synchronize()` at setup,
  `ApplicationNetworkSetup.cpp` ~259).  Latched once, immutable: no promotions, no
  demotions, no per-epoch evaluation.
- **Tier 1 (count):** station st *supports* pair p iff st has a usable raw code
  observation for p in ≥ ⌈K_eff/2⌉ census epochs (absolute floor — avoids flimsy 2-of-2
  ratios from gappy stations).  Admitted-candidate iff #supporting stations ≥
  `effective_min = max(2, min(cb_admission_min_stations, n_census_stations))` (2 = the
  hard estimability floor; the clamp keeps 3-station dev rosters runnable).
- **Tier 2 (IF-pinnability):** per satellite, among its tier-1 candidates, require ≥ 2
  DISTINCT carrier frequencies (SignalBand mapping — C1P vs C1X same band ⇒ not
  distinct).  A satellite failing this has ALL candidates demoted to QUARANTINED (logged
  "no IF-pinnable pair") — kills the §1.2 datum-orphan class at the source rather than
  letting the rank gate find it.
- **Emergent registration semantics when ON:** an admitted pair registers whenever ≥ 1
  present station holds its SM state, so it stays registered through 1-station dips
  (datum rows still pin every registered column; Card-2 `cb_time_linked` carry supplies
  the prev-epoch target) — the anti-flap win.  A zero-present epoch drops the key for that
  epoch; existing resume/re-seed machinery (`code_bias_datum_resumed_` / reseed scratch)
  covers reappearance.
- **No new stochastic machinery:** quarantined states inherit today's birth seed, initial
  σ, `sat_code_bias_q`, warm cache.  No new prior knob (the 10 m `initial_std.sat_bias`
  IS the sponge prior).

## 3. Mechanism

### 3.1 New seam — `filter/NetworkBiasAdmission.{hpp,cpp}` (+ tests)

Decomposition-style seam (sibling of NetworkSbasis / NetworkPhaseDatum): a POD result
(`admitted` set of "sat|sig" keys, `n_admitted` / `n_quarantined_census` / `active`,
`isAdmitted()`) plus one pure static `runCensus(synced, i0, census_epochs, min_stations,
logger)`; quarantined = everything else, implicit.  Full Doxygen per house style.

Census window `[i0, min(i0+K, synced.size()))` — **i0 is the post-`start_offset_hours`
loop start**, not index 0 (a census over epochs the loop never processes classifies on
the wrong data; `ApplicationNetworkSetup.cpp` ~230 documents the fast-forward).
Usable-observation predicate over `SyncedEpoch::by_station` →
`RinexObservation::measurements` (`RinexParser.hpp` ~46): 3-char id, `id[0]=='C'`, no
`'_'`, finite nonzero; GLONASS skipped (no OSB scope — `ApplicationNetworkAssembly.cpp`
~129).  Deterministic iteration (std::map order).  Clamp: fewer than `census_epochs`
epochs available ⇒ K_eff = available count (a 10-ep smoke with census_epochs=15 must
still work, with a log line).

### 3.2 Config (conventions per CLAUDE.md; section `network`)

```
network.cb_admission                : bool, default false   (master enable)
network.cb_admission_min_stations   : int,  default 4
network.cb_admission_census_epochs  : int,  default 15
```

Getters `networkCbAdmission()`, `networkCbAdmissionMinStations()`,
`networkCbAdmissionCensusEpochs()`; parse in `loadNetworkSection_`
(`GnssConfigLoadNetwork.cpp`) via `jval`; add to the network summary toggle-log line.
Naming mirrors `phase_datum_min_stations` / `parallel_min_stations`.

### 3.3 Call site — run TU, pre-loop

Compute once before the epoch loop (after warm-resume, where the loop start index is
known); store in `NetworkRunContext`.  Flag off ⇒ `active=false`, nothing else happens.
One-shot census log: per-constellation summary (pairs seen / admitted / quarantined-count
/ quarantined-pinnability) + first-N per-pair table (key, support count, verdict, reason).

### 3.4 Registration-site swap — `ApplicationNetworkAssembly.cpp` ~873–900

Under `if (config_.estimateNetworkSatCodeBias())`, branch on `ctx.admission.active`.
**OFF:** existing block byte-for-byte untouched (count → keep_keys → register → prune).
**ON:** skip the count pass; for present stations, for each SM sat-bias state always
insert into `station_keep` (quarantined states are never pruned), and if
`admission.isAdmitted(key)` do `est.registerCodeBias` + `registered_code_keys`
dedup-append exactly as today.  `pruneSatBiasStatesExcept(station_keep)` stays
structurally (a no-op by construction; parse-failure keys keep today's fate).
Everything downstream is untouched by construction (§1.3); unregistered states ride the
existing `catch { continue; }` local fallback.

### 3.5 Stale doc fix (rides the card)

`GnssConfig.hpp` ~1371–1374 still says the W6 scope is "GPS + Galileo" — stale since the
multi-band slice; BDS `C##` columns exist (they are the symptom).  Same for the
withhold-path error HINT text, which still tells the user to set
`active_systems = GPS+Galileo`; the accurate hint under this card is the admission census
/ min-station knob.

### 3.6 Telemetry (standing invariant #4 — counters are the ON-path truth)

Run-end counters (`NetworkRunContext::counters`): `n_cb_admitted`,
`n_cb_quarantined_census` (census close); `n_cb_late_quarantined` (distinct keys first
seen post-census — counted at the registration site: SM key parses, not admitted, not in
census sets); existing `n_gate_withheld`, the primary success metric.
**Anti-inert check (hard smoke criterion):** a BDS-armed ON run with
`n_cb_quarantined_census == 0` means the census is miswired — FAIL the smoke regardless
of how clean the log looks.

## 4. What does NOT change

> **⚠ 2026-07-22 — the FIRST bullet's premise was REFUTED by measurement; the
> text below is preserved verbatim and unrenumbered because
> `docs/SPEC_inc8b_election_split.md`:969-975 quotes it as a written inter-card
> scope contract.  Read it against §11.**  Code-only quarantine does not leave
> the phase side alone: on the 305 storm it drove pure-`CB|` and `CB+CLK+PB` to
> zero (this section's own success criterion, met) while CREATING **89
> pure-`PB|` directions from zero**, and the epoch still withheld.  The W6
> satellite code bias is load-bearing for the phase↔clock split.  **The
> per-(sat,signal) phase co-gate is MANDATORY, not optional** (§11, §12).

- **Phase side (PB).** Out of scope, deliberately.  The Jul-17 dump names
  BOTH `CB|C##` and `PB|C##` near-nulls, so `PB|C##` directions are EXPECTED
  to survive INC-2 unless the phase datum's own machinery
  (`phase_datum_min_stations` / sticky / anchor election,
  `ApplicationNetwork.cpp` ~1310–1456; `healNearNullPhaseBias` backstop)
  clears them under the run's pdp settings.  Two reasons not to widen scope
  here: the phase gate/heal already exist and are config-armable, and the
  anchor election is mid-decomposition (8b split pending) — threading a
  census into it now collides with that sequencing.  The INC-2 smoke must
  ATTRIBUTE the surviving null set by name (`CB|…` vs `PB|…`,
  `network.trace_estimability`): the card's own success criterion is
  **zero surviving `CB|C##` directions**; the measured `PB|C##` residue is
  the report that seeds the follow-up ruling.
- **Receiver IFB drop** (`ApplicationNetworkAssembly.cpp` ~140–146):
  unchanged.  On a non-primary band a quarantined pair's local state absorbs
  the {sat OSB + station IFB residual} lump — a superset nuisance, honest at
  1-station support (perfectly confounded there anyway).
- **MW/lane tiers:** BDS already hardcoded off (`MwLaneTiers` ~107) — no
  interaction.
- **GLONASS:** no OSB states exist; census skips R sats.
- **`estimate_iono_sh` forcing `estimate_sat_code_bias` on**
  (`GnssConfig.hpp` ~1392): unchanged; admission applies whenever W6 is armed
  AND `cb_admission` is on.
- **Warm resume:** the census is per-process, recomputed over the resumed
  span; it is NOT carried in the resume snapshot (v1).  A resume starting at a
  different epoch can classify differently — known property, field-inert for
  post-processing (deterministic per input span).

## 5. Standing-invariant pre-flight

1. **Stiff time-linked quantities need a wired event response.**  No NEW zero-q carry:
   quarantined states inherit the identical process model (`sat_code_bias_q` 1e-10),
   birth-seed and warm-cache lifecycle pre-prune states have today.  The residual
   exposure — a satellite hardware bias step against a stiff local sponge — is strictly
   SMALLER than the status quo: today the entire OSB leaks into per-pair iono every epoch;
   under quarantine only the post-step delta leaks until re-convergence.  Backstops:
   station-side innovation handling + D5.  No new detector required.
2. **Release-only data runs**, renamed exe (§7).
3. **Deferrals triaged.**  (a) RT-producer census arm: DEFERRED, field-inert (the RT
   network arm does not run BDS W6 today; needs an arm-once variant at RTF).  (b) PB-side
   census threading: DEFERRED pending INC-2 attribution DATA (§4).  (c) Admission in the
   warm-resume snapshot: DEFERRED, field-inert for post-processing.
4. **Run-end counters** specified (§3.6) with an explicit anti-inert FAIL criterion.
5. **Grade datum-invariant / SD quantities only** (§6 INC-3): DD fractionals
   (`scripts/grade_ddfrac.py`), median/std/worst-sat.  No raw wire-step grading.
6. **Default-OFF + byte-identity.**  OFF path structurally byte-identical (branch-guarded
   registration block); gate via the existing 10-arm matrix + estim arm harness (INC-5..8
   pattern — `docs/network_decomposition_plan.md`).  INC-1 is additionally byte-identical
   ON (census is observational).  Never compare across build configs.

## 6. Increments & gates

*(All three EXECUTED — outcomes §9/§10.  Gate definitions kept as the pre-registration the
results are read against.)*

**INC-1 — census + config + logging (no consumers).**  Seam, config keys, run-TU pre-loop
call, census table log, counters; registration site untouched.  *Gates:* new unit tests
green (§9.3); full suite green (pre-card expectation 805/804/1; actual pre-card baseline
at build time **827/826/1**, post-card 839/838/1); OFF byte-identity; **ON byte-identity**
on a BDS arm (census inert by construction — proves the wiring costs nothing numerically).

**INC-2 — registration swap.**  §3.4 branch + `n_cb_late_quarantined` counter + §3.5 doc
fix.  *Gates:* unit test for the membership swap (quarantined SM state present +
unregistered + scatter-catch exercised); suite green; OFF byte-identity (10-arm matrix +
estim arm); ON BDS smoke = `_maxnet_305_gpsgalbds_d15_5ep` + `cb_admission` against the
KNOWN Jul-17 baseline (0/5 epochs emitted, withheld every epoch).  Success as
pre-registered = epochs emit, `n_gate_withheld` ~0, near-null set down to the 3 declared
clock gauges **plus any surviving `PB|C##`** (expected — §4), census counters nonzero
(anti-inert), no new disc events on GPS/GAL, D5 flag counts ~parity.  **ON is NOT
byte-identical even for GPS-only arms** (latched census vs per-epoch counts differ by
design) — grade invariants, don't diff bytes.

**INC-3 — validation (graded).**  15-ep gate arm first, then ≥1 h graded run **ON COMMAND
ONLY** (staged-cadence directive).  Grade: DD fractionals + median/std/worst-sat on
GPS/GAL unchanged-vs-control, BDS emission present for admitted pairs, withhold telemetry
over the full span, PB-vs-CB null attribution (§4).

## 7. Run discipline & harness configs

- **The A/B baseline is the measured storm repro:**
  `config/test_suite/_maxnet_305_gpsgalbds_d15_{5,10}ep.json` (Jul-17 result KNOWN: zero
  solution epochs, whole-epoch withhold every epoch, 14→16 near-null directions named
  `PB|C##`/`CB|C##`; ~3.2 min/epoch ⇒ 5-ep arm ≈ 16 min).  ON arm = same config +
  `cb_admission` (+ `trace_estimability`).  Cheap INC-1/INC-2 iteration uses a 50-sta
  variant (`_cbadm_50sta_15ep_gpsgalbds.json` from `_ppprtk50_6_gps_gal_bds.json`,
  `estimate_iono_sh` on) — but the proof A/B is the maxnet pair.  New harness configs get
  `parallel_carry=true` / `parallel_reduce=false` (MUTEX — the loader silently forces
  pcarry off if both are set; grep the log to confirm).
- Build per **CLAUDE.md → Build** (`scripts/vs2026_build.ps1`; VS 2022 is RETIRED by
  standing directive).  Data runs Release only, renamed copy `gnss_processor_cbadm_run.exe`;
  pre-kill zombies NAME-SCOPED to that exe (wildcard sweeps kill concurrent sessions'
  runs); LNK1104 = build lock → retry.  `CMakeLists.txt`: explicit source adds for the new
  .cpp and test file (no globbing).
- ~~leave the card's work uncommitted for user review~~ — **STALE, the card is committed
  (§9).**  What survives: do NOT commit, revert or "clean up" anything outside this card's
  files (the tree routinely carries heavy concurrent sibling work), and do NOT flip the
  `_wlver_*` family (it stays `parallel_carry=false` as the byte-id baseline).

## 8. Rulings reserved to the user (do not decide unilaterally)

Persistent `PB|C##` nulls after INC-2 ⇒ report the spectrum dump and stop (pdp-threading
is a separate card).  Any necessary deviation from the §2 criteria (min_stations default,
sustained floor, tier-2 rule) ⇒ report with data, propose, await ruling.
*(Both rulings were taken: the `PB|C##` nulls were reported and the user's call was "stop;
consolidate" on 2026-07-22 — §12.)*

---

## 9. As built (2026-07-21/22; committed)

### 9.1 Footprint — the entire CB-ADM change

| File | Change |
|---|---|
| `include/filter/NetworkBiasAdmission.hpp` | NEW — POD result + `runCensus` static (pure) |
| `src/core/filter/NetworkBiasAdmission.cpp` | NEW — the census scan |
| `tests/test_network_bias_admission.cpp` | NEW — 12 unit tests |
| `include/config/GnssConfig.hpp` | 3 getters + 3 members; fixed the stale "GPS+Galileo" W6 doc (§3.5) |
| `src/core/config/GnssConfigLoadNetwork.cpp` | parse + validate + ON-log + INERT-warn; 2 stale-doc fixes |
| `include/app/NetworkRunContext.hpp` | `admission` member + 3 counters |
| `src/core/app/ApplicationNetwork.cpp` | pre-loop census call site + table log + run-end telemetry + fixed zero-epoch HINT |
| `src/core/app/ApplicationNetworkAssembly.cpp` | registration-site swap (OFF verbatim / ON latched) |
| `CMakeLists.txt` | +`NetworkBiasAdmission.cpp` (gnss_core), +test (gnss_tests) |

Plus the phase co-gate of §11.3: `MeasurementModel::ensureNetworkSharedStates` gained the
`cbadm_admitted_code_keys` parameter (`include/measurement/MeasurementModel.hpp:101`,
`src/core/measurement/MeasurementModel.cpp:1148` param / `:1228-1231` gate), fed from
`networkPassAHead_` / `networkPassASlipScan_`.

Refreshed anchors (re-grepped 2026-08-15): registration swap
`ApplicationNetworkAssembly.cpp:1708-1780` (OFF branch ~`:1720-1743`, ON branch
~`:1755-1776`), census consumers `:1640-1690`; loader `GnssConfigLoadNetwork.cpp:2389-2401`;
`shared_phase_keys` built from `getSatPhaseBiasStates()` at
`ApplicationNetwork.cpp:1628-1630`; `spectrum_diag_budget = 6` at
`NetworkRunContext.hpp:1913`.

### 9.2 Census and config as shipped — no deviation from §2 / §3.1 / §3.2

The shipped census IS the specified one: §2's two-tier criterion (tier-1 sustained
support ≥ ⌈K_eff/2⌉ census epochs and #supporting ≥
`effective_min = max(2, min(min_stations, n_census_sta))`; tier-2 ≥ 2 distinct carrier
bands via `classifySignalForSat().band` or ALL that satellite's candidates demoted),
GLONASS excluded, deterministic in `std::map` order, default class QUARANTINED.  Config
keys are §3.2's verbatim — `cb_admission` false / `cb_admission_min_stations` 4 /
`cb_admission_census_epochs` 15 (`GnssConfigLoadNetwork.cpp:2389-2401`).

Two shipped properties not in §3: the OFF registration branch is the **verbatim legacy
logic re-scoped**, so OFF is byte-identical, while **ON is NOT byte-identical even on
GPS-only arms** (latched census vs per-epoch counts) — grade invariants, don't diff
bytes; and the loader warns **INERT** when `cb_admission` is armed without the W6 slice
(`estimate_sat_code_bias`).

### 9.3 Tests and suite

12 new `NetworkBiasAdmissionTest` cases: sustained-support floor, `effective_min` clamp
incl. the 3-station roster, tier-2 demotion, same-band co-channel non-distinctness,
late-seen default, K_eff clamp, GLONASS/phase/zero exclusion, i0 offset, determinism,
empty-window inactive, registration-swap contract.  Full suite at build time
**839 / 838 passed / 1 skipped** (pre-card baseline 827/826/1 + 12 new; ZERO
regressions).  `gnss_core` + `gnss_tests` built clean.

---

## 10. Measured efficacy ladder

> **READ §13 FIRST — the two units in these tables are not interchangeable.**  The
> estimability dump is BUDGET-LATCHED at `spectrum_diag_budget = 6`
> (`NetworkRunContext.hpp:1913`), so a run yields exactly 6 dumps.  Every "ev<cutoff"
> figure is **PER DUMP**; every direction-count, family table and culprit table is
> **ACROSS DUMPS**.  They differ by the factor 6.

### 10.1 Census wiring — 50-sta smoke (Release), MECHANISM CONFIRMED

`_cbadm_50sta_15ep_gpsgalbds.json` (first 50 of the 305 roster, GPS+Gal+BDS, full
carry/iono-SH stack), `cb_admission` ON, K_eff = 15 × 50 stations, `effective_min` = 4,
`sustained_floor` = 8:

- **939 (sat,signal) pairs seen / 734 admitted / 205 quarantined.**
- Per constellation: BeiDou 355 → 250 admitted / 105 low-count; Galileo 269 → 245/24;
  GPS 228 → 189/39; QZSS 56 → 36/20; SBAS 31 → 14/10, **plus 7 tier-2 no-IF-pin
  demotions**.
- **Anti-inert PASS** (205 quarantined > 0).
- **`n_cb_late_quarantined = 0`** — census keys byte-match the sat-bias state keys
  exactly, including the single-digit `"C 1"` space padding.
- Admitted MEO BeiDou (C19–C48) registered as shared columns (`CB|C19`…`CB|C48`);
  quarantined single-digit GEO/IGSO BeiDou (C01–C07) did not register ⇒ the swap is
  correct.

### 10.2 50-sta efficacy A/B (control = legacy gate vs CB-ADM ON)

Both arms withheld all 6 dumped epochs (0 emitted).  Below-cutoff near-null directions,
per-epoch average, by dominant family:

| class | control | CB-ADM ON |
|---|---|---|
| pure_CB | 2.8 | 2.5 |
| mixed_CB↔PB | 11.5 | 19.5 |
| pure_PB | 1.0 | 5.0 |
| pure_CLK | 3.0 | 2.8 |

Direct mechanism evidence: CB-ADM registered `CB|` columns for **24 BeiDou sats vs the legacy
gate's 15** — 9 more sparse sats the per-epoch gate pruned.  An admitted sat (sustained support
≥ 4 stations) can still be tracked by ONE station in a given epoch; the anti-flap design
registers it anyway, and the code datum cannot pin the resulting single-station CB↔PB↔CLK
confounding — a 1-station code column is inseparable from that sat's clock.

### 10.3 50-sta `min_stations` sweep (10 ep) — the config is not a valid efficacy test

Raising `min_stations` 4 → 10 (**285 admitted / 654 quarantined**) DID drop the pure-CB share
(2.8 → 1.0/epoch) but exploded the phase side: ev<cutoff **19 (control) → 540 (min=10)** per
epoch, dominated by ~476 pure-PB directions.  **The finding, which recurs at 305:** the W6
satellite CODE bias is **LOAD-BEARING for the phase↔clock split** — it anchors the sat clock so
the phase bias can separate, and quarantining it while the phase bias and clock stay shared
columns breaks that separation.  At the default min = 4 this is mostly benign (pure_PB ≈ 5);
the sweep proves the 50-station GLOBAL scatter is degenerate for BeiDou and cannot demonstrate
efficacy either way.

### 10.4 120-sta 10-ep A/B WITH the phase heal armed

Earlier configs had the phase-side machinery OFF (`phase_datum_min_stations=1`,
`phase_datum_prioritized` unset ⇒ `healNearNullPhaseBias` early-outs).  Arming it
(`phase_datum_prioritized=true`, `phase_datum_min_stations=2`) still withheld all 11
epochs in BOTH arms:

| below-cutoff / epoch | legacy + heal | CB-ADM + heal |
|---|---|---|
| ev<cutoff (declared = 3) | 23 | 26 |
| pure_CB | 3.2 | 5.5 |
| mixed_CB↔PB | 16.3 | 16.7 |
| pure_PB | 0.0 | 3.0 |
| pure_CLK (= the 3 gauges) | 3.0 | 3.0 |

- The `CLK|` near-nulls are per-constellation COMMON MODE = the 3 DECLARED clock gauges
  (`CLK|E…`, `CLK|C…`, `CLK|G…`, equal weights) — EXPECTED, not the storm.  **A satellite-CLOCK
  coverage gate is misdirected**: the clocks are already gauged and there is no single-station
  `CLK|` near-null (grep found none).
- The storm is **mixed CB↔PB ≈ 16/epoch, present in BOTH arms** — ≥2-station satellites,
  confounded for reasons unrelated to the sparse-code-bias leak.

### 10.5 ★ The geometry lesson (this is why 10.2–10.4 could not decide anything)

**Fast 50/120-station GLOBAL subsets test the WRONG storm.**  Being subsets of the 305 GLOBAL
roster, ALL their constellations are sparse ⇒ generic under-determination, which CB-ADM does
not target.  The real BeiDou W6 storm needs **GPS/Gal DENSE and only BeiDou sparse** — the 305
roster — and cannot be shrunk to a fast config without losing either the storm or the density.
The lesson of the §10.2 → §10.5 order: three cheap arms in a row agreed with each other and
were all measuring something else.

### 10.6 305 CONTROL storm characterization — the definitive test, and it VALIDATES the premise

`_maxnet_305_gpsgalbds_d15_5ep_ctl.json` (legacy gate, `trace_estimability`) reproduced
the known Jul-17 storm: **0/8 epochs emitted, 22 ev<cutoff per dump** (declared_dim = 3),
**135 below-cutoff directions across 6 dumps**.

- **Constellation:** BeiDou **123** / 135, Galileo 6, GPS 6.  The 305 config ISOLATES the
  BeiDou storm; the fast subsets did not.
- **Top culprit satellites** (appearances across dumps): C06 **71**, C13 **68**, C29
  **53**, C16 **49**, C34 43, C36 32, C38 31 — dominated by BeiDou GEO/IGSO, the
  regionally-tracked birds a GLOBAL network sees from few stations.
- **Family signature:** CB+PB **98**, pure-CLK **16** (the gauges), CB+CLK+PB **13**,
  pure-CB **7**, CLK+PB **1**.  **118 of 135 directions involve a `CB|` component** —
  CB-ADM's quarantine targets the storm directly.

### 10.7 305 CB-ADM arm (min = 4) — CB-ADM ALONE IS NET-NEGATIVE

Same config + `cb_admission` (census: **903 admitted / 134 quarantined / 14 late**).
Result: **22 → 43 ev<cutoff per dump**; directions across dumps 135 → 258.

| directions (across dumps) | control | CB-ADM |
|---|---|---|
| pure CB | 7 | **0** |
| CB+CLK+PB | 13 | **0** |
| CB+PB | 98 | 151 |
| pure PB | **0** | **89** |
| pure CLK (gauges) | 16 | 18 |
| total | 135 | 258 |

CB-ADM did its job on the code side — every pure-`CB|` and `CB+CLK+PB` direction
eliminated — and CREATED 89 pure-`PB|` directions from zero.  **This refutes §4's
premise** (see the §4 banner and §11).

### 10.8 305 + per-epoch ≥2 guard + registration-site co-gate — a measured BYTE-IDENTICAL NO-OP

Implemented the apparently-principled fix (register an admitted code pair only when ≥ 2
present stations hold it THIS epoch; co-gate phase to code-eligible sats).  The 305
re-run came out **byte-identical to the un-guarded CB-ADM run**: 43 ev<cutoff, the same
258 directions, the same culprits **C16 = 61 / C13 = 60 / C06 = 60**, census 903/134/14,
`n_cb_late_quarantined = 14`.  It never triggered, and the two reasons are the finding:

1. At 305 density EVERY admitted satellite already has ≥ 2 present stations, so the
   per-epoch guard is a no-op.
2. Quarantine is per-(sat,**SIGNAL**); almost every quarantined sat still has one admitted
   signal ⇒ code-eligible ⇒ the phase co-gate withholds nothing.

So the storm is NOT a per-epoch coverage problem.  Distinct near-null `PB|` satellites
went **control 20 → CB-ADM 35** — +15 ADMITTED, well-covered BeiDou.  The +89 pure-PB come
from removing the SPARSE satellites' code biases from the shared **s2z code-bias datum**,
which destabilizes the code↔phase↔clock separation of the REMAINING admitted sats.  A
datum-coupling effect that coverage gates cannot fix.

### 10.9 305 + STATE-ALLOC phase co-gate — WORKS; removes the orphan class

Co-gate applied at phase-bias STATE ALLOCATION (§11.3).  Crash-free (12-sta smoke
completes; 305 completes).

| ev<cutoff / dump | control | broken CB-ADM | + state-alloc co-gate |
|---|---|---|---|
| total | 22 | 43 | **33** |
| pure-PB | 3 | 89 | **24** |
| CB+PB | 98 | 151 | 129 |
| pure-CB | 7 | 0 | 21 |

The co-gate collapsed the pure-PB ORPHAN class (89 → 24) and shrank the shared system
(**1805 → 1729 columns**) — confirming the root-cause fix.  It does NOT clear the storm:
still 0 epochs emitted.

---

## 11. Root cause and the co-gate

### 11.1 The smoking gun — a code↔phase REGISTRATION-GRANULARITY asymmetry

A 5-agent adversarial deep-dive (buildCodeBiasDatumRows / rank gate / code↔phase coupling
/ GIM-iono / phase-election) converged on one eigenvector pair:

```
CONTROL:  ev = 2.19e-11   [PB|C29|L2X = 0.65, CB|C29|C2X = 0.60]   ← coupled lump
CB-ADM:   ev = 6.75e-11   [PB|C29|L2X = 0.99]                      ← pure-PB orphan
```

The pure-`PB|` signals (L2X / L7X / L1I) are the exact phase counterparts of the
quarantined sparse code pairs (C2X / C7X / C1I), each tracked by ~2 stations.  β_g^s (the
phase bias) on such a signal is observable **ONLY via its same-signal code companion**
b_g^s through the shared clock; quarantining the code collapses the mixed lump onto β_g^s
alone → an isolated pure-PB gauge (ev ~ 1e-11).

### 11.2 The four eliminated alternatives (do not re-propose)

| candidate | eliminated by |
|---|---|
| s2z / IF-pin rank deficiency | refuted at ROW CONSTRUCTION — the IF pin is emitted per registered sat, s2z per channel to 1 member |
| iono / GIM error | an own-SH-VTEC run is IDENTICAL |
| the clock gauge | the 3 declared gauges sit cleanly at ~1e-9, well separated |
| per-epoch coverage | §10.8's byte-identical no-op |

### 11.3 ⚠ The co-gate LEVEL — learned by crashing the ON path

The co-gate must be applied at the `shared_phase_keys` ELECTION / phase-bias **STATE
ALLOCATION**, **not** at the column registration site.  Withholding a `registerPhaseBias`
for a key still present in `shared_phase_keys` breaks the *"every shared_phase_key is a
registered column"* invariant that eq34 / carry / snapshot rely on ⇒ **hard crash**
(confirmed both ways: a registration-site co-gate crashed the ON path, neutering it
restored the run).

`shared_phase_keys` is built from `getSatPhaseBiasStates()`
(`ApplicationNetwork.cpp:1628-1630`), gated by `allowed_phase_keys` in
`MeasurementModel::ensureNetworkSharedStates`.  Shipped: compute
`allowed_phase_keys = { phase key : its same-signal code companion is census-ADMITTED }`
and pass it as `cbadm_admitted_code_keys` (`MeasurementModel.hpp:101`,
`MeasurementModel.cpp:1148` / `:1228-1231`), fed from `networkPassAHead_` /
`networkPassASlipScan_`.  The quarantined signals then never allocate a phase-bias state
and `shared_phase_keys` excludes them consistently everywhere.

### 11.4 The residual is WEAK-BUT-REAL, not genuinely null

*(The adversarial split was recorded UNRESOLVED at the time — "RESIDUAL after the
co-gate (adversarial split, unresolved)" — and is resolved here in favour of dd_4.)*
Eigenvalue histogram of the co-gated below-cutoff set (first dump): **21 at
ev ∈ [1e-3, cutoff)**, 6 at [1e-6, 1e-3), 3 at [1e-9, 1e-6), 5 deep (< 1e-9 — the 3
declared gauges + ~2).  The 21 dominant ones are distributed across a BeiDou-3 MEO sat's
many signals; worked example:

```
ev = 4.41e-3  [PB|C26|L7D = 0.18, CB|C26|C2I = 0.18, PB|C26|L5P = 0.18,
               PB|C26|L1P = 0.18, CB|C26|C8X = 0.18]
```

The admitted BeiDou families ARE observable, but WEAKLY (~1e-3), and the single relative
cutoff `kNullSpaceTol · ev_max` = **6e-3** cannot span BeiDou's dynamic range —
weak-real ~1e-3 vs true gauge ~1e-9, with a clean spectral gap between.  So the residual
is a GATE-CALIBRATION / datum-regularization issue, not a genuine deficiency.

### 11.5 The complementary pre-filter (different granularity, not a substitute)

HEAD contains a cross-station observability pre-filter, `constexpr int
kMinStationsPerSat = 2` at `src/core/app/ApplicationNetworkAssembly.cpp:1523` (drop at
`:1534`), which drops a **SATELLITE** seen by < 2 stations ENTIRELY (clock + phase +
observations) — the "remove poorly-observed sats" intuition at SATELLITE granularity.
*(The HANDOFF's `ApplicationNetworkAssembly.cpp:747` anchor has drifted; symbol and
semantics unchanged.)*  It does NOT catch this storm, whose driver is **SIGNAL-level**
sparsity on multi-station sats (C06/C13/C16 are dense on C2I/C6I and 2-station on
C2X/C7X).  Raising `kMinStationsPerSat` would not help.  The per-(sat,signal) co-gate is
the signal-granularity complement.

---

## 12. Verdict and standing

1. **`cb_admission` code-only quarantine is NET-NEGATIVE on the real 305 storm**
   (22 → 43 ev<cutoff per dump).  Do NOT ship it without the phase co-gate; the two go
   together, quarantining a sparse signal on BOTH bias families.
2. **With the state-alloc co-gate it is net-positive-but-incomplete** (43 → 33; pure-PB
   orphans 89 → 24; shared columns 1805 → 1729) and still withheld every epoch on its own.
3. **SUPERSEDED-BY `docs/SPEC_beidou_w6_datum.md`.**  The 305 storm arm now emits **6/6
   dumps** via a FOUR-piece chain — CB-ADM + phase co-gate → `rank_gate_tol` 1e-12 →
   `cb_kaula_prior` σ = 10 m → PA heal — in which **each piece is NECESSARY and none alone
   emits**.  The two "alternative candidates" the HANDOFF offered (gate calibration OR a
   Kaula prior, "the Kaula prior is the cleaner first try") were both REQUIRED, not
   alternatives.  **⚠ Do not read "emits 6/6" as "BeiDou products are good" — product
   QUALITY is UNCLAIMED.**  See also `docs/CONFIG_promotion_ledger.md:1095`.
4. **The census + quarantine + per-epoch guard + co-gate are correct, default-OFF,
   byte-identical-OFF, unit-tested components** and are committed.
5. **`kMinStationsPerSat = 2` is complementary, not a substitute** (§11.5).

---

## 13. Erratum — how to read §10's tables (added 2026-07-22 by Card H3)

**Every A/B table above mixes two units, and they are not interchangeable.**  The
estimability dump is BUDGET-LATCHED: the producer records only the first
`spectrum_diag_budget` gate evaluations of a run (**6**, verified at
`include/app/NetworkRunContext.hpp:1913`).  A 305-station arm therefore yields exactly 6
dumps, and the same measurement can be quoted two ways:

| phrasing | actual unit | 305 control arm |
|---|---|---|
| "22 ev<cutoff", "22 → 43 → 33" | **per dump** | 22.5 |
| "135 below-cutoff directions", "135 → 258", the family tables (CB+PB 98, pure-CLK 16, …) | **across dumps** (6) | 135 |

The two differ by the factor 6.  Concretely: the control arm's `pure_CLK 16` across dumps
is 2.7 per dump — the **3 declared clock gauges**, one per constellation, minus the dumps
where the gate dropped one.  It is *not* 16 unexplained gauge directions.

- **Never compare a dump's `N ev<cutoff` with a withhold line's `observed M`.**  The
  withhold message reports the POST-heal re-gate — a different quantity on the same epoch
  (measured, control arm TOW 259200: dump **22**, withhold **14**, identical cutoff).  They
  are never summed or cross-validated.
- **Never judge an arm by its raw below-cutoff total.**  The population is four classes
  with four owners, and a *larger* total is an IMPROVEMENT when it trades genuinely-null
  directions for weak-but-real ones a calibrated cutoff can clear — exactly what control
  (22/dump) → co-gated (33/dump) did.

**Canonical unit-labelled presentation:** the four-class reconciliation table in
`docs/SPEC_beidou_w6_datum.md` §0.1.

**Reproducibility:** `python scripts/characterize_storm.py <log> --cross-check` (Card AN)
reproduces §10.6's control figures exactly from the archived logs — 135 directions across
6 dumps, BeiDou 123 / Galileo 6 / GPS 6, CB+PB 98 / pure_CLK 16 / CB+CLK+PB 13 / pure_CB 7
/ CLK+PB 1, 118 of 135 involving a `CB|` component, culprits C06 71 / C13 68 / C29 53 /
C16 49 — **18 of 18 oracle values PASS**.  The script's `ORACLE['source']` provenance
string must name **this** document (§10.6), not the atticked HANDOFF.
