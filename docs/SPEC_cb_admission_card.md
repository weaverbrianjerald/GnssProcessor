# SPEC — W6 code-bias admission census + station-local quarantine ("CB-ADM")

**Status:** SPEC (Jul-21 2026, Fable) — ready for implementation.  Targets the
BeiDou network blocker in its Jul-17 form (bias-datum sparsity: whole-epoch
withholds, `PB/CB|C##` null directions).  Default-OFF; OFF path byte-identical.

**One sentence:** replace the per-epoch instantaneous ≥2-station admission gate
on W6 satellite code-bias (OSB) columns with a **run-latched startup census**
per (sat, signal) — admitted pairs become shared border columns exactly as
today, while sub-threshold pairs keep their measurements and keep their
per-station sat-bias states as **station-local sponge states** (never
registered, never datumed, never emitted), so sparse BDS signals stop leaking
their OSBs into per-LOS iono AND stop injecting near-null directions that
withhold entire epochs.

**Line-number caveat:** anchors below are as of Jul-21 (tree mid-decomposition,
INC-0..8 landed).  Verify by symbol, not line, before editing.

---

## 0. Decision trail — do not relitigate

The user's design question was: gate BDS bias-state creation on a minimum
station count, or restrict BDS to well-tracked bands — and if we estimate code
biases only for well-observed states while keeping the signals, won't the
unmodeled biases leak into the ionosphere?  Analysis answered yes, and the
quarantine variant was **chosen by the user** over the alternatives.  Rejected,
with reasons — do not re-propose:

- **Keep measurement without any bias state** — the unmodeled OSB cannot sit in
  the residual; at a (station, sat) pair the network-shared states of the other
  signals are pinned by the rest of the network, so the per-pair sponges are
  slant iono (μ-slope signature) and the receiver bias (common mode).  The iono
  share is order d/Δμ: BDS OSB spreads are tens of ns (1 ns ≈ 0.3 m) against
  Δμ ≈ 0.25–0.78 across B1I/B3I/B2a/B1C ⇒ **meters** of slant-iono error at
  affected pairs, which re-enters phase with −μⱼ and wrecks that satellite's PB
  fractionals.  This is precisely what the current code does for sub-threshold
  pairs (§1.2) — at BDS scale it is not a "local nuisance".
- **Deweight instead of drop/quarantine** — the OSB is deterministic, not
  noise; weight scales the leak linearly but never removes it, and weakens
  estimability if the state exists.
- **Per-epoch (instantaneous) gating** — count flapping across the threshold
  ⇒ per-epoch prune/re-register/re-seed cycles ⇒ chain-datum churn, the
  project's standing plague.  The gate must be latched.
- **Static per-constellation band whitelist** — mishandles the BDS-2/BDS-3
  split (C2I/C6I/C7I birds vs C2I/C6I/C1P/C5P birds); the per-(sat,signal)
  census subsumes it.
- **Shared-column quarantine** (register the column, exclude from datum/
  spectrum/emission, hold with a loose SharedPrior) — a loose prior's info
  (1/σ² ~ 1e-3 at σ=30 m) sits numerically AT the rank-gate cutoff
  (`kNullSpaceTol = 1e-9`·σmax with σmax ~1e6), so it either trips the gate or
  survives by coincidence; making it structural means carving a checked-subset
  concept into the spectrum machinery.  The station-local variant (§2) needs
  none of that — local columns are eliminated in the Schur reduction and never
  enter the shared spectrum at all.

## 1. Motivation — current semantics, measured facts

### 1.1 The existing gate

`Application::networkAssemblyRegisterColumns_`,
`src/core/app/ApplicationNetworkAssembly.cpp` ~854–900: when
`network.estimate_sat_code_bias` is on, per epoch —

1. `cb_station_count["sat|sig"]` = number of **present** stations whose
   StateManager holds that sat-bias state (~873–882);
2. `keep_keys` = keys with count ≥ 2;
3. present stations register `keep_keys ∩ SM` into the shared border
   (`est.registerCodeBias`) and accumulate the per-epoch
   `registered_code_keys` (first-seen order — the vector is a **per-epoch
   local** of the run TU, `ApplicationNetwork.cpp` ~2157);
4. `sm.pruneSatBiasStatesExcept(station_keep)` deletes everything else
   (warm-cached: `StateManager.cpp` ~1886–1921, value+variance stashed for
   warm restore on re-acquisition).

The design comment at ~861–872 documents the sub-threshold fate explicitly: a
single-station code bias "stays a local nuisance (its sat-bias state is pruned
below, leaving its raw code row to fold into the receiver clock / slant iono —
the pre-W6 path)".  That is the leak of §0, by design — tolerable for rare
one-off GPS channels, poison at BDS scale where a large fraction of
(sat,signal) pairs sit at 1–2 stations and **flap** across the threshold.

### 1.2 The BDS failure chain

- Flapping across cnt≥2 ⇒ alternating epochs of {registered+datumed} vs
  {pruned+leaking}, with prune/warm-restore and Card-2 datum re-seed churn.
- Pairs that do register can still be datum-orphaned: a BDS sat whose OTHER
  signals stayed sub-threshold has <2 distinct-frequency registered signals ⇒
  `selectIfPinPair` cannot form its IF pin ⇒ its CB columns hang on the
  sum-to-zero channel row alone ⇒ near-null direction.
- The null-space rank gate withholds the **whole epoch** on any near-null
  direction (`ApplicationNetworkAssembly.cpp` ~147, ~1230;
  counter `ctx.counters.n_gate_withheld`, `ApplicationNetwork.cpp` ~423) —
  a handful of sparse BDS states has epoch-global blast radius.  This is the
  observed Jul-17 symptom: withholds every epoch, `PB/CB|C##` nulls named by
  the estimability dump.

### 1.3 Why the quarantine is nearly free (existing infrastructure)

- The shared scatter already has the local fallback: an SM sat-bias state whose
  key is NOT registered hits `est.codeBiasColumn` → throw → `catch { continue; }`
  (`ApplicationNetworkAssembly.cpp` ~130–138) and **stays a station-local
  column** in the station block.  Local columns are Schur-eliminated
  station-side; they never appear in the shared NEQ, the spectrum, or the rank
  gate.
- Sat-bias states are born as sponges already: WLS birth-seed absorbs the
  current residual (`seedSatBias`, `MeasurementModel.cpp` ~884; "the small
  (10 m) initial std lets future epochs refine the rest"), initial σ =
  `initial_std.sat_bias` (`StateManager::addSatBiasState`, `StateManager.cpp`
  ~1733), process noise `sat_code_bias_q` = 1e-10 m²/s
  (`GnssConfigTypes.hpp` ~1606–1616 — hardware delays drift ~mm/day).
- Everything downstream keys off `registered_code_keys` / the registered
  border: datum rows (`NetworkSbasis::buildCodeBiasDatumRows`,
  `NetworkSbasis.hpp` ~298, call site `ApplicationNetwork.cpp` ~3759–3775),
  Card-2 carry, estimability dump `col_desc`, the solution trace, snapshot
  emission (~7346–7349) and carry stash (~8210–8212).  **Not registering a key
  automatically excludes it from all of them** — no per-consumer edits.

So the entire behavioral change is ONE membership-test swap at the
registration site plus a census that computes the latched set.

## 2. Design (decisions, not options)

- **Admission unit:** the (sat, signal) code pair.  Two classes:
  **ADMITTED** (shared border column, datumed, emitted — identical to today's
  registered pairs) and **QUARANTINED** (measurements kept; per-station SM
  sat-bias states kept and NEVER pruned while the flag is on; never registered
  ⇒ station-local sponge).  Default class is QUARANTINED — pairs first seen
  after the census window are quarantined automatically.
- **Census:** a pure scan over the first K epochs the run loop will process
  (the synced vector is fully in memory — `MultiRinexSync::synchronize()`
  builds it at setup, `ApplicationNetworkSetup.cpp` ~259).  Latched once,
  immutable for the run: no promotions, no demotions, no per-epoch evaluation.
- **Criterion, tier 1 (count):** station st *supports* pair p iff st has a
  usable raw code observation for p in ≥ ⌈K_eff/2⌉ of the census epochs
  (absolute floor — avoids flimsy 2-of-2 ratios from gappy stations).
  Admitted-candidate iff #supporting stations ≥
  `effective_min = max(2, min(cb_admission_min_stations, n_census_stations))`
  (2 is the hard estimability floor per the existing design comment; the
  clamp keeps 3-station dev rosters runnable).
- **Criterion, tier 2 (IF-pinnability):** per satellite, among its tier-1
  candidates, require ≥ 2 DISTINCT carrier frequencies (SignalBand mapping —
  same distinctness the pin needs; C1P vs C1X same band ⇒ not distinct).  A
  satellite failing this has ALL its candidates demoted to QUARANTINED (logged
  per-sat: "no IF-pinnable pair") — this kills the §1.2 datum-orphan class at
  the source instead of letting the rank gate find it.
- **Emergent (intended) registration semantics when ON:** an admitted pair is
  registered whenever ≥ 1 present station holds its SM state — it stays
  registered straight through 1-station dips (datum rows still pin every
  registered column; Card-2 `cb_time_linked` carry supplies the prev-epoch
  target), which is exactly the anti-flap win.  A zero-present epoch drops the
  key for that epoch; the existing resume/re-seed machinery
  (`code_bias_datum_resumed_` / reseed scratch) already covers reappearance.
- **No new stochastic machinery:** quarantined states inherit today's birth
  seed, initial σ, `sat_code_bias_q`, and warm cache untouched.  No new prior
  knob (the 10 m `initial_std.sat_bias` is already the sponge prior).

## 3. Mechanism

### 3.1 New seam — `filter/NetworkBiasAdmission.{hpp,cpp}` (+ tests)

Decomposition-style seam (sibling of NetworkSbasis / NetworkPhaseDatum).  A
POD result + one pure static function; full Doxygen per house style.

```cpp
struct NetworkBiasAdmission {
    std::set<std::string> admitted;      // "sat|sig" keys, datum-grade
    // quarantined = everything else (implicit default; keep a census-time
    // list only for logging/counters)
    int n_admitted{0}, n_quarantined_census{0};
    bool active{false};                  // flag armed AND census ran
    bool isAdmitted(const std::string& key) const;
};

static NetworkBiasAdmission runCensus(
    const std::vector<SyncedEpoch>& synced,
    size_t i0,                // first epoch index the run loop will process
    int census_epochs, int min_stations,
    /* logger */);
```

Census window `[i0, min(i0+K, synced.size()))` — **i0 is the post-
`start_offset_hours` loop start**, not index 0 (a census over epochs the loop
never processes classifies on the wrong data; `ApplicationNetworkSetup.cpp`
~230 documents the fast-forward).  Usable-observation predicate over
`SyncedEpoch::by_station` → `RinexObservation::measurements`
(`RinexParser.hpp` ~46): 3-char id, `id[0]=='C'`, no `'_'`, finite nonzero
value; skip GLONASS satellites (no OSB scope — `ApplicationNetworkAssembly.cpp`
~129).  Deterministic iteration (std::map order) — the census must be
reproducible run-to-run.

Clamp: if the span has fewer than `census_epochs` epochs, K_eff = available
count (a 10-ep smoke with census_epochs=15 must still work, with a log line).

### 3.2 Config (conventions per CLAUDE.md; section `network`)

```
network.cb_admission                : bool, default false   (master enable)
network.cb_admission_min_stations   : int,  default 4
network.cb_admission_census_epochs  : int,  default 15
```

Getters `networkCbAdmission()`, `networkCbAdmissionMinStations()`,
`networkCbAdmissionCensusEpochs()`; parse in `loadNetworkSection_`
(`GnssConfigLoadNetwork.cpp`) via `jval`; add to the network summary
toggle-log line.  Naming mirrors `phase_datum_min_stations` /
`parallel_min_stations` precedent.

### 3.3 Call site — run TU, pre-loop

Compute once in the run TU before the epoch loop (after warm-resume, where the
loop start index is known), store the result in `NetworkRunContext`.  When the
flag is off, `active=false` and nothing else happens.

One-shot census log: per-constellation summary (pairs seen / admitted /
quarantined-count / quarantined-pinnability) + first-N per-pair table
(key, support count, verdict, reason).

### 3.4 Registration-site swap — `ApplicationNetworkAssembly.cpp` ~873–900

Under `if (config_.estimateNetworkSatCodeBias())`, branch on
`ctx.admission.active`:

- **OFF:** existing block byte-for-byte untouched (count → keep_keys →
  register → prune).
- **ON:** skip the count pass entirely.  For present stations, for each SM
  sat-bias state: always insert into `station_keep` (quarantined states are
  never pruned); if `admission.isAdmitted(key)` → `est.registerCodeBias` +
  `registered_code_keys` dedup-append, exactly as today.  Keep the
  `pruneSatBiasStatesExcept(station_keep)` call structurally (it becomes a
  no-op by construction; parse-failure keys keep today's fate).

Everything downstream is untouched by construction (§1.3).  The unregistered
states ride the existing `catch { continue; }` local fallback in the scatter.

### 3.5 Stale doc fix (rides the card)

`GnssConfig.hpp` ~1371–1374 still says the W6 scope is "GPS + Galileo" — stale
since the multi-band slice; BDS `C##` columns exist (they are the symptom).
Correct the Doxygen while touching the area.  Same for the producer's
withhold-path error HINT text, which still tells the user to set
`active_systems = GPS+Galileo` (grep the withhold/rank-gate log strings) —
under this card the accurate hint is the admission census / min-station knob.

### 3.6 Telemetry (standing invariant #4 — counters are the ON-path truth)

Run-end counters (NetworkRunContext::counters):

- `n_cb_admitted`, `n_cb_quarantined_census` (census close),
- `n_cb_late_quarantined` (distinct keys first seen post-census — count at the
  registration site: SM key parses, not admitted, not in census sets),
- existing `n_gate_withheld` — the primary success metric.

**Anti-inert check (hard smoke criterion):** a BDS-armed ON run with
`n_cb_quarantined_census == 0` means the census is miswired — FAIL the smoke
regardless of how clean the log looks.

## 4. What does NOT change

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

1. **Stiff time-linked quantities need a wired event response.**  No NEW
   zero-q carry is introduced: quarantined states inherit the identical
   process model (`sat_code_bias_q` 1e-10), birth-seed, and warm-cache
   lifecycle that pre-prune states have today.  The residual exposure — a
   satellite hardware bias step against a stiff local sponge — is strictly
   SMALLER than the status quo: today the entire OSB leaks into per-pair iono
   every epoch; under quarantine only the post-step delta leaks until
   re-convergence/re-seed.  Backstops: station-side innovation handling + D5.
   No new detector required.
2. **Release-only data runs.**  Every smoke/graded run uses
   `build/x64-Release`, renamed exe (§7).
3. **Deferrals triaged.**  (a) RT-producer arm of the census: DEFERRED,
   field-inert — the RT network arm does not run BDS W6 today; needs an
   arm-once census variant when it does (revisit at RTF).  (b) PB-side census
   threading: DEFERRED pending INC-2 attribution DATA (§4).  (c) Admission in
   the warm-resume snapshot: DEFERRED, field-inert for post-processing (§4).
4. **Run-end counters** specified (§3.6) with an explicit anti-inert FAIL
   criterion.
5. **Grade datum-invariant / SD quantities only** (§6 INC-3): DD fractionals
   (`scripts/grade_ddfrac.py`), median/std/worst-sat.  No raw wire-step
   grading.
6. **Default-OFF + byte-identity.**  OFF path structurally byte-identical
   (the registration block is branch-guarded); gate via the existing 10-arm
   matrix + estim arm harness (INC-5..8 pattern — see
   `docs/network_decomposition_addendum_2026-07-16.md`).  INC-1 is
   additionally byte-identical ON (census is observational).  Never compare
   across build configs.

## 6. Increments & gates

**INC-1 — census + config + logging (no consumers).**
`NetworkBiasAdmission` seam, config keys, run-TU pre-loop call, census
table log, counters plumbed.  Registration site untouched.
*Gates:* new unit tests green (census classification: sustained-support floor,
effective_min clamp incl. 3-station roster, tier-2 pinnability demotion,
late-seen default, K_eff clamp, GLONASS/`'_'`/non-code exclusion, determinism);
full suite green (pre-card expectation 805/804/1 — re-verify at start);
OFF byte-identity; **ON byte-identity** (census inert by construction) on a
BDS arm — this last gate proves the census wiring costs nothing numerically.

**INC-2 — registration swap.**
§3.4 branch + `n_cb_late_quarantined` counter + §3.5 doc fix.
*Gates:* unit test for the membership swap (existing network-estimator test
seams; quarantined SM state present + unregistered + scatter-catch exercised);
suite green; OFF byte-identity (10-arm matrix + estim arm);
ON BDS smoke = `_maxnet_305_gpsgalbds_d15_5ep` + `cb_admission` against the
KNOWN Jul-17 baseline (0/5 epochs emitted, withheld every epoch): success =
epochs emit, `n_gate_withheld` ~0, and the estimability dump's near-null set
reduces to the 3 declared clock gauges **plus any surviving `PB|C##`
directions** (expected — see §4; they are the phase side's share, not this
card's).  Census counters nonzero (anti-inert), no new disc events on
GPS/GAL, D5 flag counts ~parity.  State explicitly: ON is NOT byte-identical
even for GPS-only arms (latched census vs per-epoch counts differ by design)
— grade invariants, don't diff bytes.

**INC-3 — validation (graded).**
15-ep gate arm first; then ≥1h graded run **ON COMMAND ONLY** (staged-cadence
directive).  Grade: DD fractionals + median/std/worst-sat on GPS/GAL
unchanged-vs-control (BDS admission must not degrade them), BDS emission
present for admitted pairs, withhold telemetry over the full span, PB-vs-CB
null attribution (§4).  Deliverable: HANDOFF doc with measured numbers.

## 7. Run discipline & harness configs

- **The measured storm repro is the A/B baseline:**
  `config/test_suite/_maxnet_305_gpsgalbds_d15_{5,10}ep.json` (Jul-17 result
  KNOWN: zero solution epochs, whole-epoch withhold every epoch, 14→16
  near-null directions named `PB|C##`/`CB|C##` by the estimability dump;
  ~3.2 min/epoch ⇒ 5-ep arm ≈ 16 min).  ON arm = same config +
  `cb_admission` (+ `trace_estimability` for the attribution dump).  For
  cheap INC-1/INC-2 iteration, also derive a 50-sta variant
  (`_cbadm_50sta_15ep_gpsgalbds.json` from `_ppprtk50_6_gps_gal_bds.json`,
  `estimate_iono_sh` on) — but the card's proof A/B is the maxnet pair.  New
  harness configs get `parallel_carry=true` / `parallel_reduce=false`
  (all-parallel directive; the two are MUTEX — loader silently forces pcarry
  off if both set; grep the log to confirm).  Do NOT flip the `_wlver_*`
  family (it stays pcarry=false as the byte-id baseline).
- Build via the vcvars pattern (CLAUDE.md).  Data runs: Release only, renamed
  copy `gnss_processor_cbadm_run.exe`; pre-kill zombies NAME-SCOPED to that
  exe only (wildcard sweeps kill concurrent sessions' runs); LNK1104 = build
  lock → retry.
- `CMakeLists.txt`: explicit source adds for the new .cpp and test file (no
  globbing).
- The tree carries heavy uncommitted sibling work (UWL, lane-carry, Iono-2,
  decomposition).  Do NOT commit, revert, or "clean up" anything outside this
  card's files; leave the card's work uncommitted for user review.

## 8. Rulings reserved to the user (do not decide unilaterally)

- If INC-2 attribution shows persistent `PB|C##` nulls: report the spectrum
  dump and stop — pdp-threading is a separate card.
- Any deviation from the §2 criteria (min_stations default, sustained floor,
  tier-2 rule) discovered necessary during implementation: report with data,
  propose, await ruling — per the falsifier discipline, spec deviations are
  measured and documented, not silently absorbed.
