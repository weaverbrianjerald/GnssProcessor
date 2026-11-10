# Scope — Network OSB datum: M1 (min-2-GIM-valid guard) + connected-components inter-channel datum

**Date:** 2026-06-29
**Target:** `ProcessingMode::PPP_RTK_Network`, flag `network.estimate_sat_code_bias`
(`estimateNetworkSatCodeBias()`).
**Source items:** `docs/network_code_bias_review_2026-06-29.md` §2 **M1** (MEDIUM) — that file was
deleted in commit `a8f044f` and was never atticed; recoverable at `39b263a` — + the
"connected-component datum is future work" note in the datum block header of
`ApplicationNetwork.cpp`.
**Method:** orchestrator inline recon + a 7-agent scoping workflow (2 recon · 2 independent
designs · 3 adversarial verifiers, all `effort:high`) + a hand-verification of the one
load-bearing factual dispute (H1).

---

## 0. Bottom line

These are **two separable items that happen to live in the same code region**, and they have
**opposite cost/benefit profiles.** Ship them as two slices, not one:

| Slice | What | Verdict | Recommendation |
|---|---|---|---|
| **A — M1** | Network-wide ≥2-GIM-valid-sats-per-constellation guard before admitting a constellation's OSB columns | **Pure robustness, strictly safer, no downside** | **Ship now, ON whenever the flag is on** (not behind a sub-toggle) |
| **B — Connected-components datum** | Replace per-(const,SIGNAL) sum-to-zero with one sum-to-zero per co-tracking connected component | **Correct but marginal; trades fail-safe for fail-loud; benefit invisible to the validated consumer** | **Defer behind a default-OFF sub-toggle**; build only after an A/B closed-loop quantifies the gain against a *non-gauge-invariant* consumer |

Rationale in one line each: **M1** closes a real latent per-constellation zero-emit and only
ever removes a column *together with its gauge pin* (Weyl ⇒ gate stays clean), so there is no
regime where it is the wrong thing to do. **CC** recovers a ≤0.14 m inter-channel over-constraint
that sits *at or below* the product's own GIM-limited ~0.3–0.7 m DCB RMS and **cancels entirely
at the gauge-invariant closed-loop consumer**, while introducing a new whole-epoch-withholding
failure mode on a producer that has no numerical regression suite.

---

## 1. Background — the gauge structure being pinned (precise)

Both items turn on the same fact, so state it exactly. After `drop_cols`
(`ApplicationNetwork.cpp` ~649) removes every non-primary `sys_IFB<band>` receiver state when the
flag is on, the satellite OSB columns carry **all** code-level freedom on every band. The receiver
side keeps only one `sys_DCB` (the reference pair, GF-IF coupled) per `(rx, constellation)`. The
code-bias null space then has two orthogonal families:

* **(A) Per-satellite clock-common** — a constant added to *all* of sat *g*'s biases is absorbed by
  its estimable clock. Dim = #sats. **Pinned by the per-sat ionosphere-free pin** (`a1+a2=1`,
  `ApplicationNetwork.cpp` ~1322–1356). *Unchanged by both slices.*
* **(B) Per-(constellation, band) channel-level** — shifting a *set of channels* on a band by a
  common constant across all sats, compensated by every receiver's per-band DCB/IFB. **This is the
  family the datum sum-to-zero pins, and where the granularity question lives.**

The decisive sub-fact (confirmed against `StateInitializer.cpp::resolveReceiverBias` ~140–174): the
receiver per-band bias is **per-(rx, band), satellite-independent** — *one* `sys_DCB`/`sys_IFB<band>`
scalar shared across every satellite that receiver tracks on that band. Consequently, **within a
band the satellite inter-channel bias `x[g,C1C] − x[g,C1W]` is OBSERVABLE** whenever a common
receiver co-tracks both channels (the raw between-channel difference cancels iono + receiver-band
bias + geometry, no GIM needed). The GIM supplies the *between-satellite* and *inter-band* structure;
within-band inter-channel structure comes for free from raw differencing.

**Why the current per-(const,SIGNAL) sum-to-zero over-constrains:** it pins one row per *channel*
(`k` rows on a `k`-channel band), forcing each channel's per-constellation mean to 0. But the true
gauge dimension on that band is the **number of co-tracking connected components** `p ≤ k`. When
`p < k` the datum over-pins the `k − p` *observable* inter-channel means toward conventional zero.
Per the user's own `.BIA` cross-check (memory `project_network_code_bias_scope`): well-observed GPS
inter-channel matches sub-mm (G18 `C1W−C1C` 0.1714 vs `.BIA` 0.1736); the residual over-constraint
is **≤0.14 m and only on the ~6 GPS Block-III sats that broadcast L1C** (`C1L`/`C1X`); exactly 0 for
Galileo and GPS L2 (those AC means are conventionally zero anyway).

---

## 2. Slice A — M1: network-wide min-2-GIM-valid-sats-per-constellation guard

### The hole (confirmed live)
`appendGimIonoRows` (`ApplicationNetwork.cpp` ~701–759) builds the GIM constraint per
constellation as an `mc×mc` common-mode-free block `R = diag(var) + σ_c²·11ᵀ`. With **`mc==1`** (one
GIM-valid sat for a constellation) the block collapses to a `1×1` loose prior with **no between-sat
structure** — that constellation's DCB is *not* made observable. But registration
(`registered_code_keys`, ~970–998) gates on **≥2 *stations***, not ≥2 GIM-valid sats. So a
constellation with ≥2 stations but only 1 GIM-valid sat registers an OSB column that is observable
only through pure gauge → `assertNullSpaceRank` sees `near_zero > declared_dim` → **the whole epoch
is withheld** (zero products). Inert on IGS_Pasadena (GPS+Gal always have ≥2 GIM-valid sats); latent
on marginal Galileo / 2-station networks.

### Design
Count GIM-valid sats **network-wide per constellation** using the *same* validity predicate the row
builder uses, then gate constellation admission at the single registration funnel:

```cpp
// Built once per epoch, BEFORE the registered_code_keys keep_keys loop (~979):
std::map<char, std::set<std::string>> gim_valid_sats;            // const -> distinct sats
if (config_.estimateNetworkSatCodeBias() && gim_ && gim_->isLoaded()) {
    const auto& iw = config_.getIonosphereWeightingConfig();
    for (size_t i = 0; i < stations.size(); ++i) if (present[i])
        for (const auto& o : station_obs[i]) {                  // the per-station synced obs in scope
            const char c = o.satellite.empty() ? '\0' : o.satellite.front();
            if (c != 'G' && c != 'E') continue;                 // GPS/Gal only (GLONASS is clock-only)
            if (o.elevation < iw.elevation_cutoff_deg) continue;
            if (!gim_->slantIonoDelay(se.time, stations[i].ecef, o.sat_pos,
                                      Constants::GPS_L1_FREQ).valid) continue;
            gim_valid_sats[c].insert(o.satellite);
        }
}
// In the keep_keys insertion (~979): drop a constellation whose network-wide count < 2.
const char cst = NetworkSbasis::constellationOf(parts->satellite);
if (gim_valid_sats[cst].size() < 2) {
    if (warned_m1_.insert(cst).second)                          // member std::set<char>, one-shot
        log->warn("Network[W6-cb/M1]: constellation {} has {} GIM-valid sat(s) network-wide "
                  "(<2); excluding its satellite CODE bias this epoch (clock + phase bias still "
                  "emit).", cst, gim_valid_sats[cst].size());
    continue;                                                   // do NOT registerCodeBias; state stays local
}
```

### Why it is safe and composes (verified)
* **No dangling state.** Every code-bias consumer derives from the single `registered_code_keys`
  funnel (datum rows iterate it; `col_desc`, the D2 carry-forward stash, and the emit loop all key
  off columns that exist iff registered; `codeBiasColumn` throws on an unregistered key). Excluding a
  constellation = simply not inserting its keys. No datum row references an unregistered column; no
  orphaned column survives. Its sat-bias state falls to `pruneSatBiasStatesExcept` → local nuisance →
  *identical to flag-off for that constellation*.
* **Gate stays clean by construction.** `declared_dimension` counts only clock pins (independent of
  code bias). M1 removes a column **and its gauge pin together**, so `near_zero` and the soft-row
  span move in lockstep (Weyl). M1 *cannot* trip the gate.
* **Graceful degradation.** Clock + phase bias for those sats still emit; the sat clock carries the
  IF-code lump (standard IGS convention) and a consumer falls back to external `.BIA` for that
  constellation — exactly the intended behavior.
* **Retires review U1** in effect (the `mc==1`-vs-datum fight becomes moot — the target column is no
  longer datumed/emitted). Note the `mc==1` *row* still executes per-station in the parallel produce
  phase (that lambda has no view of M1's later network-wide decision); it folds a harmless loose
  prior into a station-local block on a column nobody emits. The path is *moved*, not deleted.

### Caveats / guardrails (from V2)
1. **Granularity is correct but is a rank-safety floor, not a tight observability boundary.** A
   1-GIM-sat constellation can still have *same-band* inter-channel biases that are observable
   without GIM; M1's blunt per-constellation exclusion drops those too. Safe (better to emit nothing
   than withhold the epoch), but document it. Per-constellation *is* the right granularity for the
   GIM-observed *inter-band* biases (the `mc×mc` block is joint across the constellation's sats — not
   per-sat, not per-band).
2. **≥2 is a rank floor, not an accuracy floor.** At exactly 2 GIM-valid sats the block yields one
   between-sat constraint — minimal rank, GIM-structure-limited, poorly conditioned. The product can
   still be marginal-quality at the boundary.
3. **Network-wide count must sum over the full `present[]` union** (implementation trap — a
   per-station count would reintroduce the wobble M1 prevents).
4. **Boundary flicker.** A constellation hovering at the 2-sat boundary (sats near the elevation
   cutoff) toggles its OSB columns in/out across epochs. Consider **N-epoch hysteresis** (drop after
   `<2` for N consecutive epochs; re-include after `≥2` for N) before excluding — this interacts with
   the code-bias discontinuity counter (§4), which would otherwise bump on every toggle.
5. **No consumer-side "OSB absent this constellation" marker** in the product. A consumer that assumes
   per-constellation completeness across epochs isn't warned (only the operator log is). Consistent
   with the existing emit contract; flag if a completeness-assuming consumer ever appears.

### Touch points
* `src/core/app/ApplicationNetwork.cpp` — hoist `gim_valid_sats` above the keep_keys loop (~979); add
  the constellation gate; add `warned_m1_` member.
* (No change to `NetworkSbasis`, `NetworkEstimator`, emit, or config — M1 is on whenever the flag is.)

### Validation
Add a 2-station fixture with exactly **1 GIM-valid Galileo sat**; assert (a) Galileo OSB excluded,
(b) Galileo clock + phase bias still registered/emitted, (c) GPS OSB still emits, (d)
`assertNullSpaceRank` passes (`near_zero == declared_dim`). This also fills the recon-flagged gap:
there is currently **no test that a datum/admission change stays gauge-rank-consistent.**

---

## 3. Slice B — connected-components inter-channel datum

### Algorithm (both designs converged here)
Per `(constellation, band)`, build a union-find over the band's tracking channels; **one sum-to-zero
row per connected component** (summing all registered `(sat,signal)` columns whose signal is in that
component). Run **independently per `(const, band)`** — receiver DCB/IFB is per-band; never merge L1
and L2 nodes. The per-sat IF pin, `addSharedConstraintRows`, the gate, and the emit loop are
**untouched**; only the count and support of the sum-to-zero rows change.

The witness is already in hand: a station's `getSatBiasStates()` holds `(sat|signal)` iff that station
tracked that channel (lazy per-station allocation), so no `.observed[]` rescan is needed. Build it
alongside the existing `cb_station_count` pass.

### Edge rule — **same-receiver, same-band, ANY satellite** (resolved against the two designs)
The two designs disagreed (D1: any-sat; D2: same-sat). **D1 is estimability-correct.** Worked from
the state structure: the level gauge is `x[·,C1C] += a`, `x[·,C1W] += b`, `rx_band1[r] += c_r`. A
receiver tracking **both** channels (on *any* sats) forces `c_r = −a` and `c_r = −b` ⇒ `a = b` ⇒ the
two channels share **one** gauge (one component). A receiver tracking only one imposes no link. D2's
"same-satellite" justification ("each satellite carries its own per-receiver per-band shift") is
**false** — there is no per-(rx, sat, band) state; the receiver bias is per-(rx, band),
satellite-independent. D2's same-sat rule would *over-split* the any-sat-but-not-same-sat case →
re-pin an observable differential level → the exact over-constraint CC is meant to remove. So:
**connect two channels iff some present station carries OSB states for both on the same band.** At
network scale this collapses each band to ~1 component unless its channels are tracked by *completely
disjoint* receiver sets (the Galileo `C5Q`/`C5X` case → 2 components, robust). Single-channel band →
1 node → 1 row → identical to per-signal.

Adopt **D2's quality floor** on top: a `min_edge_witnesses` knob (default 1; ≥2 hardens noisy
networks). An edge witnessed by a single receiver frees a mean estimated from one noisy difference;
treating a 1-witness edge as absent keeps the channels split → one extra **harmless over-pin** (Weyl,
gate stays clean) rather than a high-variance freed mean. This only ever *adds* rows, so it can never
trip the `under`-specified throw. (Matches `feedback_mw_thresholds_empirical`.)

### Estimability — "covers" not "exactly spans" (V1)
The `p` component rows span the exactly-`p`-dimensional per-(const,band) level null space and place
**zero** constraint on within-component (observable) inter-channel directions — so the means are
genuinely freed. **But the literal "exactly spans" is false:** family (A) (per-sat IF pins) and
family (B) (band-level rows) share a ≥1-dim-per-constellation overlap (the all-ones / clock-common
direction is in both spans). The combined pin set **covers** (≥ spans) the null space and
over-determines it by that benign, pure-gauge amount. This redundancy is **identical in the existing
per-signal datum** — CC neither introduces nor worsens it. Do **not** write code that tries to drop
the "redundant" row (which physical row is redundant is non-canonical). Keep the existing degenerate
single-band IF-pin branch (all-ones, ~1350) intact — do not assume CC subsumes it.

**Failure-mode asymmetry (the core safety argument):** an *under-connected* graph (fewer edges than
reality → more components → more rows) is a harmless over-pin → gate clean. An *over-connected* graph
(a fabricated edge merges two genuinely-disjoint channels → `p−1` rows for a `p`-dim null space) →
`under`-specified → **gate throws → whole epoch withheld**. The strict edge rule + `min_edge_witnesses`
bias toward the safe (over-pin) side; add a **build-time rank assertion** (cheap `ColPivHouseholderQR`
on the small per-band indicator block: assert row-rank == component count) to catch a witness-harvest
over-merge before it reaches the live gate. This matters because the producer has **no numerical
regression suite** — an edge-rule bug otherwise surfaces only as a silently withheld epoch.

### The CC-specific H1 gap (found during verification — not in either design)
**Status update on H1:** H1 (code-bias discontinuity bookkeeping) is being implemented in a
**separate, concurrent session — in flux, not yet a settled/committed landing.** As of this scope's
read the tree already had `NetworkProductEmitter.cpp` ~50–80, 102–137 (`code_disc_counter_`,
`prev_code_satset_`, `prev_band_tuple_`, `disc_per_signal`; consumed via
`SsrCorrectionStore::getCodeBiasDiscontinuity`), so the review's "H1 open / no bookkeeping" and recon
R1's "code bias emitted without stream-state memory" were **already superseded at read time**. Caveat
(`feedback_concurrent_build_locks`): the exact symbols/line ranges are a moving target — **re-grep
before relying on them**, and treat the interaction below as a coordination note for that session, not
a finished fact. Net: H1 is **not** a CC prerequisite to build.

**However** — the concurrent H1 detection bumps on (i) a forced datum change, (ii) a per-(const,signal)
**sat-set** change (`changed_channels`), and (iii) a per-sat **band-tuple** change (`sat_band_changed`).
For the *current per-signal* datum that is **complete**: each channel's gauge is its network sat-set,
exactly what (ii) detects — so the H1 session's bookkeeping is sufficient for the shipping (per-signal)
datum. **For CC it is NOT complete.** A pure **component-partition** change — the one receiver tying
`C5Q`/`C5X` drops `C5X`, so the band splits 1→2 components and the freed `C5Q−C5X` mean re-references —
need not change any emitted sat-set or band-tuple, so it is **invisible to that detection** → an
unsignalled step on a consumer that applies OSB absolutely. CC therefore requires a **new re-reference
trigger**: the producer must `markDatumChange()` (or emit a per-channel bump) when the component
partition changes vs the previous epoch. **Coordination note for the H1 session:** their sat-set /
band-tuple detection is correct and sufficient for per-signal; the partition trigger is a CC-only
extension and belongs in the CC slice, not in their H1 work — flag it so it isn't assumed covered.
Cheap to detect (compare this epoch's partition to the last); **mandatory if CC is built.**

### Cost / benefit honesty (V3 — the reason to defer)
* **Benefit ≤0.14 m on ~6 GPS L1C Block-III sats, against a GIM-limited ~0.3–0.7 m DCB RMS** — at or
  below the product's own noise floor.
* **Invisible to the validated consumer.** The closed-loop user consumer re-references OSB against its
  own per-(rx,band) receiver DCB/IFB, so the freed inter-channel mean **cancels** — CC and per-signal
  score *identically* at the user. That identity is, in fact, the gauge-invariance *test*: run it, and
  if (as expected) it scores identically, CC buys **nothing** for the only validated consumption path.
* The population that would feel a 0.14 m inter-channel error is a consumer that applies **raw
  per-(sat,signal) OSB absolutely** *and* tracks GPS L1C multi-channel — not the validated path, and
  possibly nonexistent today.
* **Fail-safe → fail-loud trade.** Per-signal is a deliberate fail-safe over-pin (the block header
  chose per-signal for exactly this robustness). CC's correctness depends on a co-tracking-graph
  computation whose over-merge bug withholds the epoch. Trading a sub-noise-floor bias for a new
  zero-emit failure mode, on a no-regression-suite producer, is poorly priced **unless** a real
  non-gauge-invariant consumer is shown to be hurt.

### Flag decision
Sub-toggle `network.code_bias_datum: "per_signal" | "connected_components"`, **default `per_signal`**.
Keep the per-signal path as the in-tree A/B oracle. Expose `network.code_bias_datum_min_edge_witnesses`
(int, default 1). Promote CC to default only after the closed-loop A/B parity run **and** the
partition-change discontinuity trigger lands. M1 is **not** gated — on whenever the flag is.

### Touch points
* `src/core/app/ApplicationNetwork.cpp` — witness harvest at the `cb_station_count` pass (~972–997);
  replace the `st0_row` construction (~1306–1320) with the per-(const,band) union-find under the
  sub-toggle; add the partition-change `markDatumChange()` trigger; the build-time rank assertion.
* `src/core/config/GnssConfigLoad.cpp` — parse `code_bias_datum` + `min_edge_witnesses` in the W6
  block (~2628); add a `CodeBiasDatum` enum + getter to `GnssConfig` (doxygen per convention).
* `src/core/filter/NetworkSbasis.cpp::assertNullSpaceRank` — **no change** (proof: `near_zero ==
  declared_dim` preserved).
* `src/core/products/NetworkProductEmitter.cpp` — only if the partition-change trigger is implemented
  emitter-side instead of via `markDatumChange()`.

### Validation
(a) fully-co-tracked GPS L1 fixture → CC folds **1** row where per-signal folds 3, gate passes both;
(b) disjoint `C5Q`/`C5X` fixture → CC folds 2 rows (degenerate == per-signal); (c) closed-loop A/B
asserts user-side score **parity** per_signal vs CC (the missing gauge-invariance test); (d) the
emitted OSB `C1W−C1C` for a well-observed GPS sat is unchanged (gauge-invariant) while the L1C
Block-III inter-channel mean moves off conventional zero by the measured ≤0.14 m.

---

## 4. Sequencing & decision points

1. **M1 now**, standalone, on-whenever-the-flag-is-on. Cheap, strictly safer, hardens the existing
   per-signal datum, closes a real latent zero-emit. No open questions.
2. **CC: gather the benefit number first, then decide.** Land the default-OFF sub-toggle + the A/B
   closed-loop parity harness; quantify the ≤0.14 m against the product's GIM-limited RMS at a *real*
   consumer. If the only consumer is gauge-invariant (it is, today), CC is a correctness nicety with
   ~zero measurable product gain and a new zero-emit risk — **shelve it** rather than ship it. Build
   CC fully only if a non-gauge-invariant multi-channel-L1C consumer is shown to exist and be hurt;
   if built, the **partition-change discontinuity trigger is mandatory** (§3).

## 5. Corrections to carried-forward notes
* **H1 is being implemented in a concurrent session (in flux), so the review's "H1 open" is already
  superseded — but it is not a settled landing.** As of this scope's read `NetworkProductEmitter` had
  the code-bias discontinuity bookkeeping (verified directly), so the `2026-06-29` review's "H1 HIGH /
  no bookkeeping" and recon's "code bias emitted without stream-state memory" were already out of date.
  Re-grep the symbols before relying on them (`feedback_concurrent_build_locks`); the CC partition
  trigger in §3 is a coordination note for that session, not a claim that their work is done. M1 and
  the H1 session barely collide (M1 = `ApplicationNetwork.cpp` registration; H1 = `NetworkProductEmitter.cpp`).
* The ≤0.14 m figure **is** sourced (the user's `.BIA` cross-check, memory
  `project_network_code_bias_scope`: G18 `C1W−C1C` 0.1714 vs `.BIA` 0.1736), contrary to one
  verifier's "unsourced" claim — but the substantive point (it is at/below the noise floor and
  invisible to the gauge-invariant consumer) stands.
