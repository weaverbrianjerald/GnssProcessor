# SPEC — Decomposition INC-8b: phase-datum election split

**Status: LANDED 2026-07-22 and COMMITTED (`c91a6ba`). NOT BYTE-GATED — see §2.** Written
2026-07-21 from an 8-agent survey + xhigh synthesis critic + 4-lens adversarial verify
(7+8+5+5 defects; **4 blocking, 9 major** — all folded in and marked ⚠**AV**). De-bloated
2026-08-15: the executed staging recipe, the §0 re-anchor procedure, the verbatim seam-signature
code blocks (now the shipped headers) and the July concurrency snapshot were compressed into §10.
**Everything still true of the shipped seams, and every piece of open verification debt, is
retained.**

**Not a datum card.** This is a TU decomposition increment (the phase-datum election split into
`NetworkPhaseDatum` + `NetworkChainDatum`). It belongs with `docs/network_decomposition_plan.md`
§2-6, its design authority. **Predecessor:** INC-8 (`30684a1`) deliberately did NOT move the
anchor election, recording *"splitting the 4-way legacy/ensemble/eq34/default chain across TUs =
8b-time ruling"*. This spec is that ruling.

---

# 1. WHAT LANDED

Executed on user instruction to prioritize size reduction over gating; **8b.0 (the four new gate
arms) was authored but NOT captured**, so the byte gate has not run. Sub-steps were NOT staged
separately — the cut landed as **one scripted transaction** because the run TU was under
concurrent edit and the excision had to occupy the smallest possible window.

| Metric | Value |
|---|---|
| `ApplicationNetwork.cpp` | **9,456 → 8,682 lines (−774)** |
| `NetworkPhaseDatum.cpp` | 69 → 279 (+210: census, ensemble datum, anchor election) |
| `NetworkChainDatum.{hpp,cpp}` | NEW, 105 + 712 lines (eq34 chain election) |
| Relocated verbatim | builder 96 + ensemble 14 + eq34 637 + anchor 48 = **795 lines** |
| Verbatim proof | all four bodies byte-identical (md5, post-hoc re-verify) |
| Build | clean **first try**, both seams and the excision |
| Suite | **unchanged from pre-cut** (865 / 864 pass / 1 skip *at that date*; the suite is now 1360 cases / 196 suites, so the 865 figures no longer parse against today's tree — the load-bearing fact is *unchanged by the cut*) |

**Method — why the transcription is trustworthy (the reusable part).** Three scripted phases, the
INC-6 precedent: **EXTRACT** — 21 guard anchors asserted against expected text, measured per-block
dedent, md5 per body; **ASSEMBLE** — bodies inserted **by script, nothing hand-typed**, the alias
preamble derived **MECHANICALLY** by scanning the run TU's own `auto& X = ctx.carry.X;` block and
keeping only names the body references; **EXCISE** — 13 guards, single write. A post-hoc re-verify
re-extracted each block from the run TU and proved the generated seam contains it
**byte-for-byte**. The mechanical alias derivation **independently reproduced §4.2 exactly**: eq34
references 22 `ctx` aliases (18 `carry` + 4 `knobs`), anchor exactly 2, ensemble exactly 1 — and
`pl_max_gap_s` is absent, confirming that AV minor finding **from the source rather than by
argument**.

**Deviation from D12 (an improvement).** The builder keeps the five census declarations as locals
byte-identically (sizing ctor args intact) and hands them over in a 5-line `std::move`
**epilogue**, rather than the sizing/alias preamble the AV proposed. This makes the builder body a
*pure* dedent — the AV named it "the only strictly-dedent-only option" — and freshness is still
structural, since move-assignment fully overwrites each member.

**Dedent depths differed per block and still document the diff**: eq34 body 16 sp, anchor body
**12 sp** (it sat at the same column as its own `} else {` and its own closing `}`), prologue
12 sp, guards 12 sp. A blanket dedent would have mangled one; each was dedented by its MEASURED
leading-space count. There was **no** epoch-level `continue`/`break`/`return`/`goto` anywhere in
the region, so unlike INC-7 **8b needed no `EpochAction`** — every seam is plain `void`.

---

# 2. ⚠ VERIFICATION DEBT — STILL OPEN. Read before trusting the cut.

The cut is proven **STRUCTURALLY** (verbatim bodies, clean build, unchanged suite) but **NOT
FUNCTIONALLY**: no golden arm has been run (8b.0's four arms, §5.2, were authored but never
captured, so the byte gate has never executed); **no test in the suite exercises any of the four
relocated bodies** (the passing suite proves linkage, not behaviour); and **Tier-1 items 5–8 (§6)
cover the ensemble/anchor path that NO arm in the matrix executes at all** (§5.1).

**This is R2 in live form**, and R2 is the INC-7b lesson verbatim: *a 13-arm byte-identical PASS on
a naive 8b would certify the eq34 half and validate dead arithmetic for exactly the half INC-8
deferred.* Before this is field-usable: capture 8b.0's four arms, run the 17-arm matrix, and add
the §6 tests.

---

# 3. THE RULING

| Question | Ruling |
|---|---|
| Anchor election moves with eq34? | **No — separate sub-step, separate TU.** |
| One destination or two? | **Two.** eq34 → NEW `filter/NetworkChainDatum`; ensemble + anchor + prologue → EXISTING `filter/NetworkPhaseDatum`. |
| Seam shape | **Free functions**, params named exactly as the former run-TU locals (INC-4/6 convention). eq34 additionally takes `NetworkRunContext& ctx` + alias preamble. |
| Prologue | **Moves once**, as its own seam fn filling `PhaseDatumCandidates`. Both elections consume it by const ref. **No duplication.** |
| D3c RPB allocation / legacy 3-liner | **Stay.** |
| Do INC-8b, not INC-9b | **8b.** See §10.2. |

**Why free functions, not INC-7 member fns and not an INC-7b builder** — the evidence, which
remains the precedent for the next seam decision:

- **Capture evidence.** Exactly **SIX** `Application`-member expressions across ~870 lines:
  `config_.networkPhaseDatumPrefElevDeg/Sticky/MinStations()` (prologue),
  `config_.networkPhaseDatumEq34Receiver/Eq34PinRound()` (eq34), one `log_manager_` (lazy
  `make_unique<PppUtils>`). **Zero** `gim_`, **zero** `traces`, **zero** `est`/`ws`/`setup`/
  `emitEvent`. **The anchor body alone has zero of all six** — the INC-7b evidence pattern, not
  INC-7's "config_/gim_/log_manager_-dense" profile.
- **Not INC-7b's builder either.** `CouplingRowBuilder` won because it had per-instance scratch
  constructed per-station inside a loop. Here every scratch container is declared and consumed
  inside one straight-line call invoked **once per epoch**, and eq34's lambdas (`recordEdge` ~20
  captures, `attach`, `arcLife`, `seedEdge`, `find`/`unite`) close over **per-group /
  per-component** locals (`grp_cons`, `grp_sig`, `system`, `rpb_key`, `r1`, `order`). Flattening
  those into builder members forces loop-scoped values to become members reassigned per iteration —
  **a semantic edit, not a dedent**.
- **Testability forecloses member fns independently.** No test in the tree constructs an
  `Application` (4 grep hits, all comment text). The member-fn shape would make the 8b seam
  untestable by every pattern the suite uses. `include/filter/NetworkPhaseDatum.hpp:22-25` states
  the shape invariant: *"These functions own NO state: every carry/workspace object is an explicit
  reference into the caller's NetworkRunContext."*
- **`ctx` by reference for eq34 ONLY.** The eq34 body touches ~19 `ctx.carry` maps + 5 counters.
  Enumerating them as by-ref params creates a **compiles-clean, numerically-wrong** failure class
  (R4): `local_carry`/`rpb_carry`/`prev_carry` are value-mutated (`lc.value_abs += d;`) and
  copyable, so **one dropped `&` silently deletes the S-transform**. Taking `ctx&` once removes ~24
  chances to make it. Anchor/ensemble need only two maps → they take them explicitly, so
  `NetworkPhaseDatum.hpp` stays free of any `app/` dependency.

**Why the prologue is its own seam** (not driver-side, not duplicated). Consumption is asymmetric
but genuinely shared: `union_keys` → all three sub-branches; `usable_keys` + `cov` → eq34 and
anchor; `sat_el` → eq34 only; `cand_set` + `sticky` → anchor only. Nothing in any sub-branch writes
it. Two facts force it into a named seam: (1) it is the **SOLE writer** of
`n_gated_epochs`/`n_gated_key_epochs`, so any arrangement that can compute it twice
**double-counts invariant-#4 telemetry**; (2) it **MUTATES StateManager**
(`pruneSatPhaseBiasStatesExcept`) — an invisible side effect both elections depend on. Naming it
documents that ordering.

**Rejected alternatives, with the reason each stays rejected:** member fns of `Application` and a
builder struct (above); enumerating eq34's ~19 carry maps as params (the dropped-`&` class, R4);
one monolithic move of the whole region — lines 1416–2226 — which puts a 637-line Card-3 chain
machine into a TU named for phase-datum anchoring and still byte-gates the anchor half on arms that
never execute it; prologue driver-side with 6 loose containers (hides the StateManager-mutating
gate as an unnamed precondition); hoisting the 5 `config_` reads into `ctx.knobs` (**D3**); `cov` as
a member or `std::function` (**D4**).

---

# 4. CONTRACTS THE SHIPPED SEAMS DEPEND ON

The full seam signatures live in the shipped headers (`include/filter/NetworkPhaseDatum.hpp`,
`include/filter/NetworkChainDatum.hpp`, `PhaseDatumCandidates` in
`include/filter/NetworkRunTypes.hpp`). What is recorded here is what a reader cannot recover from
the signatures.

## 4.1 The ORDERING CONTRACT (R11) — load-bearing, stated in `NetworkChainDatum.hpp`

`networkElectChainDatum` **MUTATES persistent carries PRE-SOLVE** (erases `local_carry` for
re-referenced (sat,σ), erases/shifts `rpb_carry`, shifts `prev_carry` PhaseBias entries, fills
`ctx.carry.eq34_reref`):

- **AFTER** `networkAssemblyPrefilter_`;
- **BEFORE the Card-3 D3c RPB allocation** — this seam is the SOLE writer of `shared_phase_keys` on
  the eq34 path and D3c derives this epoch's `need_rpb` set from it. ⚠ Placing the seam **after**
  D3c makes D3c prune/allocate RPB against the **PREVIOUS** epoch's `shared_phase_keys`, so this
  epoch's `pinned_rpb_keys` can name just-pruned states and newly-entering stations get no δ_{r,σ}
  column. **The rank cross-check counts RECORDED pins, not allocated columns, so it stays SILENT**
  — this surfaces only as W5 withholds / non-PD local blocks on field data.
- D3c in turn must precede `est.reset()`/`est.beginEpoch()` inside `networkAssemblyRegisterColumns_`;
- **BEFORE** `networkStashCarry` and `networkInjectD2CarryPrior`;
- the I2 extend-only merge must stay **BEFORE** `anchor_incumbent = this_epoch_anchor;`.

## 4.2 The alias preamble — exactly four knobs

`networkElectChainDatum`'s preamble aliases 18 `ctx.carry` members (`prev_carry`,
`anchor_incumbent`, `this_epoch_anchor`, `eq34_datum_rx`, `eq34_rx_parent`, `eq34_sat_root`,
`eq34_prev_el`, `eq34_ppp_utils`, `eq34_reref_total`, `eq34_erase_total`, `eq34_shift_total`,
`eq34_gap_repin_total`, `eq34_mismatch_epochs`, `eq34_retained_edges`, `eq34_prev_target`,
`eq34_reref`, `local_carry`, `rpb_carry`) plus the four `cnd` members and a byte-copy of the `cov`
lambda.

⚠ **The knob set is EXACTLY FOUR** — `carry_invariant`, `eq34_shift_on`, `eq34_longevity`,
`eq34_extend_only`. **Do NOT add `pl_max_gap_s`**: the I2 extend-only liveness test is
membership-only (`local_carry.count(...)`); the `phase_leveling_max_gap_s` horizon is applied at
end of epoch by the `local_carry` prune in `NetworkRunContext`. `eq34_reref` **must alias into
`ctx.carry`, never a local** (R6).

## 4.3 ⚠ Terminology — "ungated" must not appear (D13)

There is exactly ONE `union_keys` and the pdp gate erases from it **in place**; eq34 re-filters
against the same gated map. Both branches seed from the identical object. **The word "ungated" must
not appear in either seam's docs** — a written licence to hand `ChainDatum` a pre-gate census
re-admits every 1-tracker (sat,signal) into `shared_phase_keys`, which D3c reads three lines
downstream to allocate RPB: the exact non-PD / one-station-re-level pathology.
`PhaseDatumCandidates`' doc states it once: *"`union_keys` is post-gate; there is no pre-gate copy
and none may be added."*

## 4.4 The STAYS list — items whose staying is still a live invariant

| Item | Reason it must not move |
|---|---|
| `std::set<std::string> shared_phase_keys` **declaration** | Run-TU-owned out-param; seeded upstream by `networkPassAHead_`, read by ~10 downstream sites through `networkStashCarry`. **Never return it by value.** |
| The 4-way `if/else-if/else` **skeleton** + the chain doc comment | The mode dispatch is the run TU's contract; only bodies moved. ⚠ Guard indent (12) ≠ ensemble body indent (16). |
| `const bool sticky = config_.networkPhaseDatumSticky();` | Anchor-only consumer; passing it as a scalar keeps `NetworkPhaseDatum.hpp` free of `GnssConfig` for that fn. |
| `subBegin(SB_SLIP)/subEnd`, `subBegin(SB_PREASM)` | SB_PREASM opens before the region and closes well after it, and the register-columns `SkipEpoch` `continue` **deliberately bypasses** the `subEnd`. A semantics constraint, not a byte one (the D0b timer lines are dropped by maskspec). |
| **Card-3 D3c RPB allocation** | Sits OUTSIDE the prioritized `else`; the visible `if (phase_datum_eq34)` gate documents the "inert when eq34 off" contract. 28 lines. → **D1**. |
| `networkAssemblyRegisterColumns_` call + its `continue` | INC-7 seam; contains `est.beginEpoch()`. **Hard upper bound of the cut.** |
| `buildEnsembleGauges` (~800 lines later) | The other half of the ensemble datum, but it lives after `est.beginEpoch`/PASS-B. Out of scope; the coupling is recorded in the ensemble seam's doc. |
| Emit-site handover loop (`for (auto& [amb, sta] : this_epoch_anchor)` → `emitted_anchor`) | Sole consumer of `this_epoch_anchor`. **Do not touch** (R12). |
| Run-end counter log lines (Card-3 summary; anchored-union gate line) | Invariant-#4 telemetry the gate asserts on. Format strings byte-identical. |

---

# 5. THE GATE PLAN — still the outstanding work

## 5.1 Coverage audit of the existing 13-arm matrix — STILL TRUE

Independently re-verified: `scripts/run_inc4_gate.ps1` drives **13** arms; **no arm sets
`phase_datum_prioritized`, no arm sets `phase_datum_ensemble`, every arm sets
`phase_datum_min_stations: 1`**; none sets `eq34_carry_shift` / `eq34_longevity_election` /
`eq34_extend_only` / `eq34_receiver`. `NetworkRunContext.cpp:115` ORs the three knobs, so 12 arms
take eq34 and `alloff` takes legacy.

| Branch moved | Live today? | Arms |
|---|---|---|
| legacy (stays) | ✅ | `alloff` |
| prologue, `min_sta = 1` path | ✅ | 12 eq34 arms |
| prologue **pdp gate** (`min_sta > 1`) | ❌ **DEAD** | none → **`pdgate`** |
| **ensemble branch** | ❌ **DEAD** | none → **`pdens`** |
| eq34 chain (core) | ✅ | 12 arms |
| eq34 pin-to-round | ✅ | `cpllive`, `cpljw` only (`eq34_round = pin_round && !carry_invariant`; 10 arms set `carry_invariant:true`) |
| eq34 carry-shift / longevity / extend-only | ❌ **DEAD** | none → **`eq34x`** |
| eq34 **pref-receiver**, **mismatch** | ❌ **DEAD** | ⚠ `eq34x` does NOT cover these — §5.2 |
| **anchor election** | ❌ **DEAD** | none → **`pdanchor`** |
| D3c (stays) | ✅ | 12 eq34 arms; inert path via `alloff` |

Empirical confirmation from the committed goldens: `alloff_a/logs/network_trace.csv` row 1 =
`2313,259200.000,50,32,44,1,1,1,2.9644,12.2146,44` (n_phase_bias 44 == n_pivot_pins 44, legacy
pivot) vs `allon_a` `…,197,…,435`. `obs_a/logs/network_events.csv` holds 6 rows, all
`PHASE_DATUM,eq34_reattach`, **zero `PHASE_ANCHOR,anchor_handover`**.

## 5.2 The four new arms — recipes and assertions (authored, NEVER CAPTURED)

Two blocking constraints on capture, both still binding:

- **Capture `_a` AND `_b` for every new arm.** The harness compares golden **DIRECTORIES**, not
  manifests (`run_inc4_gate.ps1:35` passes `$a.g` = `runs/decomp_goldens/<arm>_a` to
  `compare_network_run.py`; `--manifest` mode is separate and unused). All 13 existing arms have
  `_a` + `_b`. Assert `compare <arm>_a <arm>_b --maskspec goldens/decomp/maskspec.json` is clean
  **before any `src/` edit** — otherwise the new arms become the sole byte witness for the
  anchor/ensemble half without ever being proven reproducible.
- **Parameterize the exe.** `run_inc4_gate.ps1:5` hardcodes
  `build/x64-Release/gnss_processor_dcmp.exe` with no override. Following the renamed-exe directive
  while leaving the script alone **byte-compares a stale binary against itself — the strongest
  possible false green.** Add `param([string]$Exe = …)`. Write a provenance stamp (exe SHA-256,
  `git rev-parse HEAD`, hash of `git diff` over `src/ include/`) into every new golden dir.
  **⚠ Baselines must be captured PRE-cut** (R3) — since the cut has landed, a capture today
  enshrines the cut's own output and proves nothing.

Base for the three prioritized arms is `config/test_suite/_dcmp_50sta_10ep_alloff.json` (already
has `phase_datum_eq34:false` + `carry_invariant:false`, so the loader's "carry_invariant requires
eq34" check cannot trip, and `mw_wl`/`lane_carry` are off).

**`pdanchor`** = alloff + `"phase_datum_prioritized": true` + `"trace_events": true`. Do **not**
assert `n_pivot_pins == n_phase_bias`; record both as GOLDEN VALUES and assert only what is
structurally guaranteed — `n_phase_bias >= 150` (union scale, not the legacy 44) and
`n_pivot_pins > 0` on **every** row. `run.log` CONTAINS `prioritized phase-bias anchor ON` and does
NOT contain `Network (Card 3/eq34):`. ≥1 `PHASE_ANCHOR,anchor_handover` is a **bonus, not
required** (epoch 1 has an empty `anchor_incumbent` so the coverage-ranking fallback is guaranteed
live; epochs ≥2 have a non-empty incumbent so the sticky lookup is guaranteed live) — record the
actual count.

**`pdgate`** = `pdanchor` + `"phase_datum_min_stations": 3`. `run.log` CONTAINS `anchored-union
coverage gate (< 3 trackers) excluded` **with `n_gated_epochs > 0` and `n_gated_key_epochs > 0`**
(that line only prints under `if (n_gated_epochs > 0)`), and `anchored-union coverage gate ON`.
`n_phase_bias` strictly **less** than `pdanchor`'s on the same epoch.

**`pdens`** = alloff + `"phase_datum_ensemble": true` + `"trace_events": true`. `run.log` CONTAINS
`ANCHOR-FREE ENSEMBLE phase-bias datum ON (Fix A)`. ⚠ **`gate_ok` / `solved` are SITE CONSTANTS**,
not per-row computed quantities (the success site emits `,1,1,`; withheld sites emit `,0,0,0,0,`
and `,1,0,0,0,`) — they carry exactly one bit, "this row came from the post-solve site". So require
the conjunct on **EVERY row** (`n_pivot_pins == 0 && gate_ok == 1 && solved == 1`) **plus an
emitted-epoch floor**: 10 data rows in `network_trace.csv` and **zero** rows matching `,0,0,0,0,`
or `,1,0,0,0,`. Without the floor, a capture with 9 of 10 epochs gate-withheld satisfies the
assertion and is baselined as green. Do NOT also set `phase_datum_eq34` — a hard load failure.

**`eq34x`** = `_dcmp_50sta_10ep_cpllive.json` (has `carry_invariant:false` so
shift/pin-round/rpb_q are not suppressed, and `phase_leveling:true` so `local_carry` is populated)
+ `phase_datum_eq34_carry_shift` + `_longevity_election` + `_extend_only` +
`"phase_datum_eq34_receiver": "MBAR00UGA"`. Assertions: the run-end `Network (Card 3/eq34):` line
shows `… carry value(s) S-transformed (v2-shift)` **> 0**; `run.log` CONTAINS `eq34
LONGEVITY-AWARE tree growth ON` and `eq34 EXTEND-ONLY membership ON`; `… (sat,σ) re-reference(s)`
> 0; the named station is component-0's datum receiver (which also exercises sticky-vs-preferred
precedence).

⚠ **Also assert the run-end line does NOT read `0 gap re-pin(s)`.** `eq34_retained_edges` is read
inside `recordEdge` and cleared ~300 lines later inside `if (eq34_extend_only)`, immediately before
the sticky swap. A relocation that hoists the clear above the BFS, or out of the guard, or
re-declares the set seam-locally, **zeroes `eq34_gap_repin_total` and silently reverts I2 to
wholesale replacement** — every gapped tree edge re-floats from 0 instead of re-pinning to its
pre-gap integer, i.e. a full per-arc re-level of the emitted phase-bias frame on every momentary
row gap. Exercising it needs an arc absent ≥1 epoch that resumes while its `local_carry` is still
alive; over 10 epochs at 50 stations that is close to a null event. **If no affordable arm length
produces a gap-resume, declare the channel KNOWN-VACUOUS in the commit message and require Tier-2
tests 13–16 green.**

⚠ `eq34x` arms **four** dark knobs at once, so a diff on it does not self-localize — the Tier-2
tests are the localization fallback. `mismatch` stays permanently gate-dark → test 4 carries it.

> **Escalation rule:** if a counter reads 0 at baseline capture, raise `max_duration_s` until it is
> non-zero and record the epoch count. **Do not accept a zero baseline** — that is exactly the
> inert-wiring case invariant #4 exists to catch. Explicitly reject gating on loader info lines
> alone: that proves the knob was parsed, not that the arithmetic ran.

## 5.3 Gate-script hardening (`scripts/run_inc4_gate.ps1`)

Add the four arms to `$arms`, **parameterize `$exe`**, then extend the existing invariant-#4 block
with per-arm inertness checks (`pdanchor`/`pdgate` → "prioritized phase-bias anchor ON"; `pdgate` →
the coverage-gate line; `pdens` → the Fix-A banner; `eq34x` → the S-transformed count). ⚠ **Parse
the INTEGER** for the `eq34x` check — an unanchored `"0 carry value\(s\)…"` substring also matches
`"10 …"`, `"40 …"`, `"200 …"` and cries wolf on ~1 capture in 10. **Audit the pre-existing
`cpllive`/`cpljw` check (`"injected terms: 0 present"`) for the same unanchored-numeral hazard.**

## 5.4 Which arms can prove which half

| Half | Arms that can prove it | Arms structurally blind |
|---|---|---|
| prologue | 12 eq34 + pdanchor + pdgate + pdens | alloff |
| **ensemble + anchor** | **pdanchor, pdgate, pdens ONLY** | **all 13 existing arms** |
| eq34 | 12 eq34 + cpllive/cpljw + eq34x | alloff, pdanchor, pdgate, pdens |

Run the **full 17-arm matrix** regardless — the blind arms prove the untouched paths stayed
untouched.

---

# 6. ADDITIVE TEST PLAN — NONE OF THESE EXIST

⚠ Four cross-epoch channels (extend-only retention, longevity snapshot, pin-round
`eq34_prev_target` same-station branch, root handover via `eq34_sat_root`) have **zero witness of
any kind** today: a dropped `&`, a premature `std::move`, or a seam-local re-declaration all
compile, pass the full matrix, pass the suite, and read as "gated".

**Placement.** Prologue / ensemble / anchor → `tests/test_network_phase_datum.cpp` (already in
CMake; `tests/test_network_phase_datum.cpp:14` already reserves the file for 8b — the existing
`HealFixture` is estimator-shaped and will NOT serve, write a `StationFixture`). eq34 → NEW
`tests/test_network_chain_datum.cpp`, **hand-added to the explicit CMake list** (no globbing; a
missed entry is an undefined-reference LINK error).

**Fixture is constructible:** `NetStation` needs only `name`, `sm`, and the two pin maps (model/mm
may stay null). `StateManager sm(kPppCoreSize, log, config)` precedent at
`tests/test_eq34_consumer_integer.cpp:478` and `tests/test_filter_core.cpp:53`. Elevations come
from `modeled[i]` (only `.satellite` / `.elevation` are read). Use the
`spdlog::sinks::ostream_sink_mt` idiom (INC-8 spectrum-test precedent) to assert on log text. 3–5
stations suffice.

**Tier 1 — invariants + the "(unreachable)" branches** (the byte gate is blind even with the new
arms).
1. `ChainDatum_PinCountEqualsGaugeDimensionSingleComponent` — 3 sta × 4 sats:
   `eq34_n_pins == eq34_gauge_dim == n_c+m_c == 7`, decomposing as exactly 1 `pinned_rpb_keys` + 6
   `pinned_amb_targets`; `eq34_mismatch_epochs` unchanged.
2. `ChainDatum_PinCountEqualsGaugeDimensionTwoDisjointComponents` — Σ(n_c+m_c); exactly 2 RPB pins
   at 2 distinct stations; no tree edge crosses components.
3. `ChainDatum_SingleStationComponentPinsRpbAndEveryArc` — the degenerate case.
4. `ChainDatum_MismatchWarnsAndCounts` — force the empty-`rx_list` `continue`.
5. `AnchorElection_ExactlyOnePinPerSharedKey` — Σ `pinned_amb_targets.size() ==
   shared_phase_keys.size()`, every target exactly `0.0`.
6. `AnchorElection_NoTrackerKeyIsSkippedNotPinned` — constructs exactly the orphan state
   `healNearNullPhaseBias` exists to rescue.
7. `AnchorElection_PinnedRpbKeysStayEmpty`.
8. `EnsembleDatum_ClearsAllPinsAndTakesPostGateUnion` — pins cleared on EVERY station (including
   absent), `this_epoch_anchor` empty, `shared_phase_keys == values(union_keys)`.
8b. `Builder_SizesPerStationVectorsAndIsIdempotent` — `cnd.sat_el.size() == stations.size()`, and
   calling the builder twice on the same `cnd` with disjoint station sets leaves nothing from the
   first call.

**Tier 2 — stickiness / extend-only / longevity / pdp gate** (all zero-arm).
9. `ChainDatum_StickySeedReattachesDetachedSubtree` — exactly ONE new (edge_changed) edge per
   fragment, not a vertex-by-vertex rewire.
10. `ChainDatum_LiveIncumbentSubsetIsAlwaysAForest` — two-queue BFS visits each vertex once.
11. `ChainDatum_RootStickyByComponentVote` (+ tie case asserting the lexical break).
12. `ChainDatum_ConfigPreferredReceiverOverridesSticky` — 100 % dark today.
13/14. `ExtendOnly_RetainsGappedEdgeWhileCarryAlive` / `_DropsGappedEdgeWhenCarryPruned`.
15. `ExtendOnly_GapRepinCounterIncrements` — invariant #4.
16. `ExtendOnly_RetainedEdgesClearedEachElection`.
17/18. `Longevity_RisingArcOutranksSettingArc` / `Longevity_OffIsLexicalOrderIdenticalTree` — the
   `// OFF = legacy sat-name order (byte-identical)` comment is a load-bearing claim with no test.
19. `Longevity_PrevElSnapshotEmptyWhenOff`.
20. `PdpGate_GatesUndertrackedKeysPrunesStatesAndCounts` — `min_sta=2`, 1-tracker key: `union_keys`
   erased, prune applied at the tracking station only, BOTH counters bumped.
21. `PdpGate_OneIsExactNoOp`.
21b. `ChainDatum_HonoursPdpGate` — eq34 + `min_sta=2`: `shared_phase_keys` excludes the 1-tracker
   key (guards against a pre-gate census being reintroduced — §4.3).

**Tier 3 — pin-to-round, shift walk, D3c, legacy.**
22/23. `PinRound_FirstSightingTargetsZero` / `PinRound_RepinRoundsCarryToWavelengthMultiple`
   (+ the `!std::isfinite(lam)` guard leaves target 0).
24. `ShiftWalk_ProductionMatchesReferenceRecurrence` — **highest value in the list**: drive
   `networkElectChainDatum` against the EXISTING `shiftWalk`/`propagateChain` oracle in
   `tests/test_network_estimator.cpp` (which self-documents as "a parallel copy for algebra
   falsification, NOT a call into production"). Per D6 the diff is end-to-end. *Ruled: add it as a
   NEW test driving production against the existing oracle helpers, leaving the original algebra
   tests untouched — the oracle is the only independent check that would catch a transcription
   error in the 637-line relocation, so it must not be rewritten in the same increment that
   performs the relocation. Retiring the now-redundant mirrors is an 8c cleanup.*
25. `ShiftWalk_OffTreatsNonzeroPinmoveAsUnknown`.
26. `ShiftWalk_RootHandoverFlagsUnknownForWholeComponent`.
27. `Reref_ErasesLocalCarryAtEveryStationAndCountsIt`.
28–31. `D3cRpb_{AllocatesOnePerSystemSignalForUngatedKeys, PrunesStaleWhenKeysGated,
   StationWithAllKeysGatedAllocatesNone, InertWhenEq34Off}`.
32. `LegacyDatum_SharedSetIsPivotPhaseBiasStatesOnly`.

---

# 7. RECORDED DEVIATIONS (D1–D13)

| # | Deviation | Why |
|---|---|---|
| D1 | D3c RPB allocation stays in the run TU. | 28 lines; its ordering vs `registerColumns` is the load-bearing contract and the visible eq34 gate documents inertness. Candidate for 8c — **inherit the §4.1 ordering contract**. |
| D2 | Legacy 3-liner stays. | 3 lines, pivot-by-index; the byte-identical baseline path. |
| D3 | The 5 `config_` reads are NOT hoisted into `ctx.knobs`. | Touches `NetworkRunContext.{hpp,cpp}` for zero verbatim benefit; `eq34_round` is composite (`… && !carry_invariant`) and a naive hoist silently drops it. |
| D4 | `cov` re-declared verbatim in two seams. | Only strictly-verbatim option; 3 call sites, two inside sort comparators. Cross-reference comment in both. |
| ~~D5~~ | ~~`GKey` hoisted to an anonymous namespace~~ | ⚠**AV: STRUCK** — premise false (local types have been legal template args since C++11 / N2657; all 6 refs are inside the moved body, lines 1592–1607). Keeping it function-local preserved strict dedent-only for the largest move. |
| D6 | The shift walk is NOT sub-extracted from the per-component loop. | `recordEdge` has ~20 captures across three nesting levels; extracting it alone is a semantic rewrite. Costs the oracle test its fine granularity. |
| D7 | Retargeting the existing `shiftWalk` oracle at production is deferred (test 24). | Rewriting the independent oracle in the same increment as the move removes the only thing that could catch a transcription error. |
| D8 | **Snapshot three-place-rule gap RECORDED, not fixed**: `anchor_incumbent`, `this_epoch_anchor`, `emitted_anchor`, all `eq34_*` maps/counters, `rpb_carry`, `n_gated_*` are neither fingerprinted nor restored nor written, and carry no "Not snapshotted:" exemption — while `local_carry` IS restored. | Pre-existing; **a warm resume starts with an empty tree while holding carries only that tree gives meaning to.** Byte-inert under `carry_invariant` (which forces `eq34_round` false, so targets were already 0) but **field-live on the pin-round lineage** and under-reports run-end telemetry on any resumed run. **OWN CARD — still open.** |
| D9 | **Station-0 slip-detector asymmetry FLAGGED, not fixed**: `ApplicationNetworkSetup.cpp` gates MW/IF-TD detector construction on the RAW `config_.networkPhaseDatumPrioritized()` while the run TU uses the OR-ed `ctx.knobs` value — under an eq34-only config **the pivot is a fully-carrying station with no slip path** (standing invariant #1, one-station instance). | Outside the region; would change behavior. **Still open.** |
| D10 | Stale chain-closing comment corrected in the run TU only (`// end anchor election (!ensemble && !eq34)`); not copied into the seam. | Comment-only, byte-inert. |
| D11 | The ensemble arm was the one droppable if arm budget were hard — then the ensemble branch would have STAYED inline. | **Ruled: move it and add `pdens`.** Relocating 14 lines nothing executes is the INC-7b defect at small scale; with the every-row + emitted-epoch-floor assertions the witness is genuine (~200 unpinned shared PB columns with an empty gauge vector would fail the W5 null-space gate, so the conjunct on all 10 rows can only arise if the sum-to-zero gauges were actually folded). |
| **D12** | ⚠**AV**: **the builder step is NOT a pure dedent** — the five census objects are *declarations*, two carrying a sizing ctor arg (`sat_el(stations.size())`, `usable_keys(stations.size())`). Aliasing them naively loses the sizing and `sat_el[i]` becomes an **unchecked out-of-bounds write at i ≤ 49 — heap corruption in Release, no compile error, no gate signal**. | Resolved by the `std::move` epilogue (§1), which keeps the declarations byte-identical AND re-establishes freshness structurally: otherwise a later hoist of `cnd` out of the epoch loop (permitted by the signature) turns `cover`/`cand_set`/`union_keys` into accumulating cross-epoch maps and nothing in the moved body ever clears them. |
| **D13** | ⚠**AV**: the stale "ungated union" comment corrected in the same commit (§4.3). | Predates the pdp gate; leaving it licenses a pre-gate census. |

---

# 8. TRAPS THAT REMAIN TRUE OF THE SHIPPED SEAMS

*Retained from the risk register. R1 (file drift), R3 (post-cut baselines), R5 (mixed dedent), R13
(CMake conflict), R14 (`PhaseDatumCandidates` header placement) and R15 (suite-count inflation)
were execution-time risks and are discharged. **R2 is not discharged — it is §2.*** Two of the
discharged ones carry facts worth keeping: **R1** measured `ApplicationNetwork.cpp` at **9,135 →
9,162 → 9,336 lines within a single session** (CB-ADM), which is why a cut at a memorized offset
slices mid-statement; **R15** found the tree carrying **~839** TEST macros against the last
committed **805/804/1**, from CB-ADM (+12) / Iono-2 (+3) / LF-1, so 8b's additive count had to be
reported as a delta against the pre-8b tree state, not against the last commit.

| # | Trap | Concrete failure mode |
|---|---|---|
| **R4** | **A carry container copied by value.** | `local_carry`/`rpb_carry`/`prev_carry` are value-mutated and copyable; **a lost `&` compiles clean and deletes the S-transform with no crash and no counter change.** Same class for `anchor_incumbent` (sticky degrades to a fresh election every epoch — invisible except as handover churn on a graded run). *Mitigation: `ctx&` once for eq34; literal `&`-audit against the header prototypes as a review checklist item; `eq34x`'s shift counter non-zero.* |
| **R6** | **`eq34_reref` rebuilt seam-locally.** | The natural mistake (cleared and refilled every call). Consumers at `networkInjectD2CarryPrior` (both branches), `networkStashCarry` and the M2 VCE snapshot would see an empty set and **fold every re-referenced PB prior in its RETIRED frame**. The in-region `local_carry` erase still happens, so **the local side looks clean while the shared border is poisoned — and `eq34_reref_total` still reports non-zero.** *Mitigation: alias into `ctx.carry`, never a local (§4.2); test 27.* |
| **R7** | **`eq34_ppp_utils` construction hoisted or localized.** | Hoisting out of the `if (!eq34_ppp_utils)` guard fires `PppUtils`' ctor debug log on **every** non-eq34 arm (spdlog output changes on all arms). A seam-local instance leaves the pointer **null at the emit-rebase site ~4,700 lines later** → `lam` stays 0.0 and every `NetworkEmitRebase::Entry` gets `lambda_m = 0`. *Mitigation: pass `LogManager&`; keep the lazy guard byte-identical inside the seam.* |
| **R8** | **Prologue side effects lost or duplicated.** | It PRUNES StateManager PB states and is the sole writer of `n_gated_*`. Computing it twice double-counts run-end telemetry; skipping the prune leaves per-station column sets inconsistent with the gated union. *Mitigation: one seam fn, one call site, counters by non-const `std::size_t&`; `pdgate` asserts both > 0; tests 20/21.* |
| **R9** | **`cov` divergence between the two copies.** | A later edit to one changes eq34's coverage ranking but not the anchor's. Also an unrelated identically-named `cov(...)` elsewhere in the run TU produces false hits in any "all uses moved" grep. *Mitigation: cross-reference comment in both; **preserve the 0-on-miss contract exactly** — R10(a) depends on it.* |
| **R10** | **"Cleanups" that break latent guards. Verbatim means verbatim — do not simplify.** | (a) the r1 coverage fallback dereferences `stations[r1]` while `r1 == size_t(-1)` — safe **ONLY** because `cov()` returns ≥0 and `best` starts at −1; **reordering the `\|\|` operands gives an OOB read at 50-sta scale.** (b) `grp_cons`/`grp_sig` are deliberate copies of structured bindings (C++17 forbids capturing them) — eliding them breaks compilation confusingly. (c) `st.pinned_rpb_keys.clear(); // no-op` in the anchor branch keeps the three clear-blocks textually identical. (d) the anchor comparator captures `c` from a structured binding — MSVC accepts it, it predates the increment, **out of scope**. |
| **R11** | **Ordering contract broken.** | See §4.1 — the failure is SILENT (the rank cross-check counts recorded pins, not allocated columns) and surfaces only as W5 withholds / non-PD local blocks on field data. *Mitigation: the ordering contract as written in `NetworkChainDatum.hpp`, mirrored in the D3c comment block naming the seam as its producer.* |
| **R12** | **`anchor_incumbent = this_epoch_anchor` "tidied" into a move/swap** for symmetry with the anchor branch. | Empties `this_epoch_anchor` before the emit-site handover loop: **`emitted_anchor` never advances, no `eq34_reattach`/`anchor_handover` events, and the next genuine handover compares against a permanently stale baseline.** Product bytes may stay identical on a short matrix. *Mitigation: the copy-vs-swap asymmetry is DELIBERATE; annotate both sites. `obs`/`pdanchor` event-row counts are the check.* |

---

# 9. CROSS-CARD DEBT THIS INCREMENT SURFACED

- ⚠ **Stale-doc finding, still owed to another card:** the INC-6 slip-QC claim — *"the
  allon/jw/snap arms run `if_phase_td` armed and the harness asserts non-vacuous QC row counts"* —
  is **measured FALSE**: every `_dcmp_*` arm has it OFF. Re-measured `extends`-resolved
  2026-08-21 (`scripts/gnss_config_chain.py`): **19** `_dcmp_*` configs exist repo-wide (14 in
  `config/test_suite/`, 5 in `config/archive/`), of which the gate script drives the **13**
  audited in §5.1 — and `network.cycle_slip.if_phase_td` resolves FALSE on all 19, with
  `mw_detect` resolving false on all 19 too. The conclusion is unchanged and now covers the
  whole lineage rather than a subset. *(This line previously said "all 15 configs checked",
  which matched neither the 13 gate arms nor the 19 files on disk.)* **INC-6's slip-QC seam was
  therefore gated by a matrix that never armed IF-TD**, and the QC-row assertion does not exist in
  `run_inc4_gate.ps1`. `docs/network_decomposition_plan.md` §9 (subsection "⚠ What this matrix does
  NOT cover — slip QC") now records this refutation directly, and its Appendix A carries the stale
  8b anchor row (`4919-5556`, stale by ~-3,400) under an explicit HISTORICAL banner. Both belong to
  whoever owns the Architecture/decomposition group.
- `docs/SPEC_cb_admission_card.md` §4 declares the phase side out of scope **because** "anchor
  election is mid-decomposition (8b split pending)" — a written contract not to enter this region.
  Its §8 reserves a pdp-threading follow-up that WOULD enter it if CB-ADM's INC-2 smoke shows
  persistent `PB|C##` nulls; that follow-up now writes against a carved seam. If it lands, the
  prologue seam is the natural place to thread admission state — `networkBuildPhaseDatumCandidates`
  was given a signature that can absorb a census reference later without param-list churn.
- The ground rules of `docs/SPEC_slip_qc_hardening.md` §1 (default-OFF + same-build byte-identity,
  Release-only with a renamed exe, name-scoped zombie kills, additive unit tests, explicit CMake
  lists) applied to this increment unchanged and apply to its outstanding gate work.

---

# 10. HISTORICAL RECORD — the pre-cut region map and the concurrency plan

*Appended 2026-08-15 so the executed-plan material is retrievable without re-reading git history.
These are **pre-cut** addresses against `ApplicationNetwork.cpp` at snapshot 2026-07-21 23:02, file
length 9,162 — they do NOT resolve in today's tree. Guards and closers **STAYED**; only bodies
moved, and dedent depths differ per block (never apply a blanket dedent).*

## 10.1 Region map (was §0.1)

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

The anchor body sat at the **same column as its own `} else {` and its own closing `}`** (verified:
2176 `            } else {`, 2177 `            std::map<char, std::vector<size_t>> cand;`) — dedent
12→4, not 16→4. Relocation ranges as executed: 8b.i **1421 + 1423–1460 + 1465–1521** (dedent 12→4;
the range is **three fragments**, not contiguous — 1422 (`sticky`) stays and 1461–1464 (`cov`) is
re-declared); 8b.ii **1524–1537** (16→4) and **2177–2224** (**12→4**) — ⚠**AV (major)**: the
ensemble cut range is **1524**–1537, not 1523, because 1523 is the guard; 8b.iii **1539–2175**
verbatim (16→4), with `struct GKey` left function-local (D5). Guards 1523 / 1538 / 2176 / 2225
stay, and the 4-way skeleton is 1416/1420/1523/1538/2176/2225. **Control-flow fact (was §0.2):**
**NO** epoch-level `continue`/`break`/`return`/`goto` anywhere in 1416–2255 (~60 hits, all
inner-loop or lambda/comparator returns).

## 10.2 The concurrency ruling (was 9.1) — do INC-8b, not INC-9b

`buildCarryDdPriors` is a ~1,430-line capture-all lambda invoked twice, and **nine LF-1 hunks land
inside it** — including a structural rewrite of the MW fold loop from serial `for` to
`#pragma omp parallel for` with a per-block `WlCounters` array, three in-place counter renames, and
a new `finalizeLaneBatch` call — plus three more LF-1 hunks outside it that are read from inside.
LF-1's remaining work points back into those same lines. **INC-8b's region has zero hunks from any
of the three cards**, with ~140 untouched lines of margin below and ~30 above. Hence 8b, not
INC-9b.

## 10.3 Live-session state (was 9.2, re-measured 2026-07-22 07:31)

⚠ **There were FOUR uncommitted cards, not three** — the fourth missed by the survey because its
files sit outside every region the survey was pointed at:

| Card | Footprint | Last edit | Status |
|---|---|---|---|
| **Slip-QC hardening** (`docs/SPEC_slip_qc_hardening.md`) | `slip/IfPhaseSlipDetector.{hpp,cpp}` +442, `slip/NetworkSlipQc.{hpp,cpp}` +181, `SlipDetectorRegistry.cpp` +9, NEW `slip/NetworkRawObservables.{hpp,cpp}`, NEW `tests/test_if_phase_slip_detector.cpp` + `tests/test_network_slip_qc.cpp` | **00:12:39** | **ACTIVE, mid-edit** |
| CB-ADM | pre-loop census + Assembly registration swap | 00:06:57 | uncommitted, awaiting review |
| Iono-2 | dual-layer SH, `sh_model2` | Jul-17 | dormant |
| LF-1 lane-fold | `buildCarryDdPriors` OMP rewrite | Jul-17 | dormant |

**`build/x64-Release/gnss_processor.exe` linked 00:11:43 — and `IfPhaseSlipDetector.cpp` was edited
at 00:12:39, 56 s LATER**, so the tree carried an unbuilt edit and **no binary built from it
corresponded to any reviewed state of the slip-QC card**. Slip-QC does not collide with the 8b
region (slip path vs election path) and is *plausibly* byte-inert on the matrix (no `_dcmp_*` arm
arms `if_phase_td` or `mw_detect`), but **plausible is not proven — `NetworkSlipQc.cpp` +141 lines
is on the PASS-A drain path all arms execute.** *(9.2a, the inertness PRECONDITION on 8b.0:
baselining new goldens is only sound if the four-card dirty tree is byte-inert on the matrix,
because the candidate binaries come from that same tree — so wait for a stable built state, build
Release, run the existing 13 arms against their committed goldens, and only capture the four new
arms' `_a`+`_b` if that is GREEN; any trip means 8b cannot be byte-gated until the offending card
lands. Worth running on its own merits: it is the only measurement that says whether four
simultaneous uncommitted cards preserved the default-OFF byte-identity contract.)*

## 10.4 Merge-conflict surfaces (was 9.3 — all add-only, all rebaseable)

| File | Our edit | Concurrent |
|---|---|---|
| `CMakeLists.txt` | +1 source line in the `filter/` block, +1 test line | CB-ADM inserted `NetworkBiasAdmission.cpp` 1–3 lines away; test cluster also live |
| `include/filter/NetworkRunTypes.hpp` | +`PhaseDatumCandidates` (append, trailing section) | Iono-2: `CarryPrior::sh_layer`, `CoupledShared::sh_layer`, 15-del/56-add `ShDevCarry` rewrite — **keep our struct far from `ShDevCarry`** |
| `src/core/app/ApplicationNetwork.cpp` | the region + 2 includes | CB-ADM above the region, Iono-2 below it |
| `include/filter/NetworkPhaseDatum.{hpp,cpp}` | +3 functions | **clean** (mtime = the INC-8 commit) |
| `tests/test_network_phase_datum.cpp` | +tests | **clean** |
| `include/app/NetworkRunContext.hpp` | **none** (D3) | 8 hunks — avoided entirely |
| `include/app/Application.hpp` | **none** (free-fn shape) | 3 Iono-2 hunks — avoided entirely |

Two of the three heaviest-contended headers are untouched purely as a consequence of the seam-shape
ruling — a second, independent argument for free functions.

## 10.5 Sequencing (was 9.4)

Preferred: land 8b after CB-ADM commits; otherwise proceed, re-running the §0 re-anchor immediately
before EACH sub-step. Never `git add -A` — stage exactly `ApplicationNetwork.cpp`, the two seam
header/impl pairs, `NetworkRunTypes.hpp`, `CMakeLists.txt`, the test files, the four arm configs,
`run_inc4_gate.ps1`, the four manifests. Build/run hygiene: Release runs use a renamed exe
(`gnss_processor_8b.exe`) **passed to the gate script via `-Exe`** so the linker never hits the
other session's lock (`LNK1104` = lock → retry); pre-kill zombies **name-scoped to your own exe
only** (a `gnss_processor*` wildcard sweep kills the concurrent session's live runs); **never run
the producer from the Debug tree.** If CB-ADM's pdp follow-up lands first, 8b.i's prologue seam is
the natural place to thread admission state (§9).
