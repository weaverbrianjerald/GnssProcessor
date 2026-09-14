# LV — the ionosphere-level initiative's own reference page — knob ledger, verdict ledger, section map, grading protocol and the problem statement

**STATUS**

- **id:** LV
- **topic:** ionosphere
- **status:** reference
- **summary:** the ionosphere-level initiative's own reference page — knob ledger, verdict ledger, section map, grading protocol and the problem statement
- **dates in the text:** 2026-08-04 → 2026-08-21
- **knobs / posture:** `estimation.global_vtec_sh.mean_vtec_prior_tecu`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — (preamble), 0, 1, 2, 6, 7, 8

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
# SPEC — the ionosphere-level information budget (LV-1/LV-2/LV-3/LV-4/LV-5, S2H)

**Posture of record for settled work: the CANONICAL EXT-CB posture — single-layer deg-8 + a₀₀ GIM anchor 32.24
± 2 + `rx_code_bias_datum`, and NO LV level stack. Read §14 before arming anything.** LV-1…LV-5 built and
graded to 12 h on 2026-08-05; S2H built and graded to 3 h on 2026-08-07. Everything is loader-default OFF and
armed per-config. Absorbed 2026-08-15: `docs/HANDOFF_ext_cb_iono_level.md` → **§29** (campaign record) and
`docs/CONTRACT_level_authority.md` §1–§4/§6/§7 → **§30** (frame contract); both now under `docs/attic/`.
CONTRACT §5 (the unbuilt `lvlauth=` / `rxdcb=` S-line tokens) went to `docs/SPEC_product_cbpin_contract.md`
**§6.6**, which owns the S-line grammar.

⚠ **APPEND-ONLY NUMBERING.** 30+ code/test/script sites and **15** configs (re-counted 2026-08-21) cite this document by section (§3, §4,
§11, §11.3, §12, §13, §14, §17 item 0/1/3, §19.6, §20.2, §20.3, §25.2, §27 item 1, §28, and the range §9–§20)
and by card label (LV-1…LV-5, S2H). None of those citations fails loudly. Compressing a section's BODY is
safe; changing which card a number names is a silent break. The page was reordered into numeric order
2026-08-15 (position is never cited, only numbers are); no number changed meaning. §16 is deliberately
mis-numbered internally — its subsections are labelled 12.1–12.5 — and keeps that anomaly.

⚠ **Provenance.** Every `runs/<name>` path below (`ext63_dual_*`, `ext305_dual_*`, `replay_ext63_12h_*`,
`agu63*`, `s2h_*`, ~30 more) resolves under `D:\GnssProcessor_archive\2026-08-12\runs\`, not the working tree.
**These measurements cannot be re-derived here.**

⚠ **Three of this document's own claims were falsified by its own arms** and are corrected in place: §1's
seed-frame gradient (→ §10.1), §4's "the de-mean frees the common mode" (→ §13), and the 1 h grading that made
the stack look like a fix (→ §14). Do not quote §1, §3 or §4 without the correction that follows them.

## Knob ledger

All `network.*`, all **loader-default OFF** (defaults stay false only because flipping them invalidates other
lineages' stored-snapshot fingerprints — §0).

| card | knob | fingerprint when armed | note |
|---|---|---|---|
| LV-1 | `iono_seed_field_frame` (+`_min_sats`, default 5) | `lv1seed:<min_sats>`, conditional | diagnostic-keep (§13.3, §15.2) |
| LV-2 | `rx_cb_carry_demean` (+`_min_stations`, default 8) | `rxcbdemean:<min_sta>` nested inside the conditional `rxcb:1` | do NOT arm for settled work (§14) |
| LV-4 | `rx_cb_carry_step_gate` (+`_k_sigma`, default 10.0) | yes, conditional | correct fix; costs the data side (§23.3) |
| LV-5 | `rx_cb_carry_demean_within` | yes, conditional | correct; requires LV-2 (loader forces off without it) |
| S2H | `rx_cb_harvest_demean` (+`rx_cb_harvest_screen_z`, default 8.0) | `rxcbharv:` | forces LV-2/LV-5 OFF; validated to 3 h (§28) |
| LV-3 | `a00_prior_from_brdc` | — | **NOT built**, deliberately (§5 item 2, §30.6) |

## Verdict ledger

| card | verdict |
|---|---|
| LV-1 | **diagnostic-keep.**  Its stated mechanism is FALSIFIED (§10.1); its real function is to put the physically correct value into the frame LV-2 pins (§13.3).  Per-station offsets are a free rx-GF-bias diagnostic. |
| LV-2 | **do NOT arm for settled work.**  It does not free the common mode — it re-pins it to the FROZEN epoch-1 `mean_λ(x_lin)` (§13.2), which ramps the level +15…+21 TECU past hour 2 (§14). |
| LV-3 | documentation card — the frame contract is §30; its optional BRDC knob is deferred with its prerequisite verified (§30.6). |
| LV-4 | **correct and required to admit the carry at all** (74 % of withholds were frame artifacts), but it **costs the data side in every cell measured** (+64 / +35 / +16 % postfit phase rms, mask-dependent) and never pays for itself.  Not a promotion candidate (§23.3, §24.5). |
| LV-5 | **correct and required with LV-4** — LV-4 without LV-5 limit-cycles; LV-5 without LV-4 is nearly inert (§12.5). |
| S2H | **validated through 3 h with ZERO fold-side tax** (§28).  The 12 h tracking verdict is OPEN.  Not armed by default. |

## Section map (numeric order)

§0 rigor waiver · §1 the problem + phenomenology (see §10.1) · §2 design principle · §3 LV-1 · §4 LV-2 · §5
LV-3 · §6 build/run mechanics · §7 do-NOT list · §8 file-touch map · §9 implementation record · §10 first
outcomes + the mechanism correction · §11 LV-4 · §12 LV-5 · §13 **the correction: what LV-2 actually does** ·
§14 **the 12 h verdict, which reverses the 1 h one** · §15 the tracking-pin fix, FALSIFIED · §16 LV-5 review
addendum (internally mis-numbered 12.1–12.5) · §17 the roadmap · §18 the residual is NOT an extrapolation
artifact; the q axis is exhausted · §19 item 1 falsified at stage 1 (PARTLY SUPERSEDED by §23) + the audit
split · §20 arm 30 and the first 303-station arm · §21 the 61-sta mask 2×2 · §22 the mask trade FLIPS SIGN at
303 sta · §23 item 1 re-run on the current denominator · §24 item-1 stage 2 · §25 the excursion attribution
(+§25.7's retraction) · §26 the 305 @ 15/25 cell closes the mask study · §27 roadmap refresh / hand-back state
· §28 card S2H · **§29 the EXT-CB campaign record (absorbed)** · **§30 the frame contract (absorbed)**.

## Grading protocol (binding for anything touching this subject)

1. **Time-matched GIM**: `scripts/gim_global_mean_series.py <run_dir>` — never a single GIM number (§14.1).
   **AND** `scripts/field_vs_gim_footprint.py --config <the run's own network> --radius 5 --every 1` (§26.4 —
   a bare invocation silently uses a default 61-station config and reads only epoch 1).
2. **≥ 2 h windows** for any level claim at 61 stations (§14); the 305 network's transient is milder but 1 h
   remains a rank-only window.
3. **Datum-invariant quantities first** (invariant #5). a₀₀ is a DIAGNOSTIC, never a grade (§19.6). The
   grades are postfit phase rms, the consumer replay, DD fractions, churn counters.
4. `scripts/product_step_census.py` on any product-affecting change (§25.2).
5. Four-instrument data-side audit: postfit rms / prires / block-Baarda (≥ 3 h only) / congruence — **expect
   the congruence dissent** (§23.4, §24.3; unexplained).

---

## 0. Rigor level for THIS initiative (user directive, 2026-08-04)

**The user explicitly waived the byte-identity / golden-certification ceremony for this work: new results
sooner.** Five clauses still bind:

- Implement DIRECTLY on the shared path. No structural byte-identity proofs, no same-build golden A/B gates,
  no promotion-ledger rows.
- Config knobs are **A/B experiment switches**. Loader defaults stay `false` ONLY because flipping them
  invalidates OTHER lineages' stored-snapshot fingerprints (concurrent sessions resume warm chains) —
  fingerprint hygiene, not certification.
- **Invariant #1 is NOT waived**: every seed/carry change must behave correctly at slip, re-allocation and
  gap events — state what happens at each edge.
- **Invariant #4 is NOT waived**: run-end counters for every new mechanism. "0 reframed / 0 demeaned" must be
  visible, never silent.
- Predictions stated in each arm's `_comment` BEFORE the run. Unit tests only where the arithmetic can
  silently be wrong (LV-2's weighted mean, LV-1's fallback).

## 1. The problem, in one paragraph

The network's ionosphere LEVEL (SH a00, and with it the whole GF frame: level ⊕ receiver-DCB common mode ⊕
per-LOS common content) is held by an **accidental** information budget, not a designed one. The physical
separator (mapping-function spread) is weak; the designed authorities are feeble (a00 climatology anchor 0.25
info/ep at σ=2 TECU; rx-DCB zero-mean datum 0.01 on the common mode, N-independent); and what actually holds
the level up is the **rigidity of unrelated stiff carries** (the rx-CB carry at q=1e-9 m²/s).

⚠ **The rest of this section's model — per-LOS seeds born in a "bias-in frame" pulling the field with ~15 info
units (376 LOS × 0.04) — is FALSIFIED. See §10.1.** The phenomenology below is measured and stands; the
gradient decomposition does not. All arms DOY-129 / 61-station GPS EXT-CB:

| arm (runs\…) | posture | a00 behaviour |
|---|---|---|
| arm 2 (no rx datum) | ungauged | walk to **-80** in 20 ep, σ collapsing |
| arm 4 `ext63_dual_20ep_rxcb` | zero-mean rx datum | ignites **12.78** (GIM: 32.24), walks to -12 |
| arm 8 `ext63_dual_20ep_d15` (`_ext63_gps_dual_20ep_d15.json`) | + SH deg 15 | ignites **6.18** — representation FALSIFIED; walk to -13.9 unchanged |
| arm 9 `ext63_dual_20ep_a00gim` | + a00=32.24±2 | L1 holds 28→21, tension **escapes into unanchored L2** (-33) |
| arm 12/13 `…a00gim_sl` (canonical) | single-layer + anchor | ignition 24.7, trough ~7, recovery to ~21-22, **breathing ±6 TECU @ 20-30 min**, gates clean |
| arm 14 `…a00gim_sl_nc` | + carry_rx_code_bias OFF | **collapse -9 TECU/ep to −37..−39** — the carry is a load-bearing crutch |

Estimator-free ground truth (`scripts/extcb_raw_if_check.py`, `scripts/extcb_gim_verdict.py`): the raw chain
is clean (per-sat IF ±0.3 m, per-sat GF vs GIM ±2.3 TECU); the receiver C1C−C2W biases have **mean ≈ -5 ns
(-15 slant-TECU) and continent-clustered spread (std ≈ 21 TECU)**. The self-contained W6 control
(`runs\agu63`) arms the SAME rx-CB carry and the same seed path — buffered there by 32 free satellite
code-bias states into sub-band a00 wander, i.e. the structure is live in the production posture too.

## 2. Design principle (applies to all three cards)

**Separate estimable content from frame content.** Between-station differences, per-LOS structure, field shape
= estimable → carries and seeds may transport them. Common modes (level, rx-DCB mean) = frame → exactly ONE
designed authority per posture owns each, refreshed per epoch, with NO memory of a previous realization riding
in through a side channel. Per posture (full contract: §30.2):

- **EXT-CB ladder**: the a00 GIM anchor (`estimation.global_vtec_sh.mean_vtec_prior_tecu: 32.24`,
  `…_sigma_tecu: 2.0`) — validated (arms 12/13).
- **Self-contained (W6)**: the satellite code-bias sum-to-zero gauge — it realizes the same convention the
  IONEX community uses (zero-mean satellite DCBs), which is WHY the control's level lands GIM-compatible with
  no external data. No change needed.

## 6. Build / run mechanics for the implementing session

- **Build: use the wrapper, per CLAUDE.md §Build** — `scripts\vs2026_build.ps1 -Config x64-Release`. ⚠ The VS
  **2022** vcvars64 block this section used to carry is RETIRED by standing user directive (CLAUDE.md,
  2026-08-12); following it now violates the directive and can fail the configure (the PATH `cmake` is the
  wrong one; the wrapper pins the bundled one).
- **Release only for data runs; run a RENAMED exe copy** (e.g. `gnss_processor_opus.exe`) so concurrent
  rebuilds never hit a locked exe. Run with **cwd = the run directory** so `logs\` lands inside it; redirect
  console to `run_console.log`.
- New `.cpp` files need manual `CMakeLists.txt` entries (none expected here).
- Configs: new arms extend `config\test_suite\_ext63_gps_dual_1h.json` DIRECTLY — **the loader refuses an
  `extends` chain deeper than 8 and that file is already at 7, so new arms must extend IT, not its children.**
  (§23.7 records the 305-chain version of the same trap.) Copy the `_comment`-with-prediction house style.
- Read results from `runs\<arm>\logs\network_vtec_sh_health.csv` (`a00_tecu`, `a00_in_band`, `rms_deg1_tecu`,
  `kaula_ratio_*`) and the run-end counter lines in `run_console.log`. Cross-grade every level claim per the
  grading protocol above.
- **Staged cadence**: smokes ~4 min, 1 h arms ~10 min. Smoke first; 1 h only when the smoke matches its
  stated prediction.

## 7. Do NOT (falsified or blocked — do not rediscover)

- Do NOT turn `carry_rx_code_bias` off as "the fix" — arm 14: -9 TECU/ep collapse.
- Do NOT retry a post-reduce cross-station `SumZeroGauge` for the rx datum — structurally blocked against the
  Card-4 DD priors (`NetworkEstimator::solve` refuses; both fold against the same cached local factors). The
  pre-reduce diagonal channel is the sanctioned route. Detail: §29.2 item 3.
- Do NOT chase SH degree/representation for the level — falsified (arm 8), and deg-15 RINGS at 61 stations
  (pointwise −446 TECU vs GIM at ignition).
- Do NOT tighten the rx-DCB datum sigma to "hold" the frame — a stiff zero-mean imports the measured -5 ns
  convention offset (DEFECT 2 again, harder).
- Do NOT add external-data dependence to the self-contained posture (GIM stays diagnostics-only there; the
  GIM anchor is EXT-CB-ladder-only).
- The satellite side and the modeled chain are CLEAN (verified estimator-free) — any new level anomaly is not
  in orbits/clocks/OSB application; do not re-audit them.
- **§29.4 is the full ELIMINATED table (eight hypotheses, with how each died).** Read it before proposing any
  level hypothesis.

## 8. File-touch map

Line numbers drift; prefer the SYMBOL.

| symbol / site | card | change |
|---|---|---|
| `StateInitializer::computeIonosphereSeed` (`StateInitializer.cpp:263`) | LV-1 | seed reframe (median + field-at-IPP), fallback, counters |
| `ApplicationNetworkAssembly.cpp` rx-CB inject block | LV-2 | Σλ·innov / Σλ accumulation; demeaned η and gate |
| `networkHarvestLocalCarries`, `networkSnapshotFingerprint` (`NetworkRunContext.cpp`) | LV-2 | μ_c at harvest; `NetworkCarryState` field; conditional fingerprint append |
| `include/app/NetworkRunContext.hpp` | LV-1/2 | knobs mirror + carry-state field + counters |
| `src/core/config/GnssConfigLoadNetwork.cpp` | LV-1/2/3 | knob parsing + config-echo log lines |
| `include/config/GnssConfig.hpp` (members/getters) | LV-1/2/4/5/S2H | knob fields — **not** `GnssConfigTypes.hpp`; that is where every sibling network toggle lives |
| `config/test_suite/_ext63_gps_dual_20ep_*` | all | validation arms with predictions |
| `tests/test_level_information_budget.cpp` | LV-2 | weighted-mean arithmetic + μ-freeze floor |

Suite hygiene if adding tests: one suite name per file; prefer a new file over widening an existing suite.

---

