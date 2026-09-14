# RF-0b — a self-contained satellite-DCB datum under free slant iono, GPS dual (2026-09-15)

**STATUS:** closed — **built, default byte-identical, NOT the baseline**; it names the real datum question and hands it to RF-0c.  Worker branch
`worktree-agent-a0aafda782ef5e7bd` (`f9c6af4`), merged.  Knob `network.w6_iono_constraint: "auto" (default) | "none"` — under `"none"` the
external-GIM branch is neither forced nor required; hard errors with `estimate_iono_sh` or `code_bias_gf_datum` (one datum per deficiency).
Suite 2123 / 288 (0 failed; new `W6IonoConstraintTest` 11/11), smokes `auto` = `87a33bf0…` (knob absent and armed), `--verify` PASS, exe
`CF6F2E62…`.  The read: `runs/_rf0b/read.md`.

## The finding: under dual, each satellite carries a geometry-free direction that nothing self-contained was closing

Per satellite two gauges act on its code-bias vector — rd 1 (clock ↔ IF combination, realized by the per-satellite IF pin) and rd 3b (slant
iono ↔ geometry-free bias); they are independent (the IF pin is invariant under the GF shift).  With two bands the two exhaust the space —
the loader's refusal of `code_bias_gf_datum` under dual is a counting fact, not a policy.  The sum-to-zero rows (already armed
unconditionally on the RF-0 posture: 32 IF pins + 4 sum-to-zero channels against 97 OSB columns, `NetworkSolveGlue.cpp:253`) change by
`−μ_j·Σ_s β^s` under the shift and so close ONLY the constellation-wide level; **the GIM rows were closing the other n_sat − 1 = 31
between-satellite GF directions.**  The unit test proves it on real datum rows (31 surviving directions on the dual roster; the GF pin → 0),
and the rank probe measures it: `"none"` with `cb_kaula_prior` ON solves (gate observed 4 vs declared 1); with the prior OFF the gate sees
**39 near-zero directions on 31 satellites** with the exact `C2W : C1C = 1.647 = μ₂/μ₁` signature and the run emits nothing.  So on RF-0b
**the loose 3 m diagonal Kaula prior silently became the datum** for the between-satellite GF split.  The two extra SBC directions on the
RF-0 field run are PHASE-bias directions (`PB:G05:L2W` …), not code — the code datum closes its own level today.

## The arms (6 h cold, both beds, `"none"` + `ionosphere_weighting.enable: false`; no "GIM loaded" line; 720 / 720 emitted, 0 withheld; field
wall 220 s vs 286 — the GIM rows cost 87 ms / epoch of assembly)

| hour | FIELD consumer p50 RF-0 → RF-0b | FIELD wire D_phase / D_code / D_clk (×) · LEVEL | SIM consumer p50 RF-0 → RF-0b | SIM wire (×) · LEVEL vs truth |
|---|---|---|---|---|
| 1 | 3.02 → 1.87 | 2.2 / 4.5 / 0.32 · 0.70 → 0.52 | 3.31 → 6.20 | 1.04 / 1.10 / 2.5 · 0.78 → **2.02** |
| 2 | 3.00 → 1.78 | 5.4 / 9.3 / 0.89 · 0.62 → 0.54 | 4.29 → 2.37 | 1.64 / 2.8 / 0.62 · 0.88 → 2.07 |
| 3 | 1.55 → 1.00 | 6.0 / **17.1** / 1.00 · 0.63 → 0.49 | 3.57 → 0.98 | 2.06 / 2.2 / 0.95 · 0.79 → 2.06 |
| 4 | 2.14 → 1.28 | 10.8 / 10.9 / 0.64 · 0.68 → 0.49 | 3.27 → **0.39** | 2.26 / 2.1 / 0.93 · 0.81 → 2.05 |
| 5 | 0.98 → **2.96** | 9.8 / 2.9 / 0.73 · 0.71 → 0.50 | 1.05 → **0.23** | 2.37 / 2.2 / 1.58 · 0.81 → 1.55 |
| 6 | 3.29 → 2.56 | 13.1 / 2.1 / 1.09 · 0.74 → 0.51 | 0.57 → 0.56 | 4.33 / 6.5 / 0.42 · 0.84 → 1.51 |

The field's phase-bias tail moves to the sparse L2X channel and grows an order of magnitude (G31 L2X within-arc rms 243 m, G26 202, G23 202 —
the channel whose between-satellite GF split is now purely prior-determined); the M4 contract holds on both beds.  ⚠ The GIM rows were also
local station-block rows (~1 per station-satellite above 25°): without them `n_pf_rows` falls 2 417 → 561 at epoch 1 and the per-station STA /
ZWD aggregates cover 13 → 36 of 61 stations instead of 61 — **every per-station diagnostic under RF-0b is on a self-selected population and is
not comparable to RF-0** (`n_present` stays 61).

## Grade

Under the frozen reading RF-0b is NOT the baseline: the consumer moves 0.5–2.9 m per hour on both beds (mostly better on the field, hour 5
much worse; the sim 2.6–2.9 m better in hours 3–5 and 2.9 worse in hour 1), the wire degrades on both (field D_code to 17×, D_phase to 13×;
sim to 6.5× / 4.3×), and the sim's clock LEVEL against truth degrades 2.4× while the field's against COD improves 25 % — the GIM-as-truth
signature the card pre-registered, isolated on the one channel where the sim reference is absolute.  **The posture is now reachable and
self-contained; the GIM cannot leave the producer until the between-satellite geometry-free split has a real datum instead of a shrinkage
prior.**  The `"none"` option stays as the vehicle for that datum.

## RF-0c (pre-registered): a HARD per-satellite geometry-free datum — the S-basis choice, not a prior

The textbook resolution of rd 3b: the satellite GF code bias is not separately estimable from the slant iono under dual, so the S-basis
FIXES it and the estimable iono absorbs the true DCB — consistent for any consumer that floats its own slant iono per satellite (ours does).
Build, in the datum seam beside the IF pin rows (`buildCodeBiasDatumRows`): `network.sat_gf_datum: "none" (default, byte-identical) | "zero"
| "broadcast_tgd"` — one hard pin row per (satellite, L1/L2 signal pair) on the GF combination of its code-bias columns, at 0 (`zero`) or at
the navigation message's TGD / ISC (`broadcast_tgd` — the signal-in-space the producer already parses, not an external product); the row's
weight the pin idiom's; the declared S-basis dimension unchanged (constraint rows fold pre-gate, as the IF pins do); a counter and the
datum line naming it.  Interlocks: requires `w6_iono_constraint: "none"`; `"zero"` / `"broadcast_tgd"` with the GIM or SH armed is a hard error.
Gates as RF-0b (unit test on real datum rows: the GF pin leaves **0** surviving directions on the dual roster with `cb_kaula_prior` OFF; smokes;
`--verify`).  **Arms:** the two beds 6 h with `"zero"` and with `"broadcast_tgd"`, **`cb_kaula_prior` OFF** (the prior must not be a datum),
graded on the two instruments against RF-0 (the GIM baseline) on the field and against RF-0 on the sim with the LEVEL-vs-truth channel read
separately (the GIM-as-truth confound).  **Frozen reading:** the gate sees declared = observed with the prior off and both beds emit 720/720
⇒ the datum is real; the field consumer per-hour p50 within ± 0.5 m of RF-0 and the field wire within ± 20 % (D_phase, D_code) ⇒ RF-0c IS the
baseline and the GIM leaves the producer; `broadcast_tgd` beats `zero` on the field consumer by > 0.3 m in ≥ 4 hours ⇒ the physical datum is
the posture; the L2X tail persists at > 50 m rms ⇒ the sparse channel needs its own pin per signal and the read is the per-signal roster.
