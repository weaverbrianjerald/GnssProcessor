# SPEC — Decomposition INC-8b: phase-datum election split

**Status: THE CUT IS LANDED (2026-07-22), UNCOMMITTED AND NOT YET BYTE-GATED.**
Written 2026-07-21 from an 8-agent survey + xhigh synthesis critic + 4-lens
adversarial verify (7+8+5+5 defects; **4 blocking, 9 major** — all folded in
below and marked ⚠**AV**).

## LANDED — what actually happened

Executed on user instruction to prioritize size reduction over gating; **8b.0
(the four new gate arms) was authored but NOT captured**, so the byte gate has
not run. Sub-steps were NOT staged separately — the cut landed as one
scripted transaction because the run TU was under concurrent edit and the
excision had to occupy the smallest possible window.

| Metric | Value |
|---|---|
| `ApplicationNetwork.cpp` | **9,456 → 8,682 lines (−774)** |
| `NetworkPhaseDatum.cpp` | 69 → 279 (+210: census, ensemble datum, anchor election) |
| `NetworkChainDatum.{hpp,cpp}` | NEW, 105 + 712 lines (eq34 chain election) |
| Relocated verbatim | builder 96 + ensemble 14 + eq34 637 + anchor 48 = **795 lines** |
| Verbatim proof | all four bodies byte-identical (md5, post-hoc re-verify) |
| Build | clean **first try**, both seams and the excision |
| Suite | **865 / 864 pass / 1 skip** — unchanged from pre-cut |

**Method (why the transcription is trustworthy).** Three scripted phases, the
INC-6 precedent: EXTRACT (21 guard anchors asserted against expected text,
measured per-block dedent, md5 per body) → ASSEMBLE (bodies inserted by script,
nothing hand-typed; alias preamble derived MECHANICALLY by scanning the run TU's
own `auto& X = ctx.carry.X;` block and keeping only names the body references)
→ EXCISE (13 guards, single write). A post-hoc re-verify re-extracted each block
from the run TU and proved the generated seam contains it byte-for-byte.

**The mechanical alias derivation independently reproduced §3.4 exactly**: eq34
references 22 ctx aliases (18 `carry` + 4 `knobs`), anchor exactly 2, ensemble
exactly 1 — and `pl_max_gap_s` is absent, confirming that AV minor finding from
the source rather than by argument.

**Deviation from §3.1/D12 (improvement).** The builder keeps the five census
declarations as locals byte-identically (sizing ctor args intact) and hands them
over in a 5-line `std::move` **epilogue**, rather than the sizing/alias preamble
the AV proposed. This makes the builder body a *pure* dedent — the AV named it
"the only strictly-dedent-only option" — and freshness is still structural,
since move-assignment fully overwrites each member.

**⚠ VERIFICATION STATUS — read before trusting this.** The cut is proven
*structurally* (verbatim bodies, clean build, unchanged suite) but **not
functionally**: no golden arm has been run, and no test in the suite exercises
any of the four relocated bodies. The 865-test pass proves linkage, not
behavior. Before this is field-usable: capture 8b.0's four arms (§6.2), run the
17-arm matrix (§6.4), and add the §7 tests — of which Tier-1 items 5–8 cover the
ensemble/anchor path that **no arm in the matrix executes at all**.

**Predecessor:** INC-8 (`30684a1`) deliberately did NOT move the anchor election,
recording "splitting the 4-way legacy/ensemble/eq34/default chain across TUs =
8b-time ruling". This spec is that ruling.

**Design authority:** `docs/network_decomposition_plan.md` §2-6;
working reference `docs/network_decomposition_addendum_2026-07-16.md` (its §4
anchor row for 8b, `4919-5556`, is stale by ~-3,400 — refresh it at 8b.iv).

---

## ⚠ SNAPSHOT WARNING

Every line number below is a **timestamped snapshot, not an anchor**.
`ApplicationNetwork.cpp` measured **9,135 → 9,162 → 9,336 lines within a single
session** (concurrent CB-ADM). §0 is mandatory at increment start **and between
every sub-step**. Never paste a line number from this document into a cut.

---

# 0. RE-ANCHOR FIRST (mandatory, ~30 s)

```bash
cd C:/GnssProcessor
wc -l src/core/app/ApplicationNetwork.cpp
grep -nF 'std::set<std::string> shared_phase_keys;'                        src/core/app/ApplicationNetwork.cpp
grep -nF 'if (!phase_datum_prioritized) {'                                 src/core/app/ApplicationNetwork.cpp
grep -nF 'const double pref_el = config_.networkPhaseDatumPrefElevDeg();'  src/core/app/ApplicationNetwork.cpp
grep -nF 'const bool   sticky  = config_.networkPhaseDatumSticky();'       src/core/app/ApplicationNetwork.cpp
grep -nF 'std::map<char, std::set<size_t>> cand_set;'                      src/core/app/ApplicationNetwork.cpp
grep -nF 'if (phase_datum_ensemble) {'                                     src/core/app/ApplicationNetwork.cpp  # 2 hits — take the FIRST
grep -nF '} else if (phase_datum_eq34) {'                                  src/core/app/ApplicationNetwork.cpp
grep -nF 'auto recordEdge = [&](size_t ri, const std::string& sat,'        src/core/app/ApplicationNetwork.cpp
grep -nF 'anchor_incumbent = this_epoch_anchor;'                           src/core/app/ApplicationNetwork.cpp
grep -nF 'std::map<char, std::vector<size_t>> cand;'                       src/core/app/ApplicationNetwork.cpp
grep -nF 'anchor_incumbent.swap(new_incumbent);'                           src/core/app/ApplicationNetwork.cpp
grep -nF 'end !phase_datum_ensemble (anchor election)'                     src/core/app/ApplicationNetwork.cpp
grep -nF 'Card 3 (eq34) RPB allocation'                                    src/core/app/ApplicationNetwork.cpp
grep -nF 'if (networkAssemblyRegisterColumns_(se, ctx, setup,'             src/core/app/ApplicationNetwork.cpp
```

All patterns verified UNIQUE except `if (phase_datum_ensemble) {` (2 hits: the
election + `buildEnsembleGauges`).

## 0.1 Region map — snapshot 2026-07-21 23:02, file length 9,162

⚠**AV**: guards and closers **STAY**; only bodies move. Dedent depths differ per
block — **never apply a blanket dedent across the region**.

| Block | Lines | Len | Indent | Disposition |
|---|---|---|---|---|
| `shared_phase_keys` decl | 1382 | 1 | 8 | **STAYS** |
| INC-7 seam calls + SB brackets | 1383–1415 | 33 | — | **STAYS** |
| chain head `if (!phase_datum_prioritized) {` | 1416 | 1 | 8 | **STAYS** |
| legacy body | 1417–1419 | 3 | 12 | **STAYS** |
| prioritized `} else {` | 1420 | 1 | 8 | **STAYS** |
| prologue frag A (`pref_el`, `pdp_min_sta`) | 1421, 1423 | 2 | 12 | → 8b.i |
| ⚠ `const bool sticky = …` | **1422** | 1 | 12 | **STAYS** (anchor-only consumer) |
| prologue frag B | 1424–1460 | 37 | 12 | → 8b.i |
| ⚠ `cov` lambda | **1461–1464** | 4 | 12 | **RE-DECLARED** in 2 seams; transient run-TU copy deleted at 8b.iii |
| prologue frag C | 1465–1521 | 57 | 12 | → 8b.i |
| ⚠ ensemble **guard** | **1523** | 1 | **12** | **STAYS** |
| ensemble **body** | **1524–1537** | 14 | 16 | → 8b.ii |
| ⚠ eq34 **guard** `} else if (…) {` | **1538** | 1 | 12 | **STAYS** |
| eq34 **body** | 1539–2175 | 637 | 16 | → 8b.iii |
| ⚠ anchor **guard** `} else {` | **2176** | 1 | 12 | **STAYS** |
| anchor **body** (**under-indented**) | 2177–2224 | 48 | **12** | → 8b.ii |
| chain close (stale comment) + `}` | 2225–2226 | 2 | 12/8 | **STAYS** (comment corrected, D10) |
| Card-3 D3c RPB allocation | 2228–2255 | 28 | 8 | **STAYS** (D1) |
| `networkAssemblyRegisterColumns_` | ~2266 | — | — | **STAYS — hard upper bound of the cut** |

The anchor body sits at the **same column as its own `} else {` and its own
closing `}`** (verified: 2176 `            } else {`, 2177
`            std::map<char, std::vector<size_t>> cand;`). Dedent 12→4, not 16→4.

## 0.2 Control-flow fact

**NO** epoch-level `continue`/`break`/`return`/`goto` anywhere in 1416–2255 (~60
hits, all inner-loop or lambda/comparator returns). **Unlike INC-7, 8b needs no
`EpochAction`** — every seam is plain `void`.

---

# 1. THE RULING

| Question | Ruling |
|---|---|
| Anchor election moves with eq34? | **No — separate sub-step, separate TU.** |
| One destination or two? | **Two.** eq34 → NEW `filter/NetworkChainDatum`; ensemble + anchor + prologue → EXISTING `filter/NetworkPhaseDatum`. |
| Seam shape | **Free functions**, params named exactly as the former run-TU locals (INC-4/6 convention). eq34 additionally takes `NetworkRunContext& ctx` + alias preamble. |
| Prologue | **Moves once**, as its own seam fn filling `PhaseDatumCandidates`. Both elections consume it by const ref. **No duplication.** |
| D3c RPB allocation / legacy 3-liner | **Stay.** |

### Why free functions (not INC-7 member fns, not INC-7b builder)

- **Capture evidence.** Exactly **six** `Application`-member expressions across
  ~870 lines: `config_.networkPhaseDatumPrefElevDeg/Sticky/MinStations()`
  (prologue), `config_.networkPhaseDatumEq34Receiver/Eq34PinRound()` (eq34), one
  `log_manager_` (lazy `make_unique<PppUtils>`). Zero `gim_`, zero `traces`, zero
  `est`/`ws`/`setup`/`emitEvent`. The **anchor body alone has zero of all six** —
  literally the INC-7b evidence pattern. That is not INC-7's "config_/gim_/
  log_manager_-dense" profile.
- **Not INC-7b's builder either.** `CouplingRowBuilder` won because it had
  per-instance scratch constructed per-station inside a loop. Here every scratch
  container is declared and consumed inside one straight-line call invoked **once
  per epoch**, and eq34's lambdas (`recordEdge` ~20 captures, `attach`, `arcLife`,
  `seedEdge`, `find`/`unite`) close over **per-group / per-component** locals
  (`grp_cons`, `grp_sig`, `system`, `rpb_key`, `r1`, `order`). Flattening those
  into builder members forces loop-scoped values to become members reassigned per
  iteration — a semantic edit, not a dedent.
- **Testability forecloses member fns independently.** No test in the tree
  constructs an `Application` (4 grep hits, all comment text). The member-fn shape
  would make the 8b seam untestable by every pattern the suite uses.
  `tests/test_network_phase_datum.cpp:14` already reserves itself for 8b, and
  `include/filter/NetworkPhaseDatum.hpp:22-25` already states the shape invariant:
  *"These functions own NO state: every carry/workspace object is an explicit
  reference into the caller's NetworkRunContext."*
- **`ctx` by reference for eq34 only.** The eq34 body touches ~19 `ctx.carry` maps
  + 5 counters. Enumerating them as by-ref params creates a **compiles-clean,
  numerically-wrong** failure class: `local_carry`/`rpb_carry`/`prev_carry` are
  value-mutated (`lc.value_abs += d;`) and copyable, so one dropped `&` silently
  deletes the S-transform. Taking `ctx&` once removes ~24 chances to make it.
  Anchor/ensemble need only two maps → they take them explicitly, so
  `NetworkPhaseDatum.hpp` stays free of any `app/` dependency.

### Why the prologue becomes its own seam (not driver-side, not duplicated)

Consumption is asymmetric but genuinely shared: `union_keys` → all three
sub-branches; `usable_keys` + `cov` → eq34 and anchor; `sat_el` → eq34 only;
`cand_set` + `sticky` → anchor only. Nothing in any sub-branch writes it. Two
facts force it into a named seam: (1) it is the **sole writer** of
`n_gated_epochs`/`n_gated_key_epochs`, so any arrangement that can compute it
twice double-counts invariant-#4 telemetry; (2) it **mutates StateManager**
(`pruneSatPhaseBiasStatesExcept`) — an invisible side effect both elections
depend on. Naming it documents that ordering.

### Rejected alternatives

1. Member fns of `Application` — see above.
2. Builder struct — see above.
3. Enumerating eq34's ~19 carry maps as params — the dropped-`&` class.
4. One monolithic move of 1416–2226 — puts a 637-line Card-3 chain machine into a
   TU named for phase-datum anchoring, and still byte-gates the anchor half on
   arms that never execute it.
5. Prologue driver-side, 6 loose containers to both seams — hides the
   StateManager-mutating gate as an unnamed precondition.
6. Hoisting the 5 `config_` reads into `ctx.knobs` — touches
   `NetworkRunContext.{hpp,cpp}` (8 concurrent hunks) for zero verbatim benefit;
   `eq34_round` is composite (`… && !carry_invariant`) and a naive hoist drops it.
   → **D3**.
7. `cov` as a member or `std::function` — re-declaring the 4-line pure lambda is
   the only strictly-verbatim option. → **D4**.

---

# 2. SUB-STEP STAGING

Each sub-step: build (Debug `gnss_core` + `gnss_tests`; Release
`gnss_processor_8b.exe`) → full gtest suite green → **its own additive tests
green in the same commit** → the stated byte check → commit. **Never combine
sub-steps.**

### 8b.0 — GATE ARMS FIRST (no `src/` edit)

Author the 4 arm configs (§6.2), register them in `scripts/run_inc4_gate.ps1`
with their invariant-#4 assertions, and capture goldens **from the PRE-cut
binary** (INC-8 precedent).

⚠**AV (blocking ×2):**
- **Capture `_a` AND `_b` for every new arm.** The harness compares golden
  **directories**, not manifests: `run_inc4_gate.ps1:35` passes `$a.g` =
  `runs/decomp_goldens/<arm>_a` to `compare_network_run.py`; the `--manifest`
  mode is separate and unused. All 13 existing arms have `_a` + `_b`. Assert
  `compare <arm>_a <arm>_b --maskspec goldens/decomp/maskspec.json` is clean
  **before any `src/` edit** — otherwise the new arms become the sole byte
  witness for 8b.ii without ever being proven reproducible. Keep
  `goldens/decomp/<arm>.manifest.json` as archival only and say so.
- **Parameterize the exe.** `run_inc4_gate.ps1:5` hardcodes
  `build/x64-Release/gnss_processor_dcmp.exe` with no override. Following §9.4's
  renamed-exe directive while leaving the script alone byte-compares a *stale
  binary against itself* — the strongest possible false green. Add
  `param([string]$Exe = "build/x64-Release/gnss_processor_dcmp.exe")` and pass
  `-Exe build/x64-Release/gnss_processor_8b.exe` for the whole series. Write a
  provenance stamp (exe SHA-256, `git rev-parse HEAD`, hash of `git diff` over
  `src/ include/`) into every new golden dir.

**Byte-checkable:** nothing moved; the 13 existing arms must still pass unchanged
(proves the harness edit is inert). Record every non-vacuity counter value from
the fresh goldens in the commit message.

### 8b.i — Vocabulary + SHARED PROLOGUE

1. `PhaseDatumCandidates` → `include/filter/NetworkRunTypes.hpp`, in a NEW
   trailing section **far from `ShDevCarry`** (Iono-2 has a 15-del/56-add rewrite
   there).
2. `networkBuildPhaseDatumCandidates` → `filter/NetworkPhaseDatum.{hpp,cpp}`.
   Relocate **1421 + 1423–1460 + 1465–1521** (dedent 12→4). ⚠**AV**: the range is
   **three fragments**, not contiguous — 1422 (`sticky`) stays, 1461–1464 (`cov`)
   is re-declared.
3. ⚠**AV (blocking):** **the builder body is NOT a pure dedent.** The five census
   objects are *declarations*, two carrying a sizing ctor arg
   (`sat_el(stations.size())`, `usable_keys(stations.size())`). Aliasing them
   naively loses the sizing and `sat_el[i]` at :1440 becomes an unchecked
   out-of-bounds write at i≤49 — heap corruption in Release, no compile error, no
   gate signal. Open the builder with an explicit, **stated-as-deviation** sizing
   preamble, then alias:
   ```cpp
   cnd.sat_el.assign(stations.size(), {});
   cnd.usable_keys.assign(stations.size(), {});
   cnd.cover.clear(); cnd.cand_set.clear(); cnd.union_keys.clear();
   auto& sat_el      = cnd.sat_el;
   auto& usable_keys = cnd.usable_keys;
   auto& cover       = cnd.cover;
   auto& cand_set    = cnd.cand_set;
   auto& union_keys  = cnd.union_keys;
   ```
   Everything below that block is then strictly dedent-only. This also
   re-establishes **structurally** the freshness that is today guaranteed by
   construction scope — otherwise a later hoist of `cnd` out of the epoch loop
   (permitted by the signature) turns `cover`/`cand_set`/`union_keys` into
   accumulating cross-epoch maps, and nothing in the moved body ever clears them.
   → **D12**.
4. Run TU: keep `sticky` at its original line, call the builder, bind the alias
   block + the transient `cov` copy so **all three sub-branch bodies are untouched
   this step**:
   ```cpp
   } else {
       const bool   sticky  = config_.networkPhaseDatumSticky();
       PhaseDatumCandidates cnd;
       networkBuildPhaseDatumCandidates(config_, stations, present, modeled, cnd,
                                        n_gated_epochs, n_gated_key_epochs);
       const auto& sat_el      = cnd.sat_el;
       const auto& usable_keys = cnd.usable_keys;
       const auto& cover       = cnd.cover;
       const auto& cand_set    = cnd.cand_set;
       const auto& union_keys  = cnd.union_keys;
       auto cov = [&](char c, size_t i) -> int {        // byte-copy of the original
           auto it = cover.find(c); if (it == cover.end()) return 0;
           auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
       };
       if (phase_datum_ensemble) { … unchanged … }
   ```
5. Tests 20/21 + the `cnd.sat_el.size() == stations.size()` assertion land here.

**Byte-checkable:** 12 eq34 arms + `pdanchor` + `pdgate` + `pdens`. `alloff` never
enters the prologue (null check — keep it anyway).

### 8b.ii — ENSEMBLE + ANCHOR ELECTION → NetworkPhaseDatum

Relocate **1524–1537** (dedent 16→4) and **2177–2224** (dedent **12→4**). Replace
with two call sites; guards 1523 / 1538 / 2176 / 2225 stay. Correct the stale
closing comment to `// end anchor election (!ensemble && !eq34)` in the run TU
only; do not copy it into the seam (**D10**).

⚠**AV (major):** the cut range is **1524**–1537, not 1523. Line 1523 is the
*guard* at indent 12; lifting it leaves an `else if` with no preceding `if`. The
dangerous repair — calling the ensemble seam unconditionally inside the
prioritized `else` — runs the pin-clear + `this_epoch_anchor.clear()` +
`shared_phase_keys = union` on **every eq34 and anchor epoch**, destroying the
datum before D3c and `registerColumns` read it.

⚠**AV (major):** tests 5/6/7/8 land **in this commit**, not deferred to 8b.iv.
A sub-step is not done until its own additive tests are green in the same commit.

**Byte-checkable:** `pdanchor`, `pdgate`, `pdens` **ONLY**. **All 13 existing arms
are structurally blind to this step** — that is why 8b.0 exists. Re-run the full
17 anyway to prove the eq34/legacy paths stayed untouched.

### 8b.iii — eq34 CHAIN ELECTION → NEW filter/NetworkChainDatum

Create `include/filter/NetworkChainDatum.hpp` + `src/core/filter/NetworkChainDatum.cpp`;
add to `CMakeLists.txt`. Relocate **1539–2175** verbatim (dedent 16→4) behind the
§3.4 alias preamble. Delete the transient run-TU `cov` + the five aliases once
both consuming bodies are gone.

⚠**AV (major): STRIKE the `GKey` hoist.** The former D5 ("function-local types
cannot cross a TU boundary") rests on a false premise — local types have been
legal template arguments since C++11 (N2657), and a whole-file grep shows all six
`GKey` references live inside the moved body (1592–1607). Leave `struct GKey`
function-local inside `networkElectChainDatum`. This keeps 1539–2175 **strictly
dedent-only**, so R5's whitespace-normalized diff applies to the largest and
least-reviewable move in the increment.

⚠**AV (major):** tests 1/2/3/4, 12, 22–27 land **in this commit**.

**Byte-checkable:** 12 eq34 arms + `cpllive`/`cpljw` (pin-to-round) + `eq34x`.

### 8b.iv — DOCS ONLY

Update the `@file` banners (`NetworkPhaseDatum`: replace "the anchor election …
complete the seam at INC-8b" with landed provenance). Record §5 deviations in
`docs/network_decomposition_addendum_2026-07-16.md` and refresh its stale §4
anchor row. Add test 24 (§7 Tier 3) if Open Question 3 resolves that way.

**Byte-checkable:** no `src/` behavior change → suite green + 2-arm spot check.

**Ordering rationale:** the gate-blind half (8b.ii) lands second, immediately
after its arms exist and while the diff is 62 lines; the 637-line eq34 move lands
third, against a matrix already proven to cover both halves.

---

# 3. EXACT SEAM SIGNATURES

## 3.1 `include/filter/NetworkRunTypes.hpp` (append, away from `ShDevCarry`)

```cpp
/// Epoch-scratch census consumed by every prioritized phase-datum realization
/// (ensemble / eq34 chain / anchor election).  Built once per epoch by
/// @ref networkBuildPhaseDatumCandidates; nothing downstream of the election
/// reads it.  Member names are the former run-TU local names so relocated
/// bodies alias them without renaming.
struct PhaseDatumCandidates {
    /// Per station: satellite -> elevation (deg), from the PRUNED obs.
    std::vector<std::map<std::string, double>>      sat_el;
    /// Per station: "sat_signal" -> "sat|signal|PB" (PASS-A allocated states).
    std::vector<std::map<std::string, std::string>> usable_keys;
    /// constellation -> station -> # sats tracked at >= pref_elev.
    std::map<char, std::map<std::size_t, int>>      cover;
    /// constellation -> candidate station indices.
    std::map<char, std::set<std::size_t>>           cand_set;
    /// "sat_signal" -> "sat|signal|PB", coverage-gated union over present stations.
    std::map<std::string, std::string>              union_keys;
};
```

## 3.2 `include/filter/NetworkPhaseDatum.hpp` (add three fns)

New includes: `"filter/NetworkRunTypes.hpp"` + `<map>`. Forward-declare
`class GnssConfig;` and `struct PppObservation;` — **do not** include `app/` or
`config/GnssConfig.hpp` here (the `.cpp` does). This keeps the file's existing
"owns NO state / explicit refs" invariant literally true.

```cpp
/**
 * @brief Build this epoch's prioritized phase-datum candidate census and apply
 *        the network.phase_datum_min_stations coverage gate.
 *
 * Relocated VERBATIM from Application::runNetwork at INC-8b.  Runs for every
 * prioritized realization (ensemble / eq34 / anchor).  SIDE EFFECT: when the
 * gate fires it PRUNES each tracking station's satellite phase-bias states
 * (StateManager::pruneSatPhaseBiasStatesExcept) so per-station column sets stay
 * consistent with the gated union — both elections depend on that prune having
 * already happened.
 *
 * @param cnd  [out] census; every member is FULLY OVERWRITTEN (sized/cleared at
 *             entry), so caller storage may be reused.
 */
void networkBuildPhaseDatumCandidates(
        const GnssConfig& config_,
        std::vector<NetStation>& stations,              // non-const: the gate prunes PB states
        const std::vector<bool>& present,
        const std::vector<std::vector<PppObservation>>& modeled,
        PhaseDatumCandidates& cnd,
        std::size_t& n_gated_epochs,
        std::size_t& n_gated_key_epochs);

/**
 * @brief Fix-A ANCHOR-FREE ensemble datum: shared set = the (post-gate) union,
 *        every station's pins cleared, no anchor elected — the gauge is the
 *        kernel sum-to-zero fold built AFTER the reduce by buildEnsembleGauges.
 *        Relocated VERBATIM at INC-8b.
 */
void networkElectEnsembleDatum(
        std::vector<NetStation>& stations,
        const PhaseDatumCandidates& cnd,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys);

/**
 * @brief Prioritized per-(sat,signal) ANCHOR election with stickiness.
 *        Relocated VERBATIM at INC-8b.
 *
 * Keeps the sticky incumbent while it is present AND still holds the key, else
 * takes the highest-coverage candidate of that constellation (stable_sort by
 * descending coverage then lexical station name).  The pin lands in the
 * anchor's OWN pinned_amb_targets (target 0.0) — the load-bearing local-block
 * invariant.  A key with no tracker is dropped from @p shared_phase_keys
 * entirely, which keeps the shared set and the pin set in bijection.
 *
 * @param anchor_incumbent  [in/out] sticky baseline; swapped at the end.  MUST
 *                          be the run's ctx.carry instance — a copy silently
 *                          degrades the election to fresh every epoch.
 */
void networkElectPhaseAnchors(
        bool sticky,
        std::vector<NetStation>& stations,
        const std::vector<bool>& present,
        const PhaseDatumCandidates& cnd,
        std::map<std::string, std::string>& anchor_incumbent,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys);
```

## 3.3 `include/filter/NetworkChainDatum.hpp` (new)

```cpp
#pragma once
/**
 * @file NetworkChainDatum.hpp
 * @brief Card-3 Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN phase-bias datum for the
 *        PPP-RTK network producer.
 *
 * Per (constellation,signal) connected component of the bipartite
 * (receiver x satellite) graph: elect a datum receiver r1 (its RPB pinned to 0)
 * and pin the ambiguities on a STICKY spanning tree rooted at r1, so every
 * between-satellite reference is an integer combination across the whole
 * component.  With n_c stations and m_c sats a component contributes
 * (n_c+m_c-1) tree-edge pins + 1 RPB pin = n_c+m_c = the exact gauge dimension
 * (cross-checked every epoch).
 *
 * Seeded at decomposition INC-8b, which relocated the eq34 election VERBATIM out
 * of Application::runNetwork.  Sibling of @ref NetworkPhaseDatum (ensemble +
 * anchor election + drop-and-heal).  Owns NO state: every carry object is an
 * explicit reference into the caller's NetworkRunContext.
 *
 * ORDERING CONTRACT — it MUTATES persistent carries PRE-SOLVE (erases
 * local_carry for re-referenced (sat,sigma), erases/shifts rpb_carry, shifts
 * prev_carry PhaseBias entries, fills ctx.carry.eq34_reref):
 *   - AFTER  networkAssemblyPrefilter_;
 *   - BEFORE the Card-3 D3c RPB allocation — this seam is the SOLE writer of
 *     shared_phase_keys on the eq34 path and D3c derives this epoch's need_rpb
 *     set from it;
 *   - D3c in turn must precede est.reset()/est.beginEpoch() inside
 *     networkAssemblyRegisterColumns_;
 *   - BEFORE networkStashCarry and networkInjectD2CarryPrior.
 * The I2 extend-only merge must stay BEFORE `anchor_incumbent = this_epoch_anchor;`.
 */

#include "filter/NetworkRunTypes.hpp"   // NetStation / PhaseDatumCandidates / LocalCarry

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class LogManager;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only

/**
 * @brief Elect the eq34 chain datum for this epoch: reset pins, seed the shared
 *        set from the (post-gate) union, build the per-group graph, elect r1 and
 *        grow the sticky spanning tree per component, then run the carry
 *        hygiene (re-reference erase + S-transformation apply + I2 extend-only
 *        merge-back) and advance the sticky baselines.
 *
 * @param ctx           [in/out] carry + counters; the body aliases the former
 *                      run-TU local names off ctx.carry (INC-4 mechanic).
 * @param log_manager_  Sole Application dependency: the LAZY
 *                      ctx.carry.eq34_ppp_utils construction.  Do NOT hoist it
 *                      to the caller — PppUtils' ctor logs, and the pointer is
 *                      re-read at the emit-rebase site.
 */
void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            LogManager& log_manager_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            std::set<std::string>& shared_phase_keys);

} // namespace GnssProcessor
```

⚠**AV (major): terminology.** There is exactly ONE `union_keys` (declared :1466)
and the pdp gate erases from it **in place** (:1473-1479); eq34 re-filters against
the same gated map at :1603. Both branches seed from the identical object. The
word **"ungated" must not appear** in either seam's docs — a written licence to
hand ChainDatum a pre-gate census re-admits every 1-tracker (sat,signal) into
`shared_phase_keys`, which D3c reads three lines downstream to allocate RPB, i.e.
the exact non-PD / one-station-re-level pathology documented at :1481-1493. State
once in `PhaseDatumCandidates`' doc: *"`union_keys` is post-gate; there is no
pre-gate copy and none may be added."* Correct the stale in-source comment at
:1552 in the same commit (**D13**).

`.cpp` includes: `filter/NetworkChainDatum.hpp`, `app/NetworkRunContext.hpp`,
`config/GnssConfig.hpp`, `filter/StateKeySchema.hpp`, `common/PppUtils.hpp`,
`logging/LogManager.hpp`, `<algorithm> <cmath> <deque> <limits> <tuple>`.
⚠**AV (minor):** `<deque>` not `<queue>`; `<limits>` is needed. Verify by grepping
the moved range for `std::` qualifiers rather than transcribing.

No cycle: no `filter/` or `slip/` header includes `app/` today, and
`NetworkRunContext.hpp` does not include the new header — the forward declaration
keeps it that way permanently.

## 3.4 Alias-preamble strategy

Every relocated body opens with an alias block that is a **byte-copy of the
run-TU original**, so the body below it is dedent-only.

`networkElectChainDatum` preamble (order = the run-TU alias order):

```cpp
    auto& prev_carry            = ctx.carry.prev_carry;
    auto& anchor_incumbent      = ctx.carry.anchor_incumbent;
    auto& this_epoch_anchor     = ctx.carry.this_epoch_anchor;
    auto& eq34_datum_rx         = ctx.carry.eq34_datum_rx;
    auto& eq34_rx_parent        = ctx.carry.eq34_rx_parent;
    auto& eq34_sat_root         = ctx.carry.eq34_sat_root;
    auto& eq34_prev_el          = ctx.carry.eq34_prev_el;
    auto& eq34_ppp_utils        = ctx.carry.eq34_ppp_utils;
    auto& eq34_reref_total      = ctx.carry.eq34_reref_total;
    auto& eq34_erase_total      = ctx.carry.eq34_erase_total;
    auto& eq34_shift_total      = ctx.carry.eq34_shift_total;
    auto& eq34_gap_repin_total  = ctx.carry.eq34_gap_repin_total;
    auto& eq34_mismatch_epochs  = ctx.carry.eq34_mismatch_epochs;
    auto& eq34_retained_edges   = ctx.carry.eq34_retained_edges;
    auto& eq34_prev_target      = ctx.carry.eq34_prev_target;
    auto& eq34_reref            = ctx.carry.eq34_reref;   // ctx.carry — 3 downstream consumers in 2 TUs
    auto& local_carry           = ctx.carry.local_carry;
    auto& rpb_carry             = ctx.carry.rpb_carry;
    const bool carry_invariant   = ctx.knobs.carry_invariant;
    const bool eq34_shift_on     = ctx.knobs.eq34_shift_on;
    const bool eq34_longevity    = ctx.knobs.eq34_longevity;
    const bool eq34_extend_only  = ctx.knobs.eq34_extend_only;
    const auto& sat_el      = cnd.sat_el;
    const auto& usable_keys = cnd.usable_keys;
    const auto& cover       = cnd.cover;
    const auto& union_keys  = cnd.union_keys;
    auto cov = [&](char c, size_t i) -> int {              // byte-copy; sibling copy in NetworkPhaseDatum
        auto it = cover.find(c); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };
```

⚠**AV (minor ×3):** the knob set is **exactly four** — `carry_invariant`,
`eq34_shift_on`, `eq34_longevity`, `eq34_extend_only`. **Do NOT add
`pl_max_gap_s`**: the I2 extend-only liveness test is membership-only
(`local_carry.count(...)` at :2113); the `phase_leveling_max_gap_s` horizon is
applied at end of epoch by the `local_carry` prune in `NetworkRunContext`.

`networkElectPhaseAnchors` preamble: `const auto& cand_set = cnd.cand_set;
const auto& cover = cnd.cover; const auto& union_keys = cnd.union_keys;
const auto& usable_keys = cnd.usable_keys;` + the same `cov` byte-copy.
`networkElectEnsembleDatum` preamble: `const auto& union_keys = cnd.union_keys;`
(no `cov`).

---

# 4. THE STAYS LIST

| Item | Reason it must not move |
|---|---|
| `std::set<std::string> shared_phase_keys` **declaration** | Run-TU-owned out-param; seeded upstream by `networkPassAHead_`, read by ~10 downstream sites through `networkStashCarry`. Never return it by value. |
| Legacy 3-liner | 3 lines; reads `stations.front().sm` (pivot-by-index, structural); it is the byte-identical baseline path. |
| The 4-way `if/else-if/else` **skeleton** (1416/1420/1523/1538/2176/2225) + the 10-line chain doc comment | The mode dispatch is the run TU's contract; only bodies move. ⚠**AV**: guard indent (12) ≠ ensemble body indent (16). |
| `const bool sticky = config_.networkPhaseDatumSticky();` | Anchor-only consumer; passing it as a scalar keeps `NetworkPhaseDatum.hpp` free of `GnssConfig` for that fn. |
| `subBegin(SB_SLIP)/subEnd`, `subBegin(SB_PREASM)` | ⚠**AV (minor)** — correct rationale: SB_PREASM opens at :1398 and closes only at :2288, and the register-columns `SkipEpoch` `continue` at :2271 **deliberately bypasses** the `subEnd`. (The D0b timer lines are dropped by maskspec, so this is a semantics constraint, not a byte one.) |
| INC-7 seam calls (`networkPassAHead_` / `…SlipScan_` / `…Prefilter_`) | Already carved; the prefilter's PB prune is a precondition of the region. |
| **Card-3 D3c RPB allocation** | Sits OUTSIDE the prioritized `else`; the visible `if (phase_datum_eq34)` gate documents the "inert when eq34 off" contract. 28 lines. → **D1**. |
| `networkAssemblyRegisterColumns_` call + its `continue` | INC-7 seam; contains `est.beginEpoch()`. **Hard upper bound.** |
| `buildEnsembleGauges` (~800 lines later) | The other half of the ensemble datum, but it lives after `est.beginEpoch`/PASS-B. Out of scope; record the coupling in the ensemble seam's doc. |
| Emit-site handover loop (`for (auto& [amb, sta] : this_epoch_anchor)` → `emitted_anchor`) | Sole consumer of `this_epoch_anchor`; sits deep in Iono-2 hunk territory. **Do not touch.** |
| Run-end counter log lines (Card-3 summary; anchored-union gate line) | Invariant-#4 telemetry the gate asserts on. Format strings byte-identical. |

---

# 5. RECORDED DEVIATIONS

| # | Deviation | Why |
|---|---|---|
| D1 | D3c RPB allocation stays in the run TU. | 28 lines; its ordering vs `registerColumns` is the load-bearing contract and the visible eq34 gate documents inertness. Candidate for 8c — **inherit the §3.3 ordering contract**. |
| D2 | Legacy 3-liner stays. | 3 lines, pivot-by-index. |
| D3 | The 5 `config_` reads are NOT hoisted into `ctx.knobs`. | Touches `NetworkRunContext.{hpp,cpp}` (8 concurrent hunks) for zero verbatim benefit; `eq34_round` is composite and a naive hoist silently drops `!carry_invariant`. |
| D4 | `cov` re-declared verbatim in two seams. | Only strictly-verbatim option; 3 call sites, two inside sort comparators. Cross-reference comment in both. |
| ~~D5~~ | ~~`GKey` hoisted to an anonymous namespace~~ | ⚠**AV: STRUCK** — premise false (local types are legal template args since C++11; all 6 refs are inside the moved body). Keeping it function-local preserves strict dedent-only. |
| D6 | The shift walk is NOT sub-extracted from the per-component loop. | `recordEdge` has ~20 captures across three nesting levels; extracting it alone is a semantic rewrite. Costs the oracle test its fine granularity. |
| D7 | Retargeting the existing `shiftWalk` reimplementation oracle at production is deferred (see Open Question 3). | Rewriting the independent oracle in the same increment as the move removes the only thing that could catch a transcription error. |
| D8 | **Snapshot three-place-rule gap recorded, not fixed**: `anchor_incumbent`, `this_epoch_anchor`, `emitted_anchor`, all `eq34_*` maps/counters, `rpb_carry`, `n_gated_*` are neither fingerprinted nor restored nor written, and carry no "Not snapshotted:" exemption — while `local_carry` IS restored. | Pre-existing; a warm resume starts with an empty tree while holding carries only that tree gives meaning to. **Own card.** |
| D9 | **Station-0 slip-detector asymmetry flagged, not fixed**: `ApplicationNetworkSetup.cpp` gates MW/IF-TD detector construction on the RAW `config_.networkPhaseDatumPrioritized()` while the run TU uses the OR-ed `ctx.knobs` value — under an eq34-only config the pivot is a fully-carrying station with no slip path (standing invariant #1, one-station instance). | Outside the region; would change behavior. |
| D10 | Stale comment corrected in the run TU only. | Comment-only, byte-inert; not copied into the seam. |
| D11 | The ensemble arm is the one the engineer may drop if arm budget is hard — then the ensemble branch STAYS inline. | See Open Question 1. |
| **D12** | ⚠**AV**: **8b.i is NOT a pure dedent** — the builder opens with a 5-line sizing/clear preamble replacing the original declarations. | Sizing (`stations.size()`) cannot survive naive aliasing; also re-establishes freshness structurally. Call it out in the commit message. |
| **D13** | ⚠**AV**: the stale "ungated union" comment at :1552 is corrected in the same commit. | Predates the pdp gate; leaving it licenses a pre-gate census. |

---

# 6. THE GATE PLAN ← the most important section

## 6.1 Coverage audit of the existing 13-arm matrix

Independently re-verified: `scripts/run_inc4_gate.ps1` drives **13** arms;
**no arm sets `phase_datum_prioritized`, no arm sets `phase_datum_ensemble`,
every arm sets `phase_datum_min_stations: 1`**; none sets `eq34_carry_shift` /
`eq34_longevity_election` / `eq34_extend_only` / `eq34_receiver`.
`NetworkRunContext.cpp:115` ORs the three knobs, so 12 arms take eq34 and
`alloff` takes legacy.

| Branch being moved | Live today? | Arms |
|---|---|---|
| legacy (stays) | ✅ | `alloff` |
| prologue, `min_sta = 1` path | ✅ | 12 eq34 arms |
| prologue **pdp gate** (`min_sta > 1`) | ❌ **DEAD** | none → **`pdgate`** |
| **ensemble branch** | ❌ **DEAD** | none → **`pdens`** |
| eq34 chain (core) | ✅ | 12 arms |
| eq34 pin-to-round | ✅ | `cpllive`, `cpljw` only (`eq34_round = pin_round && !carry_invariant`; 10 arms set `carry_invariant:true`) |
| eq34 carry-shift / longevity / extend-only | ❌ **DEAD** | none → **`eq34x`** |
| eq34 **pref-receiver**, **mismatch** | ❌ **DEAD** | ⚠**AV**: `eq34x` does NOT cover these — see below |
| **anchor election** | ❌ **DEAD** | none → **`pdanchor`** |
| D3c (stays) | ✅ | 12 eq34 arms; inert path via `alloff` |

Empirical confirmation from the committed goldens:
`alloff_a/logs/network_trace.csv` row 1 = `2313,259200.000,50,32,44,1,1,1,2.9644,12.2146,44`
(n_phase_bias 44 == n_pivot_pins 44, legacy pivot) vs `allon_a` `…,197,…,435`.
`obs_a/logs/network_events.csv` holds 6 rows, all `PHASE_DATUM,eq34_reattach`,
**zero `PHASE_ANCHOR,anchor_handover`**.

**A 13-arm byte-identical PASS on a naive 8b would certify the eq34 half and
validate dead arithmetic for exactly the half INC-8 deferred.** That is the
INC-7b lesson verbatim.

## 6.2 New arms — exact recipes

Base for the three prioritized arms is `config/test_suite/_dcmp_50sta_10ep_alloff.json`
(already has `phase_datum_eq34:false` + `carry_invariant:false`, so the
loader's "carry_invariant requires eq34" check cannot trip, and `mw_wl`/
`lane_carry` are off).

**`pdanchor`** → `_dcmp_50sta_10ep_pdanchor.json` = alloff + `"phase_datum_prioritized": true` + `"trace_events": true`

1. ⚠**AV (minor):** do **not** assert `n_pivot_pins == n_phase_bias`. Record both
   as GOLDEN VALUES in the 8b.0 commit message and assert only what is
   structurally guaranteed: `n_phase_bias >= 150` (union scale, not the legacy
   44) and `n_pivot_pins > 0` on **every** row.
2. `run.log` CONTAINS `prioritized phase-bias anchor ON`.
3. `run.log` does NOT contain `Network (Card 3/eq34):`.
4. `logs/network_events.csv` ≥1 `PHASE_ANCHOR,anchor_handover` — **bonus**, not
   required. Structural argument: epoch 1 has an empty `anchor_incumbent` so the
   coverage-ranking fallback is guaranteed live; epochs ≥2 have a non-empty
   incumbent so the sticky lookup is guaranteed live. Record the actual count.

**`pdgate`** → `pdanchor` + `"phase_datum_min_stations": 3`

1. `run.log` CONTAINS `anchored-union coverage gate (< 3 trackers) excluded`
   **with `n_gated_epochs > 0` and `n_gated_key_epochs > 0`** (that line only
   prints under `if (n_gated_epochs > 0)`).
2. `run.log` CONTAINS `anchored-union coverage gate ON`.
3. `n_phase_bias` strictly **less** than `pdanchor`'s on the same epoch.

**`pdens`** → alloff + `"phase_datum_ensemble": true` + `"trace_events": true`

1. `run.log` CONTAINS `ANCHOR-FREE ENSEMBLE phase-bias datum ON (Fix A)`.
2. ⚠**AV (major):** `gate_ok` / `solved` are **site constants**, not per-row
   computed quantities (success site emits `,1,1,`; withheld sites emit `,0,0,0,0,`
   and `,1,0,0,0,`). They carry exactly one bit: "this row came from the
   post-solve site". So require the conjunct on **EVERY row**
   (`n_pivot_pins == 0 && gate_ok == 1 && solved == 1`) **plus an emitted-epoch
   floor**: 10 data rows in `network_trace.csv` and **zero** rows matching
   `,0,0,0,0,` or `,1,0,0,0,`. Without the floor, a capture with 9 of 10 epochs
   gate-withheld satisfies the assertion and is baselined as green.
3. Do NOT also set `phase_datum_eq34` — it is a hard load failure.

**`eq34x`** → base `_dcmp_50sta_10ep_cpllive.json` (has `carry_invariant:false`
so shift/pin-round/rpb_q are not suppressed, and `phase_leveling:true` so
`local_carry` is populated for the shift to act on) +
`"phase_datum_eq34_carry_shift": true` + `"phase_datum_eq34_longevity_election": true`
+ `"phase_datum_eq34_extend_only": true`.

1. run-end `Network (Card 3/eq34):` line shows `… carry value(s) S-transformed
   (v2-shift)` **> 0**.
2. `run.log` CONTAINS `eq34 LONGEVITY-AWARE tree growth ON` and
   `eq34 EXTEND-ONLY membership ON`.
3. `… (sat,σ) re-reference(s)` > 0.
4. ⚠**AV (major):** also assert the run-end line does **not** read
   `0 gap re-pin(s)`. `eq34_retained_edges` is read inside `recordEdge` (:1813)
   and cleared 300 lines later (:2120) inside `if (eq34_extend_only)`, immediately
   before the sticky swap. A relocation that hoists the clear above the BFS, or
   out of the guard, or re-declares the set seam-locally, zeroes
   `eq34_gap_repin_total` and **silently reverts I2 to wholesale replacement** —
   every gapped tree edge re-floats from 0 instead of re-pinning to its pre-gap
   integer, i.e. a full per-arc re-level of the emitted phase-bias frame on every
   momentary row gap. Exercising it needs an arc absent ≥1 epoch that resumes
   while its `local_carry` is still alive; over 10 epochs at 50 stations that is
   close to a null event. Apply the §6.2 escalation rule; **if no affordable arm
   length produces a gap-resume, declare the channel KNOWN-VACUOUS in the commit
   message and require Tier-2 tests 13–16 green before 8b.iii is called done.**
5. ⚠**AV (major): pref-receiver and mismatch are NOT covered by `eq34x`.**
   `eq34_pref_rx` (loader default `""`, absent from every `_dcmp_*.json`) gates
   the r1 override at :1723-1725, inside the relocated body; `mismatch` reads
   `0 pin/gauge-mismatch epoch(s)` in every existing capture.
   **RESOLVED — option (a) taken:** `eq34x` carries
   `"phase_datum_eq34_receiver": "MBAR00UGA"` (verified present in `cpllive`'s
   50-station list and already the `clock_datum_station`, so it is guaranteed
   present every epoch). Assert the named station is component-0's datum
   receiver; this also exercises sticky-vs-preferred precedence. Consequence:
   `eq34x` now arms **four** dark knobs at once, so a diff on it does not
   self-localize — the Tier-2 tests (12, 13–19) are the localization fallback,
   which is why §7 requires them in 8b.iii's own commit.
   `mismatch` stays permanently gate-dark → test 4 carries it.

> **Escalation rule:** if a counter reads 0 at baseline capture, raise
> `max_duration_s` until it is non-zero and record the epoch count. **Do not
> accept a zero baseline** — that is exactly the inert-wiring case invariant #4
> exists to catch.

## 6.3 Gate-script hardening (`scripts/run_inc4_gate.ps1`)

Add the four arms to `$arms`, **parameterize `$exe`** (§8b.0), then extend the
existing invariant-#4 block:

```powershell
if ($a.n -eq "pdanchor" -or $a.n -eq "pdgate") {
    $ok = Select-String -Path "$dir/run.log" -Pattern "prioritized phase-bias anchor ON" -Quiet
    if (-not $ok) { Write-Output "ARM-WARN(inert-anchor) $($a.n)"; $fail = 1 }
}
if ($a.n -eq "pdgate") {
    $ok = Select-String -Path "$dir/run.log" -Pattern "anchored-union coverage gate \(< 3 trackers\) excluded" -Quiet
    if (-not $ok) { Write-Output "ARM-WARN(inert-pdpgate) $($a.n)"; $fail = 1 }
}
if ($a.n -eq "pdens") {
    $ok = Select-String -Path "$dir/run.log" -Pattern "ANCHOR-FREE ENSEMBLE phase-bias datum ON" -Quiet
    if (-not $ok) { Write-Output "ARM-WARN(inert-ensemble) $($a.n)"; $fail = 1 }
}
if ($a.n -eq "eq34x") {
    # AV: parse the integer — an unanchored "0 carry value(s)…" substring also
    # matches "10 …", "40 …", "200 …" and cries wolf on ~1 capture in 10.
    $m = Select-String -Path "$dir/run.log" -Pattern "(?<n>\d+) carry value\(s\) S-transformed"
    if (-not $m -or $m.Matches[0].Groups['n'].Value -eq '0') {
        Write-Output "ARM-WARN(inert-shift) $($a.n)"; $fail = 1
    }
}
```

⚠**AV (major):** audit the pre-existing `cpllive`/`cpljw` check
(`"injected terms: 0 present"`) for the same unanchored-numeral hazard.

## 6.4 Per-sub-step byte checks

| Step | Arms that can prove it | Arms structurally blind |
|---|---|---|
| 8b.0 | all 13 must still pass unchanged; **new arms `_a` vs `_b` clean** | — |
| 8b.i (prologue) | 12 eq34 + pdanchor + pdgate + pdens | alloff |
| 8b.ii (ensemble + anchor) | **pdanchor, pdgate, pdens ONLY** | **all 13 existing arms** |
| 8b.iii (eq34) | 12 eq34 + cpllive/cpljw + eq34x | alloff, pdanchor, pdgate, pdens |
| 8b.iv | suite + 2-arm spot check (allon, pdanchor) | — |

Run the **full 17-arm matrix at every sub-step** regardless — the blind arms prove
the untouched paths stayed untouched.

---

# 7. ADDITIVE TEST PLAN

⚠**AV (major): tests land in their own sub-step's commit**, not deferred to
8b.iv. At the instant 8b.iii lands, four cross-epoch channels (extend-only
retention, longevity snapshot, pin-round `eq34_prev_target` same-station branch,
root handover via `eq34_sat_root`) have **zero witness of any kind** if the tests
come later — a dropped `&`, a premature `std::move`, or a seam-local
re-declaration all compile, pass the full 17-arm matrix, pass the suite, and get
committed as "gated". Where a test cannot precede its seam, state per channel in
the commit message: `unwitnessed at move time: <channel>`.

**Placement.** Prologue / ensemble / anchor → `tests/test_network_phase_datum.cpp`
(already in CMake; its banner reserves it for 8b — the existing `HealFixture` is
estimator-shaped and will NOT serve, write a `StationFixture`). eq34 → NEW
`tests/test_network_chain_datum.cpp`, **hand-added to the explicit CMake list**
(no globbing; a missed entry is an undefined-reference link error).

**Fixture is constructible:** `NetStation` needs only `name`, `sm`, and the two
pin maps (model/mm may stay null). `StateManager sm(kPppCoreSize, log, config)`
precedent at `tests/test_eq34_consumer_integer.cpp:478` and
`tests/test_filter_core.cpp:53`. Elevations come from `modeled[i]` (only
`.satellite` / `.elevation` are read). Use the `spdlog::sinks::ostream_sink_mt`
idiom (INC-8 spectrum-test precedent) to assert on log text. 3–5 stations suffice.

**Tier 1 — invariants + the "(unreachable)" branches** (byte gate is blind even
with the new arms). → 8b.ii / 8b.iii commits.
1. `ChainDatum_PinCountEqualsGaugeDimensionSingleComponent` — 3 sta × 4 sats:
   `eq34_n_pins == eq34_gauge_dim == n_c+m_c == 7`, decomposing as exactly 1
   `pinned_rpb_keys` + 6 `pinned_amb_targets`; `eq34_mismatch_epochs` unchanged.
2. `ChainDatum_PinCountEqualsGaugeDimensionTwoDisjointComponents` — Σ(n_c+m_c);
   exactly 2 RPB pins at 2 distinct stations; no tree edge crosses components.
3. `ChainDatum_SingleStationComponentPinsRpbAndEveryArc` — the degenerate case.
4. `ChainDatum_MismatchWarnsAndCounts` — force the empty-`rx_list` `continue`.
5. `AnchorElection_ExactlyOnePinPerSharedKey` — Σ `pinned_amb_targets.size() ==
   shared_phase_keys.size()`, every target exactly `0.0`.
6. `AnchorElection_NoTrackerKeyIsSkippedNotPinned` — constructs exactly the orphan
   state `healNearNullPhaseBias` exists to rescue, tying the two halves of
   `NetworkPhaseDatum` together.
7. `AnchorElection_PinnedRpbKeysStayEmpty`.
8. `EnsembleDatum_ClearsAllPinsAndTakesPostGateUnion` — pins cleared on EVERY
   station (including absent), `this_epoch_anchor` empty,
   `shared_phase_keys == values(union_keys)`.
8b. ⚠**AV**: `Builder_SizesPerStationVectorsAndIsIdempotent` — assert
   `cnd.sat_el.size() == stations.size()`, and that calling the builder twice on
   the same `cnd` with disjoint station sets leaves nothing from the first call.

**Tier 2 — stickiness / extend-only / longevity / pdp gate** (all zero-arm today).
9. `ChainDatum_StickySeedReattachesDetachedSubtree` — exactly ONE new
   (edge_changed) edge per fragment, not a vertex-by-vertex rewire.
10. `ChainDatum_LiveIncumbentSubsetIsAlwaysAForest` — two-queue BFS visits each
    vertex once.
11. `ChainDatum_RootStickyByComponentVote` (+ tie case asserting the lexical break).
12. `ChainDatum_ConfigPreferredReceiverOverridesSticky` — 100% dark today.
13/14. `ExtendOnly_RetainsGappedEdgeWhileCarryAlive` / `_DropsGappedEdgeWhenCarryPruned`.
15. `ExtendOnly_GapRepinCounterIncrements` — invariant #4.
16. `ExtendOnly_RetainedEdgesClearedEachElection`.
17/18. `Longevity_RisingArcOutranksSettingArc` / `Longevity_OffIsLexicalOrderIdenticalTree`
    — the `// OFF = legacy sat-name order (byte-identical)` comment is a
    load-bearing claim with no test.
19. `Longevity_PrevElSnapshotEmptyWhenOff`.
20. `PdpGate_GatesUndertrackedKeysPrunesStatesAndCounts` — `min_sta=2`, 1-tracker
    key: `union_keys` erased, prune applied at the tracking station only, BOTH
    counters bumped.
21. `PdpGate_OneIsExactNoOp`.
21b. ⚠**AV**: `ChainDatum_HonoursPdpGate` — eq34 + `min_sta=2`:
    `shared_phase_keys` excludes the 1-tracker key (guards against a pre-gate
    census being reintroduced).

**Tier 3 — pin-to-round, shift walk, D3c, legacy.**
22/23. `PinRound_FirstSightingTargetsZero` / `PinRound_RepinRoundsCarryToWavelengthMultiple`
    (+ the `!std::isfinite(lam)` guard leaves target 0).
24. `ShiftWalk_ProductionMatchesReferenceRecurrence` — **highest value in the
    list**: drive `networkElectChainDatum` against the EXISTING
    `shiftWalk`/`propagateChain` oracle in `tests/test_network_estimator.cpp`
    (which self-documents as "a parallel copy for algebra falsification, NOT a
    call into production"). Per D6 the diff is end-to-end. See Open Question 3.
25. `ShiftWalk_OffTreatsNonzeroPinmoveAsUnknown`.
26. `ShiftWalk_RootHandoverFlagsUnknownForWholeComponent`.
27. `Reref_ErasesLocalCarryAtEveryStationAndCountsIt`.
28–31. `D3cRpb_{AllocatesOnePerSystemSignalForUngatedKeys, PrunesStaleWhenKeysGated,
    StationWithAllKeysGatedAllocatesNone, InertWhenEq34Off}`.
32. `LegacyDatum_SharedSetIsPivotPhaseBiasStatesOnly`.

---

# 8. RISK REGISTER (ranked)

| # | Risk | Concrete failure mode | Mitigation |
|---|---|---|---|
| **R1** | **The file drifts live.** | Measured 9,135 → 9,162 → **9,336** within one session (CB-ADM). A cut at a memorized offset slices mid-statement. | §0 re-anchor at increment start **and between every sub-step** (the staging already forces a rebuild there — free). Never paste a survey line number. |
| **R2** | **Byte-gating dead arithmetic** (the INC-7b defect). | 8b.ii relocates ~62 lines that ZERO existing arms execute; a dropped sticky lookup, a lost `anchor_incumbent.swap`, a skipped pin-clear, or a reordered `stable_sort` predicate all pass a 13-arm PASS and ship broken. | 8b.0's four arms with §6.2 assertions, wired as hard failures, **baselined pre-cut**, `_a`/`_b` reproducibility proven. |
| **R3** | **Baselines captured post-cut** enshrine the cut's own output. | The new arms would certify whatever 8b produced. | 8b.0 before any `src/` edit; `git log` must show the arm commit strictly first. Provenance stamp (exe hash + source hash) in every new golden dir. |
| **R4** | **A carry container copied by value.** | `local_carry`/`rpb_carry`/`prev_carry` are value-mutated and copyable; a lost `&` compiles clean and deletes the S-transform with no crash and no counter change. Same class for `anchor_incumbent` (sticky degrades to a fresh election every epoch — invisible except as handover churn on a graded run). | `ctx&` once for eq34. Literal `&`-audit against the §3 prototypes as a review checklist item. `eq34x`'s shift counter non-zero post-cut. |
| **R5** | **Mixed dedent depths read as a non-verbatim edit.** | eq34 body 16 sp, anchor body **12 sp**, prologue 12 sp, guards 12 sp. A blanket dedent mangles one. | Dedent each block by its MEASURED leading-space count. Attach a whitespace-normalized diff (`sed 's/^[ \t]*//'`) of each relocated body vs the pre-cut text to the commit. State both depths in the message. |
| **R6** | **`eq34_reref` rebuilt seam-locally.** | Natural mistake (cleared and refilled every call). Consumers at `networkInjectD2CarryPrior` (both branches), `networkStashCarry` and the M2 VCE snapshot would see an empty set and fold every re-referenced PB prior **in its retired frame**. The in-region `local_carry` erase still happens, so the local side looks clean while the shared border is poisoned — and `eq34_reref_total` still reports non-zero. | Alias into `ctx.carry`, never a local; explicit comment at the alias line. Test 27. |
| **R7** | **`eq34_ppp_utils` construction hoisted or localized.** | Hoisting out of the `if (!eq34_ppp_utils)` guard fires `PppUtils`' ctor debug log on **every** non-eq34 arm (spdlog output changes on all arms). A seam-local instance leaves the pointer null at the emit-rebase site ~4,700 lines later → `lam` stays 0.0 and every `NetworkEmitRebase::Entry` gets `lambda_m = 0`. | Pass `LogManager& log_manager_`; keep the lazy guard byte-identical inside the seam. |
| **R8** | **Prologue side effects lost or duplicated.** | It PRUNES StateManager PB states and is the sole writer of `n_gated_*`. Computing it twice double-counts run-end telemetry; skipping the prune leaves per-station column sets inconsistent with the gated union. | One seam fn, one call site, counters by non-const `std::size_t&`. `pdgate` asserts both > 0. Tests 20/21. |
| **R9** | **`cov` divergence between the two copies.** | A later edit to one changes eq34's coverage ranking but not the anchor's. Also an unrelated identically-named `cov(...)` elsewhere in the run TU produces false hits in any "all uses moved" grep. | Cross-reference comment in both bodies. Preserve the 0-on-miss contract exactly (R10a depends on it). |
| **R10** | **"Cleanups" during the move that break latent guards.** | (a) the r1 coverage fallback dereferences `stations[r1]` while `r1 == size_t(-1)` — safe ONLY because `cov()` returns ≥0 and `best` starts at −1; reordering the `\|\|` operands gives an OOB read at 50-sta scale. (b) `grp_cons`/`grp_sig` are deliberate copies of structured bindings (C++17 forbids capturing them) — eliding them breaks compilation confusingly. (c) `st.pinned_rpb_keys.clear(); // no-op` in the anchor branch keeps the three clear-blocks textually identical. (d) ⚠**AV**: the anchor comparator at :2181 captures `c` from a structured binding — MSVC accepts it, it predates the increment, **out of scope for 8b**. | Verbatim means verbatim. List (a)–(d) in the commit message as "do not simplify". |
| **R11** | **Ordering contract broken.** | The election mutates carries and fills `eq34_reref` PRE-SOLVE. ⚠**AV**: the contract must name **D3c** explicitly — placing the seam after D3c makes D3c prune/allocate RPB against the PREVIOUS epoch's `shared_phase_keys`, so this epoch's `pinned_rpb_keys` can name just-pruned states and newly-entering stations get no δ_{r,σ} column. The rank cross-check counts RECORDED pins, not allocated columns, so it stays silent; surfaces only as W5 withholds / non-PD local blocks on field data. | §3.3 ORDERING CONTRACT as written. Mirror sentence in the D3c comment block naming the seam as its producer. The sub-step diff must show no call-site reordering. |
| **R12** | **`anchor_incumbent = this_epoch_anchor` "tidied" into a move/swap** for symmetry with the anchor branch. | Empties `this_epoch_anchor` before the emit-site handover loop: `emitted_anchor` never advances, no `eq34_reattach`/`anchor_handover` events, and the next genuine handover compares against a permanently stale baseline. Product bytes may stay identical on a short matrix. | The copy-vs-swap asymmetry is deliberate; annotate both sites. `obs`/`pdanchor` event-row counts are the check. |
| **R13** | **CMakeLists conflict.** | CB-ADM inserted `NetworkBiasAdmission.cpp` 1–3 lines from where `NetworkChainDatum.cpp` goes; the test list is likewise live. A missed entry is an undefined-reference **link** error, not a compile error. | Rebase the insertion at 8b.iii start (`git diff CMakeLists.txt` first). Verify the new symbol links before running the matrix. |
| **R14** | **`PhaseDatumCandidates` in the wrong header.** | In `NetworkPhaseDatum.hpp` it creates `NetworkRunContext.hpp → NetworkPhaseDatum.hpp → NetworkRunContext.hpp` the moment someone caches the census on `ctx.carry`. | Declare it in `NetworkRunTypes.hpp` (already included by `NetworkRunContext.hpp`) — the INC-7b rule. |
| **R15** | Suite-count claim inflated by concurrent cards. | The tree carries ~839 TEST macros vs the last committed 805/804/1, from CB-ADM (+12) / Iono-2 (+3) / LF-1. | Report 8b's additive count as a **delta against the pre-8b tree state**, not against the last commit. |

---

# 9. CONCURRENCY PLAN

## 9.1 Do INC-8b, not INC-9b

`buildCarryDdPriors` is a ~1,430-line capture-all lambda invoked twice, and
**nine LF-1 hunks land inside it** — including a structural rewrite of the MW fold
loop from serial `for` to `#pragma omp parallel for` with a per-block
`WlCounters` array, three in-place counter renames, and a new `finalizeLaneBatch`
call — plus three more LF-1 hunks outside it that are read from inside. LF-1's
remaining work points back into those same lines. **INC-8b's region has zero
hunks from any of the three cards**, with ~140 untouched lines of margin below and
~30 above.

## 9.2 Live-session state (re-measured 2026-07-22 07:31)

⚠ **There are FOUR uncommitted cards, not three.** The fourth was missed by the
survey because its files sit outside every region the survey was pointed at:

| Card | Footprint | Last edit | Status |
|---|---|---|---|
| **Slip-QC hardening** (`docs/SPEC_slip_qc_hardening.md`) | `slip/IfPhaseSlipDetector.{hpp,cpp}` +442, `slip/NetworkSlipQc.{hpp,cpp}` +181, `SlipDetectorRegistry.cpp` +9, NEW `slip/NetworkRawObservables.{hpp,cpp}`, NEW `tests/test_if_phase_slip_detector.cpp` + `tests/test_network_slip_qc.cpp` | **00:12:39** | **ACTIVE, mid-edit** |
| CB-ADM | pre-loop census + Assembly registration swap | 00:06:57 | uncommitted, awaiting review |
| Iono-2 | dual-layer SH, `sh_model2` | Jul-17 | dormant |
| LF-1 lane-fold | `buildCarryDdPriors` OMP rewrite | Jul-17 | dormant |

- **`build/x64-Release/gnss_processor.exe` linked 00:11:43 — and
  `IfPhaseSlipDetector.cpp` was edited at 00:12:39, 56 s LATER.** The tree
  carries an unbuilt edit: the session was interrupted between an edit and its
  build. **No binary built from the current tree corresponds to any reviewed
  state of the slip-QC card.**
- Slip-QC does **not** collide with the 8b region (slip path vs election path),
  and measurement says it is *plausibly* byte-inert on the matrix: **no
  `_dcmp_*` arm arms `if_phase_td` or `mw_detect`** (all 15 configs checked;
  every one has `cycle_slip: {if_phase_td: false}` and none mentions
  `mw_detect`). Plausible is not proven — `NetworkSlipQc.cpp` +141 lines is on
  the PASS-A drain path all arms execute.
- ⚠ **Stale-doc finding:** `docs/network_decomposition_addendum_2026-07-16.md`
  §INC-6 claims "the allon/jw/snap arms run if_phase_td armed and the harness
  asserts non-vacuous QC row counts". **Measured false** — every arm has it
  OFF. INC-6's slip-QC seam was therefore gated by a matrix that never armed
  IF-TD. Correct the addendum at 8b.iv; the QC-row assertion it describes does
  not exist in `run_inc4_gate.ps1`.

## 9.2a ⚠ PRECONDITION ON 8b.0 — the inertness gate

Baselining the four new arms is only sound if the four-card dirty tree is
byte-inert on the matrix, because the 8b.i/ii/iii candidate binaries will be
built from that same dirty tree. **Before capturing any new golden:**

1. Wait for the tree to reach a stable, built state (no source newer than the
   binary).
2. Build Release from it, copy to `gnss_processor_8b.exe`, and run the
   **existing 13 arms** against their committed goldens.
3. **Green** ⇒ all four cards are genuinely default-OFF byte-inert; capture the
   four new arms' `_a`+`_b` here, and record the exe + `git diff` hashes as
   provenance. **Any trip** ⇒ 8b cannot be byte-gated until the offending card
   lands; identify it and stop.

This check is worth running on its own merits: it is the only measurement that
tells anyone whether four simultaneous uncommitted cards have preserved the
default-OFF byte-identity contract.
- `docs/SPEC_cb_admission_card.md` §4 declares the phase side out of scope
  **because** "anchor election is mid-decomposition (8b split pending)" — a
  written contract not to enter our region. But §8 reserves a pdp-threading
  follow-up that WOULD enter it if CB-ADM's INC-2 smoke shows persistent `PB|C##`
  nulls. **Land 8b before that ruling** so the follow-up writes against a carved
  seam.

## 9.3 Merge-conflict surfaces (all add-only, all rebaseable)

| File | Our edit | Concurrent |
|---|---|---|
| `CMakeLists.txt` | +1 source line in the `filter/` block, +1 test line | CB-ADM inserted `NetworkBiasAdmission.cpp` 1–3 lines away; test cluster also live |
| `include/filter/NetworkRunTypes.hpp` | +`PhaseDatumCandidates` (append, trailing section) | Iono-2: `CarryPrior::sh_layer`, `CoupledShared::sh_layer`, 15-del/56-add `ShDevCarry` rewrite — **keep our struct far from `ShDevCarry`** |
| `src/core/app/ApplicationNetwork.cpp` | the region + 2 includes | CB-ADM above the region, Iono-2 below it |
| `include/filter/NetworkPhaseDatum.{hpp,cpp}` | +3 functions | **clean** (mtime = the INC-8 commit) |
| `tests/test_network_phase_datum.cpp` | +tests | **clean** |
| `include/app/NetworkRunContext.hpp` | **none** (D3) | 8 hunks — avoided entirely |
| `include/app/Application.hpp` | **none** (free-fn shape) | 3 Iono-2 hunks — avoided entirely |

Two of the three heaviest-contended headers are untouched purely as a consequence
of the seam-shape ruling — a second, independent argument for free functions.

## 9.4 Sequencing

1. **Preferred:** land 8b after CB-ADM commits. Coordinate; if it will commit
   within the session, wait.
2. **Otherwise:** proceed, re-running §0 immediately before EACH sub-step.
3. Never `git add -A`. Stage exactly: `ApplicationNetwork.cpp`, the two seam
   header/impl pairs, `NetworkRunTypes.hpp`, `CMakeLists.txt`, the test files, the
   four arm configs, `run_inc4_gate.ps1`, the four manifests.
4. **Build/run hygiene:** Release runs use a renamed exe (`gnss_processor_8b.exe`)
   **passed to the gate script via `-Exe`** (§8b.0) so the linker never hits the
   other session's lock (`LNK1104` = lock → retry). Pre-kill zombies
   **name-scoped to your own exe only** — a `gnss_processor*` wildcard sweep kills
   the concurrent session's live runs. **Never run the producer from the Debug
   tree.**
5. If CB-ADM's pdp follow-up lands first, 8b.i's prologue seam is the natural
   place to thread admission state; give `networkBuildPhaseDatumCandidates` a
   signature that can absorb a census reference later without param-list churn.

---

# 10. OPEN QUESTIONS FOR THE AUTHOR

**Q1 — Does the ensemble branch (14 lines, zero live arms) move in 8b.ii,
forcing a fourth new arm (`pdens`)?**
*Recommend: move it and add `pdens`.* The arm is a three-key copy of `alloff`,
and with the ⚠AV every-row + emitted-epoch-floor assertions the witness is
genuine: ~200 unpinned shared PB columns with an empty gauge vector would fail the
W5 null-space gate, so `n_pivot_pins==0 && gate_ok==1 && solved==1` on every one
of 10 rows can only arise if the sum-to-zero gauges were actually folded.
**Explicitly reject** moving it without `pdens` — relocating 14 lines nothing
executes is the INC-7b defect at small scale.

**Q2 — `eq34x` arms three dark knobs at once. If its baseline shows
`S-transformed = 0` over 10 epochs, extend or split?**
*Recommend: extend `max_duration_s` on the single arm first* (`cpllive` has
`carry_invariant:false` so `eq34_round` is true and pin displacements should occur
within a few epochs); record the measured value; split into `eq34shift` / `eq34i2`
only if extending past ~30 epochs still yields zero. **Explicitly reject** gating
on loader info lines alone — that proves the knob was parsed, not that the
arithmetic ran.

**Q3 — Should 8b also retarget the existing `shiftWalk` reimplementation oracle
at the newly-callable production seam (test 24)?**
*Recommend: add test 24 as a NEW test that drives production against the existing
oracle helpers, leaving the original algebra tests untouched.* The oracle is the
only independent check that would catch a transcription error in the 637-line
relocation, so it must not be rewritten in the same increment that performs the
relocation — but there is no reason not to consume it. Retiring the now-redundant
mirrors is an 8c cleanup.

**Q4 — The three-place snapshot-rule gap (D8) — doc-only record, or fix in 8b?**
*Recommend: record only; do not touch `NetworkRunContext.hpp`* (8 concurrent
hunks; keeping that file untouched is a concrete win of the free-function ruling),
then open a card. The gap is pre-existing and byte-inert under `carry_invariant`
(which forces `eq34_round` false, so targets were already 0), but it is field-live
on the pin-round lineage and under-reports run-end telemetry on any resumed run.
**Explicitly reject** fixing it inside 8b — a snapshot-semantics change inside a
byte-identity increment makes a gate failure un-attributable.

**Q5 — Sequencing.** Land 8b.0 (arms only, no `src/` edit) now while CB-ADM is
mid-flight, or hold the whole increment until CB-ADM commits? 8b.0 touches only
`config/test_suite/`, `scripts/run_inc4_gate.ps1`, `goldens/decomp/` and
`runs/` — **zero overlap with any live card** — but it does consume Release
run time on the shared machine.
