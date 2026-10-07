# Card 1b: coupling-row joint whitening (premise FALSIFIED)

**STATUS**

- **id:** Card 1b
- **topic:** network-vehicle
- **status:** closed
- **summary:** PREMISE FALSIFIED: `M_w` is empty in 98.4 % of station-epochs and effectively diagonal otherwise, so joint whitening equals the scalar path byte for byte; the knob is RETIRED, default OFF.
- **dates in the text:** 2026-07-10
- **knobs / posture:** `network.clock_model`, `network.trace_section_timers`, `network.carry_coupling_time_update`, `network.carry_coupling_joint_whiten`
- **hashes named in the text** (commit / exe / byte-cert): none as commits; OFF byte-identity anchor SHA `69C20D38E22B5EBE`
- **run dirs named in the text:** `runs/t7_joint_lowfloor_12h`, `runs/t7c_jointwhiten_12h`
- **source:** `docs/archive/SPEC_card1_coupling_carry.md` §8, 8.1, 9, 10, 11, 12

**Outcome.** The card asked whether whitening a station's coupling rows jointly recovers the DD information that scalar marginalization was thought to destroy, the presumed cause of T7's flat DD σ. Its own premise gate failed: the marginalized term is 10 to 1000× smaller than `var_eff`, and the matched 200-epoch A/B is byte-identical over 10 614 rows with 0 LLT fallbacks. The addendum's 10× slowdown was a misread total (439.2 against 434.7 ms/epoch). The flat ~5.6 cyc σ is the current-epoch conditional ambiguity variance, an iono/ambiguity conditioning problem. The [ledger](../../CONFIG_promotion_ledger.md) records the knob RETIRED-negative on the tested lineage (`network.clock_model` off; the ON case is untested).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_card1_coupling_carry.md -->
## 8. Card 1b — coupling-row JOINT whitening: the mechanism and its pre-registered gate

*(Absorbed from `docs/HANDOFF_carry_rigor_card1b.md`, Fable-specced 2026-07-10 after grading `runs/t7_joint_lowfloor_12h`. Card 1 as built is CORRECT per its spec and its unit gate — the spec itself under-modelled one term. Do not refactor Card 1.)*

**T7 evidence baseline** (`runs/t7_joint_lowfloor_12h` — Cards 1+2, low floors, 12 h): **NO divergence (a00 ±35 TECU — Card 1's objective achieved)** and **σ-calibration ~1 (honest covariance)** — but **formal DD σ is FLAT at ~5–6 cyc for 12 h** (per-epoch code level; hour-by-hour medians 5.8 → 4.8). Card 1b's proposed mechanism: Card 1 marginalizes each row's WHITE shared columns into a huge SCALAR `dep_var`, deweighting every row to nothing — yet those terms are COMMON across a station's rows and cancel in any DD, so diagonal marginalization destroys the between-row correlation that carries the DD information. The carry goes inert while staying honest: the observed flat-σ + calib ≈ 1 signature.

**The fix as specced** (exact; mirrors `appendShIonoRows`). Per station, per epoch, build the coupling rows AS A GROUP:

1. Partition each row's shared terms: PRESENT stashed columns stay as `A_s` coefficients (unchanged from Card 1); **WHITE/DEPARTED columns** (no stash entry, or a white per-epoch state such as sat clocks when `network.clock_model` carries them outside D2 — those columns are NOT in the D2/dense stash and MUST be classified white, `P_w` = the clock's a-priori variance, not "present"; that classification is the entire point of the card) go into a marginalization matrix `M_w` (n_rows × n_white) with per-column prior variance `P_w,jj`.
2. Row noise becomes the JOINT covariance **`R = diag(var_eff_r) + M_w · diag(P_w) · M_wᵀ`** (n_rows × n_rows, SPD).
3. **LLT-whiten rows, `A_s`, `A_r` and RHS by the SAME factor** — copy the per-constellation whitening in `appendShIonoRows` verbatim (`llt.matrixL().solve`). The huge-variance common directions then act as common-mode-free: information ORTHOGONAL to them (the DD content) is preserved instead of destroyed.
4. **Group scope: all coupling rows of ONE station in one whitening block** (they only correlate within a station). Cost: one LLT of ~n_carried (≈30–40) per station — trivial.
5. **Numerical guard:** if the LLT fails (R not SPD from rounding), add `1e-12·max(diag)` to the diagonal once and retry; if it still fails, **fall back to Card-1 per-row scalars for that station THIS epoch and count it** (log a WARN with the count per run).

**The pre-registered premise gate.** Instrument the Card-1 row construction on the T7 config's first 10 epochs and log, for a sample of rows, `var_eff` vs the `dep_var`/white-term contribution. **CONFIRM the dep_var/white contribution dominates `var_eff` by ≥ 10× — that is this card's premise. If it does NOT dominate, STOP and report: the flat σ then has a different cause and this card is the wrong fix.**

Out of scope (as written): Card 2 code, refloat/RAIM gating (arm T2d separately only if the σ trend shows arc-lifetime limiting AFTER this fix), floors, datum work, kernel reduce/folds.

### 8.1 ⚠ SUPERSEDED — the 2026-07-10 late ADDENDUM ("ARMED BUT INERT + 10× SLOWDOWN")

**This addendum's perf claim is FALSIFIED by §9 below, 25 lines later in the original document. It is preserved because the misread is itself a recorded lesson: a reader who stops at the heading carries away a wrong perf number and a phantom hot loop.**

It reported, from `runs/t7c_jointwhiten_12h` (toggle armed) vs `runs/t7_joint_lowfloor_12h`: output BYTE-IDENTICAL through the first 3,926 trace rows (sha-confirmed) while running "~10× slower (**11 s/epoch vs 1.1**; D0-bracketed sections unchanged at ~0.5 s — the extra ~10 s is in UNBRACKETED new code)", and concluded the whitening computes and is then discarded, or `M_w` is empty so R degenerates to Card-1 diagonals while an unrelated hot loop burns the time. **The "11 s/epoch" was the TOTAL epoch-loop wall (11,328.8 ms over 26 epochs) misread as per-epoch (÷26 = 436 ms).** Its four-step debug order (run the skipped premise check; verify the whitened blocks are the ones appended; find and D0-bracket the hot loop; cheap 200-epoch A/B before any long run) was executed in full — see §9.

## 9. Card 1b — RESULT (2026-07-10, all four addendum steps executed): PREMISE FALSIFIED; toggle is a NO-OP; no hot loop

Instrumented the Card-1b row construction (census of `M_w` columns + `var_eff` vs the marginalized dep term, per station, first 40 epochs) and ran a clean, matched scalar-vs-joint A/B (`config/test_suite/_t7_ab_{scalar,joint}.json`, 200 epochs / 3.3 h, the ONLY differing key = `carry_coupling_joint_whiten`). Instrumentation was log-only (byte-identity preserved) and has been reverted; the exe was rebuilt clean.

**Step 1 — premise check (DECISIVE, falsifies the card).**

- **`M_w` is EMPTY in 723/735 station-epochs (98.4 %).** When non-empty (**12/735**) it holds **1–2 columns — and every one is kind CodeBias, NEVER a satellite clock** (T7 has no `clock_model`, so no sat-clock state ever departs; the addendum's clock-column reclassification has no referent in this config).
- **The marginalized term is 10–1000× SMALLER than `var_eff`** (`dep` ≈ 1e-3…0.16 m² vs `var_eff` ≈ 0.6…4.3 m², and ≈5e4–9e4 m² on new arcs). The card REQUIRED `dep ≥ 10× var_eff`; the measurement is the exact opposite. **Per the card's own gate (§8): STOP — the flat σ has a different cause and this card is the wrong fix.**

**Step 2 — whitened blocks: appended correctly, NOT discarded.** The append (4551–4557) and refloat re-whiten (6113–6117) write the SOLVED `L⁻¹` blocks (`Hr/Hs/z`) with unit weight; the gate test `JointWhitenCouplingMarginalizationEqualsBatch` proves the math when `M_w` is non-diagonal. The addendum's "compute-and-discard" hypothesis is wrong: it is compute-and-apply, but **the applied result EQUALS the scalar path because `M_w` is diagonal in ~all cases** (empty, or each departed column touches one row).

**Step 3 — the "~10 s/epoch hot loop" is a PHANTOM.** Measured wall: scalar **434.7 ms/epoch**, joint **439.2 ms/epoch** (200-epoch matched A/B); premise run **435.7 ms/epoch**; the finished 12 h T7 baseline **478.8 ms/epoch**. No 10×, no unbracketed hot loop. D0 breakdown normal: assembly 60 %, reduce 19 %, other 17 % (pre-existing QC/emit/tracers).

## 10. The accidental `trace_section_timers` byte-identity A/B (Card 1b step 4)

**Step 4 — falsifier: byte-identical, i.e. the toggle changes NOTHING.** scalar vs joint `network_dd_fractional.csv` are byte-identical across **ALL 10 614 rows** (not just 3 926); `network_solution.csv`, `network_products*.txt`, `network_vtec_sh.csv`, `closed_loop_trace.csv` all byte-identical; **0 LLT fallbacks**. The addendum's own A/B was uninformative anyway: `_t7_joint_lowfloor_12h.json` and `_t7c_jointwhiten_12h.json` differ ONLY by `trace_section_timers` — both have joint ON — so its "byte-identical" compared two identical-math runs. The true toggle A/B (this result) had never been run.

> ⚠ **STABLE HEADING — do not retitle §10.** `docs/CONFIG_promotion_ledger.md` and its generated twin `docs/CONFIG_promotion_ledger.json` cite this paragraph as the MEASURED byte-identity evidence for the `network.trace_section_timers` promotion row. They previously cited it by LINE RANGE (`HANDOFF_carry_rigor_card1b.md:129-131`), which breaks on any edit; re-point them at this section anchor instead.

## 11. What the flat σ actually is — and the verdict on `carry_coupling_joint_whiten`

`dd_sigma_cyc` = `√(gᵀ·jointConditionalCovariance·g)/λ` (ApplicationNetwork.cpp:7755–7769) = **the CURRENT-EPOCH conditional DD-ambiguity σ, which equals `var_eff`** (≈1–4 m² ≈ 5–11 L1 cyc). It is FLAT at ~5.6 cyc (p10 2.85 / med 5.63 / p90 9.51 over 3.3 h); |frac| exactly uniform (mean 0.251, P(<0.1) = 0.200). The carry is "inert" **not** because a huge dep term deweights it, but because the **pure conditional ambiguity variance is itself cycle-scale** — the ambiguity is not separable from the per-station slant iono / SH-VTEC border at this field quality.

**That is the iono–ambiguity conditioning problem** (memory `project_network_ddfrac_rootcause_shvtec`), **NOT a coupling-row marginalization defect**, and **neither Card 1's scalar nor Card 1b's joint marginalization can move it.**

**Recommendation (Card 1b): retire / dormant-ize `carry_coupling_joint_whiten`** and redirect effort at (a) whether `jointConditionalCovariance` can ever decline across epochs through this producer's per-epoch conditional readout, and (b) the iono/SH field health + floor regime — not at the coupling-row noise model.

## 12. Artifacts and configs

- **Card 1d / 1e:** knob `network.carry_coupling_time_update` (default OFF, requires `carry_conditional_coupling`); gates in `tests/test_network_estimator.cpp` (`ConditionalCarryTimeUpdateRecursiveEqualsBatch`, `ConditionalCarryContractionTrace`); NumPy oracles `card1c_replicate.py` / `card1c_falsify.py`.
- **Card 1b:** knob `network.carry_coupling_joint_whiten` (default OFF, RETIRE-recommended); gate `JointWhitenCouplingMarginalizationEqualsBatch`; configs `_t7_joint_lowfloor_12h.json`, `_t7c_jointwhiten_12h.json`, `_t7_ab_{scalar,joint}.json` (only differing key = `carry_coupling_joint_whiten`); run `runs/t7c_jointwhiten_12h`.
- **OFF byte-identity anchor:** `_t4_gg_treated_smoke.json` SHA == **`69C20D38E22B5EBE`** on a **FRESHLY REBUILT x64-Release** — verify exe mtime > source mtime; *"stale-exe validation has burned us three times."* Suite bar at the time: green ≥ 602/1.

