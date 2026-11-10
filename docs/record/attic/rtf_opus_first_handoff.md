# RTF remaining-cards — Opus-first / Fable-review handoff

**Purpose.** Prime a *fresh Opus 4.8 Ultracode session* to implement the remaining RTF
cards, and tell the *follow-up Fable reviewer* exactly what to scrutinize. Strategy
(user's): Opus attempts the first pass to save Fable tokens; Fable reviews/corrects
after. This doc is the per-card map of **where that bet pays off (yellow) vs. wastes
tokens (red)**, grounded in the actual code seams.

Generated 2026-07-02 by a 10-agent deep-read workflow. **Every quoted line number WILL
drift** — the tree churns under concurrent sessions (Cards 4 & 5 landed *mid-generation*).
Re-grep the quoted symbol before editing anything.

---

## 0. Re-baseline BEFORE touching anything

The working tree is a moving target. At the start of the new session, do this first —
do **not** implement from the plan text or the memory index alone (you will clobber or
duplicate concurrent work):

```
git status --short
git diff HEAD -- <the files a card claims to touch>      # confirm what's actually there
<vcvars> && cmake --build build/x64-Release               # confirm green
build/x64-Release/gnss_tests.exe                          # confirm suite green
```

**Current in-tree state (verified this session):**
- **Cards 1–3: DONE + extensively re-validated** — clean build; full suite 542 pass / 1
  known skip / 0 fail (at the Card-3 tree state); Card-2 integration test ran on real
  BRDC/SP3/CLK fixtures (orbit 3.68 m / clock 0.65 m); 8-dimension adversarial review +
  field-map cross-check vs the RTCM decoders = **0 confirmed defects**.
- **Cards 4 & 5: DONE (landed concurrently), 3-lens verified by that session** — Card 5's
  one CONFIRMED-moderate defect (SP3/CLK-present refusal missed the CLK-without-SP3 set →
  silent orbit/clock frame split) is **already fixed** (guard gained `|| getClockCount>0`).
  Suite now 550 / 549 / 1 skip. **Not independently re-validated in this session.**
- **Remaining: Card 6, Card 7, Card 8 (armed), then the RTF-3…9 tier.**

The one open Card-5 item is **not review-catchable** — see §2.

---

## 1. Strategy map — where Opus-first pays off

No remaining card is pure-green: every one has a claim Opus's build + tests + byte-identity
cannot reach. That residue is the whole point of the color.

| Card | Verdict | Opus-first economics | The un-self-verifiable claim |
|---|---|---|---|
| **7** consumer pinned-eval + echo | 🟡 | **Strong** — seams exist, fully seam-testable | D7 re-reference sound; divergent branch only on armed data |
| **5** producer core | 🟡 *(built; verify only)* | **Do not re-implement** | emit-site IODE-stamp deviation (§2) — armed data |
| **6** eph-cutover re-level | 🟡 | Good — pure helper testable | Δ_eph lockstep invisible to jump + NIS tests |
| **RTF-4** wire protocol | 🟡 *(Tier-1 only)* | Good for Tier 1 | handshake recovers full datum state on reconnect |
| **RTF-6** pivot-station fallback | 🟡 | Green long-tier / yellow short-tier | unpinned-gauge conditioning through an outage |
| **RTF-9** MDB integrity | 🟡 *(analytic only)* | Good for the floor | honest per-parameter MDB (DIA separability) |
| **RTF-3** streaming front-end | 🔴 | **Poor** — Opus drafts, Fable re-architects | concurrency correctness; batch≡stream; no harness |
| **RTF-5** datum re-level / handover | 🔴 | **Poor** — design unmade research | "per-signal bias constants" estimability-false as stated |
| **RTF-7** network AR for RT | 🔴 | **Poor** — substrate ~80% unbuilt; needs live frac_cyc | RT policy tunable only against a running service |
| **RTF-8** atmosphere product | 🔴 | **Poor** — needs dense data not on disk | STEC datum alignment; global-only test = false green |

**Yellow → run Opus-first, budget real Fable review. Red → Fable-first design (or defer);
Opus-first burns tokens on something Fable rewrites.** RTF-3 has one green carve-out (the
`processSyncedEpoch` extraction); the rest of RTF-3 is Fable's.

**Suggested sequence:** Card 7 (cheap, unblocked, fully seam-testable) → verify Cards 4+5 →
Card 6 → **Card 8 armed run** (the numerical truth for every yellow claim) → RTF-4 Tier 1
(bankable) / RTF-6 Session A (bankable) / RTF-9 analytic floor. Hold RTF-3/5/7/8 for Fable
design passes.

---

## 2. The one cross-cutting review item (do this first in the Fable pass)

Card 5's concurrent implementation solved the IODE-stamping crux by **deviating** from the
plan: instead of threading the per-station emission-time IODE out of `computeEmissionState`,
it queries `LiveEphemerisStore::getInstance().computeSatelliteClock(sat, se.time)` at the
**emit site** (`ApplicationNetwork.cpp:4254-4260`, justified in the `:4241-4253` comment).

- **The risk:** at an **IODE-roll boundary epoch**, a station whose `emission_time`
  straddles the `toe` cutover could have modeled its C0 against version *N* while the
  emit-site query at `se.time` returns *N+1* → a 1-epoch stamp/baseline mismatch. Unit-invisible;
  only an armed roll (Card 8) exposes it.
- **The coupling:** Card 6's re-level compares stamped `clock_iode` against
  `last_emitted_iode_`. **Cards 5 and 6 must agree on which version-selection defines
  "the version this epoch used."** If the stamp source (emit-site `se.time` pick) differs
  from the geometry's source (per-station `emission_time`), Card 6 can fire a *phantom*
  roll or *miss* a real one.
- **Verdict:** judge whether the 1-epoch skew is (a) impossible, (b) harmless because
  Card-6 re-level absorbs it, or (c) a real mis-stamp. **Do not "finish the out-param
  plumbing"** — it would be dead code; the deviation is intentional.

---

## 3. Meta-rules for the Opus session (the token-saving levers)

1. **Leave a "design rationale + what I could NOT self-verify" trail.** This is *the*
   lever that makes Fable review cheap. Name your estimability assumptions and byte-identity
   reductions explicitly (in card notes + code comments) so Fable checks the right 3 things
   instead of reverse-engineering intent and re-deriving. An unannotated draft is where
   Fable tokens actually bleed.
2. **Byte-identity is proven by a written reduction argument, not asserted from a green
   suite.** Every new behavior behind its default-OFF/brdc_rts gate; show every guard.
3. **Scope discipline on bimodal cards.** RTF-4 = **Tier 1 only** (the 4076 encoder is
   frozen-deferred). RTF-6 = land **Session A** (green contract event) first; the short-tier
   gauge is a *separate Fable-gated* increment. RTF-9 = **analytic floor only**; the empirical
   α/β calibration is unrunnable until a service exists.
4. **The network suite has no numerical regression** (closed-loop is the only proxy). A
   green suite is NOT correctness for any estimability claim — say so, don't imply it.
5. **Re-grep every line number.** Concurrent sessions shift them constantly.

---

## 4. Per-card briefs

### Card 7 — consumer pinned clock eval + echo verification + age skew  🟡 (best yellow; do first)
**Scope.** The consumer half of the RTF-2 anchor contract: (a) pin the broadcast clock
baseline to the stamped `clock_iode` instead of the orbit anchor (D7); (b) verify the
orbit-coherence echo, count per-sat skews, withhold only when a budget is set; (c) add
`network_consumer.orbit_clock_max_skew_s` (default 0 = off) + a skew counter. All
default-OFF / byte-identical. **Deps:** Cards 3+4 (done). Independent of 5/6. 1 session.

**Approach (4 edits, consumer side only — no producer/wire/store changes):**
1. **Pinned eval** — `GnssCorrections::getSatelliteClockCorrection`: `bc` is currently
   pinned to `ssr_iode = ssr->iode` (orbit anchor, ~`:192-201`). Introduce a *separate*
   clock-pin `= (ssr && ssr->has_clock) ? ssr->clock_iode : -1` and pass THAT to
   `LiveEphemerisStore::computeSatelliteClock(sat,time,clock_pin)`. The existing gate
   `iodeMatches(brdc_iode, ssr->clock_iode)` (~`:261`) then drops correctly. **Leave the
   ORBIT overlay eval in `ModelGeometry.cpp:1278-1287` pinned to `ssr->iode`** — do not touch.
2. **Echo verification** — when `orbit_iod_ssr>=0`/`orbit_epoch` are set on the merged
   `OrbitClockCorrection` (`SsrCorrectionStore.hpp:138-139`), compare against the record's
   own applied orbit content; count skews; force the `:280` else-branch drop *only* when
   the budget > 0.
3. **Skew counter** — clone the RTA-13 pattern: a `mutable` counter + getter on
   `SsrCorrectionStore` (next to `bias_attr_mismatch_`), callable from the const
   `getSatelliteClockCorrection`; summary via the `RealTimeEngine` `logBiasAttrMismatchSummary`
   pattern.
4. **Config** — `orbit_clock_max_skew_s` in `NetworkConsumerConfig` (parse in the
   `network_consumer` block); read via the existing `config_ ? … : default` null-guard idiom.

**Top traps.** Conflating the clock-pin with the orbit anchor (silently defeats the card);
counting must be *unconditional* on a detected skew, only *withhold* is budget-gated;
`getSatelliteClockCorrection` is const → counter must be `mutable`+locked (store-side clone
sidesteps it); route the withhold through the existing `:280` drop, not a parallel return
that skips the broadcast-grade bookkeeping.

**Fable-review focus.** Is pinning the clock baseline to `clock_iode` (while orbit stays on
`iode`) the right estimability choice under a producer whose C0 references a *different* eph
version than the orbit anchor? Verify the byte-identity invariant `clock_iode==ssr_iode` on
**every** current merge branch (combined `:139`, standalone inherit `:183`). Is the echo
check a tautology for combined records (echo==self)? **Self-validatable:** build + suite +
seams tests (two eph versions, assert the served baseline switches with `clock_iode`) +
config round-trip + byte-identity reduction. **Armed data:** only the live divergence branch
(Card 8) — not a merge gate.

### Card 5 — producer brdc_rts core  🟡 (BUILT — verify, do not implement)
**Do not re-implement.** Present + 3-lens-verified in-tree. First action: `git diff HEAD --
src/core/app/ApplicationNetwork.cpp src/core/model/PppModel.cpp include/config/GnssConfig.hpp`,
read the Card-5 blocks, build + suite, hand to Fable. Only if the diff shows a bullet
*missing* (concurrent drift), implement that one bullet against these coordinates:
- brdc_rts flag `ApplicationNetwork.cpp:480-481`; nav-load block `:547-599` (parse → insert
  into `LiveEphemerisStore::getInstance()`, empty-store hard error, SP3/CLK-present refusal
  `:592-598`); gate suppression `:683` → `PppModel.hpp:425-426` + erase guard `PppModel.cpp:717`
  `if (geom.broadcast_only && !broadcast_phase_gate_suppressed_)`; anchor-σ `:2165` via
  `GnssConfig::resolveNetworkAprioriClockAnchorSigmaM()` (`GnssConfig.hpp:1452-1458`: explicit>0
  wins, brdc_rts→3.0, sp3→`initial_std.sat_clock`) consumed at fold `:3739` (`<100 m` binding
  gate unchanged `:3740`); IODE stamp `:4254-4260`; `apriori_product_id` `:874-879`.

**Fable-review focus.** (1) **The emit-site deviation** (§2) — the one claim neither build
nor unit tests reach. (2) The **Card-6 version-selection agreement** (§2). (3) Is 3.0 m the
right anchor strength, or is it effectively inert vs. the data (60× looser than the σ=0.05
that collapses cold-start rms 56→2.9 m)? (4) sp3-mode byte-identity: confirm
`resolveNetworkAprioriClockAnchorSigmaM()` returns bit-identical to the pre-card inline value,
esp. the `initial_std.sat_clock < 100` sub-case. (5) D1: prove the closed-loop *consumer's*
PppModel is never reached by `setBroadcastPhaseGateSuppressed(true)`. **Armed data:** the
emit-site equivalence + the 3.0 m conditioning effect (Card 8).

### Card 6 — ephemeris-cutover re-level (D3)  🟡
**Scope.** A deterministic KF state-mean surgery: on an IODE roll, shift the sat's estimable
clock-state mean by `−Δ_eph` (`Δ_eph = c·(brdc_new.bias − brdc_old.bias)`, bias in seconds,
state in metres) so the delta-to-broadcast C0 stays continuous-in-reconstruction — **no disc
bump on a successful re-level**. Fallback = per-sat forced disc bump when the previous eph
version is gone. Add `last_emitted_iode_` map; snapshot it. Default-OFF (brdc_rts only).
**Deps:** Cards 1–5. Blocks Card 8. 1 session + real Fable review.

**Approach.**
- **Insertion point (crux):** the re-level must run on `x_s` **before** `toSolutionEpoch`
  (`~:4240`) so every downstream QC (clock-model NIS, clock-monitor, D2 stash) reads the
  shifted state — i.e. a new brdc_rts block right after the withhold gate (`~:4237`).
- **Detection:** query `computeSatelliteClock(sat, se.time)` current IODE in the block
  itself and diff against `last_emitted_iode_`. Card 5's stamp block runs *after*
  `toSolutionEpoch`, too late to diff.
- **State index:** `x_s(est.clockColumn(sat))`, wrapped in `try/catch(...) continue`; iterate
  `registered_clock_sats` only. Key: `StateKeys::makeSatClockKey(sat)`.
- **Δ helper:** extract `NetworkEstimator::computeEphCutoverShift(old_bias_opt, new_bias)
  → optional<double>` — pure, unit-testable (known af0 diff → exact metres; missing previous
  → nullopt). Evaluate `bc_old = computeSatelliteClock(sat, se.time, old_iode)` pinned; if
  nullopt or `.iode != old_iode`, take the fallback.
- **The shift (coherence — the whole card):** (1) `x_s(clockColumn) -= Δ`; covariance and c1
  untouched. (2) **D2 carry: do NOT mutate `prev_carry`.** It's already folded into this
  solve and re-stashed fresh from the shifted `x_s` post-emit — an explicit mutation
  **double-shifts**. (3) clock-model: shift the object the NIS actually diffs against —
  **`clk_pred[sat].x0`**, not just `clock_osc[sat].x0` (trace the `clk_pred` lifetime; it's a
  pre-solve snapshot). (4) jump test: shift `prev_clk[sat].first -= Δ` so the adjacent-epoch
  baseline is in the new frame.
- **Map + snapshot:** update `last_emitted_iode_` at the **end of the emit block, for emitted
  (post-prune) sats only**; add it to `NetworkStateSnapshot` (new record tag — J/K/L/Z/R/A/Y/C
  are taken) so a warm resume doesn't re-level against a stale baseline.

**Top traps.** D2 double-shift (the #1 trap — the plan text says "shift the carry" but the
correct action is *nothing*, it rides `x_s` for free); shifting `clock_osc` but not the
`clk_pred` the NIS reads; placing the block *after* `toSolutionEpoch` (split-brain: emitted
C0 unshifted, QC shifted); advancing `last_emitted_iode_` for withheld/quarantined sats;
wrong sign/unit; forgetting `prev_clk`; skip GLONASS; possible `/bigobj` C1128 (add
`ApplicationNetwork.cpp` to the existing `set_source_files_properties` list).

**Fable-review focus.** Prove the same Δ reaches `x_s` / D2-next-stash / `clk_pred` /
`prev_clk` and **none is shifted twice**; invisibility to **both** fault tests on an actual
roll; ordering vs Card-5 stamp + the prunes; sign+unit via the reconstruction identity
(`brdc(new)+C0` continuous to <5 cm); fallback routes to the per-*sat* forced bump; snapshot
tag doesn't collide. **Armed data:** the end-to-end continuity + zero-disc-bump signatures
(Card 8) — no harness rolls eph.

### RTF-4 — wire protocol  🟡 (Tier 1 only)
**Scope Tier 1 (recommended).** The existing self-describing line format IS the wire format
(conventions §4). RTA-3/4/5/6/10 already made it self-describing; `NetworkProductTail` already
IS the receive-side decoder. Build: (1) refactor `NetworkProductTail::Impl` so its byte source
is an abstraction (file read vs socket recv) — **reuse the one parser**, don't duplicate the
torn-line / epoch-completion / contract-heal logic; (2) a producer socket/caster sink that
tees `NetworkProductWriter`'s bytes; (3) the connect handshake = push **S line → latest H →
next E block** (mechanically identical to the mid-file tail join). Default-OFF; byte-identical
when off. 1 session + light Fable.

**Tier 2 = do NOT build.** Frozen-deferred until a standard-SSR rover exists. No `RtcmBitWriter`
exists. Confirm scope with the user before writing any 4076 encoder.

**Top traps.** Building Tier 2 speculatively; reimplementing the line parser instead of
refactoring the byte source (torn lines split *anywhere* on a socket, more aggressively than
file buffering); wrong handshake order (H-before-S leaves a joiner mis-aligned, silently);
editing `NetworkProductWriter`'s bytes instead of teeing (breaks twin-file/replay tests).

**Fable-review focus.** Handshake **completeness** — does {S + H + next E} reconstruct full
datum state, or is there stream state a joiner needs that the push omits? (Cross-check
`NetworkProductIO.hpp:370-389`: disc-counter adoption needs `serialize_disc_counters` ON;
contract blind until next S.) Torn-line invariant across `recv()` boundaries. **Armed data:**
a live socket run across a real reconnect.

### RTF-6 — receiver-datum (pivot-station) fallback  🟡 (split into sessions)
**Critical disambiguation:** there are TWO pivots. RTF-6 is the pivot **STATION**
(`clock_datum_station`, `reference_receiver_id`), whose change is **GLOBAL** — it maps onto
`markDatumChange()` with **no argument** (`force_all_bump_`) + a re-minted generation id, NOT
the constellation-scoped satellite-pivot machinery (that's RTF-5). Conflating them under-signals
the break.

**Session A — LONG-tier contract event (🟢 do first, bankable):** a receiver-datum switch must,
in one atomic emit, (a) `markDatumChange()` [global], (b) re-mint `generation_id` so the
consumer's gid-change global reset fires (`RealTimeEngine.cpp:1606-1641`), (c) update
`reference_receiver_id` + force an S re-broadcast that epoch, (d) advance the datum-station
baseline at the emit site (deferred discipline). Add `last_emitted_datum_station`; default-OFF
`network.datum_fallback`. **Fully self-validatable** (detector unit test + twin-file + round-trip
to the consumer reset path). ~1 session.

**Session B — the fallback DRIVER (the hard part):** ordered roster + runtime datum-station
reassignment. Today `is_pivot` is construction-time and `stations.front()==pivot` is assumed
across PASS-A selection, the `:2914` pin fold, the `:3141/3152/3174` aborts, and the
`:3021-3028` carry-skip. A real swap must re-pin the new datum's ambiguities, un-pin the old,
and rederive `shared_phase_keys` — an estimability operation, not a flag flip. Make `is_pivot`
a per-epoch role chosen from an ordered roster with N-epoch hysteresis. ~1.5 sessions.

**Short tier (🟡 separate, Fable-gated):** ride the phase-bias gauge on the D2 carry with no
receiver pinning for a 1..M-epoch gap + a gauge-drift monitor. Opus builds the mechanism;
Opus CANNOT certify the unpinned gauge stays conditioned — that's Fable + armed data.

**Top traps.** Constellation-scoping a receiver-datum change; swapping the pivot by flipping
`is_pivot` (misses the re-pin sites → singular/drifting solve); no hysteresis (1-epoch gap →
reset storm); advancing the baseline before the emit; snapshot the new state. **Armed data:**
gauge drift vs threshold + clean-swap convergence under an induced outage.

### RTF-9 — MDB-anchored integrity  🟡 (analytic floor only)
**Scope the buildable half precisely:** the analytic Baarda internal-reliability floor
(`MDBᵢ = σᵢ·δ₀/√rᵢ`, `δ₀ = dataSnoopingCritical()+0.8416`) needs **no** live statistics — it's
closed-form. Only α/β calibration + integrity *validation* are armed-data follow-ups (defer).

**Approach (default-OFF `network.emit_mdb_ura`).** Reuse the user-side Baarda machinery
(`KalmanFilter.cpp:1723-1789`, `kRedFloor=1e-6`). Add the MDB floor as a **second `std::max`**
at the URA emit site (`ApplicationNetwork.cpp:4496-4519`, next to the warmup floor — structurally
identical, byte-identical when off). Consumer: fold the (now floored) per-sat URA into the FFR
AR gate (`AmbiguityResolver.cpp`/`IlsCore.cpp`) behind `network_consumer.fold_ura_into_ar`;
keep it thin — the true protection-level statistic is Fable/armed territory.

**Top traps (all Fable-review-focus too).** `rowRedundancy` is **per-station-row**, but the MDB
is on the **emitted clock parameter** (fused across all stations) — reading one row over-optimizes
the floor. The σ in `δ₀·σ/√r` is the **a-priori measurement σ**, not the posterior clock-state σ
already in the URA — conflating them (just multiplying `√scov` by δ₀) is dimensionally plausible
but **semantically wrong** (input- vs output-referred; needs the clock-to-input sensitivity). The
analytic single-outlier MDB is **optimistic exactly in the detectable-but-not-identifiable lock-in
regime** the card exists to bound (the ep42/G15 mechanism) — Opus cannot know its own floor is too
low. `δ₀`'s per-row critical value likely needs Bonferroni/Šidák over 50–300 stations × ~60 sats.
Don't rename a URA floor "protection level."

**Self-validatable:** build + suite + twin-file byte-identity off + pure-helper (`δ₀·σ/√r`) +
URA-monotonicity. **Armed data:** α/β calibration + realized-bias-respects-floor (post-Card-8).

### RTF-3 — multi-station streaming front-end  🔴 (Fable-first; one green carve-out)
**Why red.** A multi-subsystem concurrency build ("nothing exists"): per-station
transport/framer/assembler fan-out, an ingest/solve thread split with epoch-granular
backpressure, and a **~3850-line** epoch loop (`ApplicationNetwork.cpp:2167-6017`) to make
re-entrant while preserving heavy cross-epoch state. Concurrency correctness, station-identity
integrity, and batch≡stream equivalence are all un-self-verifiable with no harness. Opus-first
→ a large draft Fable must re-architect.

**The green carve-out Opus may do solo:** extract `processSyncedEpoch` and prove **byte-identity
of the batch path** through it over a **multi-epoch** run (dropped cross-epoch state is invisible
at epoch 1). Thread all persistent accumulators through a context struct — it is NOT a se-only
pure function. Stop there; hand the streaming/concurrency build to Fable.

**Landmine traps** (so Opus doesn't wander in): the loop "already consumes only `se`" is the most
misleading line in the card — it closes over dozens of persistent accumulators. MSM *does* carry a
station_id (`RtcmMsmDecoder.cpp:404`) but DF003 is caster-unreliable → identity must come from the
stream boundary. `PppModel`'s ctor hard-requires `RinexParser&` (GLONASS FCN seeded from the
header) → the header-free seam is the deepest cut. Backpressure lives on the assembled-epoch queue,
not in `NtripClient`'s byte callback. **Streaming output is arrival-timing-dependent → not
byte-identical**; only the batch path is.

### RTF-5 — producer datum re-level across handover  🔴 (Fable-first design)
**Why red.** The design is genuinely unmade: the card's "per-signal bias constants" is
**estimability-false as stated** — the bias gauge pins at the pivot *receiver*, not the pivot
*satellite*, so a satellite-clock handover leaves the bias block invariant in exact arithmetic.
Opus would produce a plausible transform Fable must re-derive. The interim tier (RTA-1 sticky
pivot + RTA-2 scoped handover) already ships. **Path:** 1 Fable design session to pin the
transform algebra + the RTF-5/RTF-6 boundary + the value/state/carry coherence decision (an
RTF-5 design doc with the estimability proof); *then*, if scoped to clock-common-mode only, a
bounded 1-session Opus card (mirrors Card-6's helper+shift) + an armed validation session.

### RTF-7 — network AR for real-time  🔴 (defer past Card 8)
**Why red.** The batch producer-AR substrate it must extend is **~80% unbuilt** (only Phase-0
`IlsCore` + Phase-1 conditional-cov exist; no DD formation, cross-station cov, re-pin/re-solve,
or fixing code), AND the RT deltas (sticky-leveling thresholds, anti-churn disc policy, wrong-fix
tripwire) are undecided research the plan says must be tuned against **frac_cyc evidence from a
running service that doesn't exist**. The two Opus-safe slices (cross-station cov;
compute+log NL-direct DD/ilsFix) deliver diagnostics, not the card. **Path:** defer until
Card 8's armed run yields the frac_cyc/convergence evidence; then Fable-first the RT policy.

### RTF-8 — atmosphere product  🔴 (Fable-first + data acquisition in parallel)
**Why red.** Needs a **dense regional cluster** (~30–50 km, ≥5–6 stations, one interior held-out)
that is **not on disk and not auto-fetchable** — a global-set validation is a false green. The
STEC datum-alignment (each station's slant iono carries a free per-(station,constellation) rx-DCB
level that must align with the emitted OSB S-basis) is deep estimability work. **Path:** 1 Fable
design session (freeze atmosphere semantics; rate **ZWD-first** — fully estimable — separately
from STEC) + **kick off dense-data acquisition in parallel** (external lead time), then a ~1.5–2
session Opus ZWD-first implementation once both land. Consumer seams already exist
(`applyGimIonoUpdate`, the RT ZWD pseudo-obs, the B9 `zwd_carry` harvest).

---

## 5. What only the armed run (Card 8) can settle

Every yellow card's residual claim funnels to the same place — an armed `brdc_rts` run
(`service_mode` on, no SP3/CLK paths, BRDC fixture, 10-min smoke → 1 h): Card 5's emit-site
equivalence + 3.0 m conditioning; Card 6's re-level continuity + zero-disc-bump; Card 7's live
divergence branch; RTF-6's gauge drift; RTF-9's realized-bias-respects-floor. **Sequence the
yellow implementations toward Card 8, then let it adjudicate them together.** Watch for the
config trap: brdc_rts hard-refuses if `getOrbitCount()>0`, so the armed config must have no
SP3/CLK paths *and* PathResolver must not auto-glob/download one for the day.
