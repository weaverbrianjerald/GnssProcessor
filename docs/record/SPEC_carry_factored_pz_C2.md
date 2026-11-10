# SPEC — INC-C2 DD-carry covariance representation (`network.carry_invariant_factored`)

*Written 2026-07-15 (Opus), mandated by the perf card `HANDOFF_carry_driver_perf.md` §2 ("SPEC THIS PRECISELY BEFORE CODING … a FABLE review gate on the algebra before implementation"). That perf card was absorbed into §7 on 2026-08-15 and moved to `docs/attic/`.*

> **⚠ The filename names a proposal this document REFUTES.** `carry_invariant_factored` was never built and, for a dense `Σ_ss`, cannot be (§2). The name is kept deliberately: `src/core/filter/NetworkTeardown.cpp:288` and `include/filter/NetworkInvariantCarryDriver.hpp:255` both cite **`SPEC_carry_factored_pz_C2 D-c`** by label, and renaming would break them silently. Read the title as "the file where the C2 question was settled".

***Headline: the perf card's exact `A + U S Uᵀ` factored carry does NOT exist as a bounded recursion — the shared satellite border densifies the DD prior every epoch. The win must come from an approximation or a K-reduction, and which one needed the INC-C0 measurement. This spec proves the obstruction, enumerates the viable levers, and closes the card at §6 with C0's numbers.***

## 0. Notation (fixed — the handoff overloads "B")

Per connected (const,signal) component this epoch: `K` chords (carried DDs), `n_s` shared satellite params, stations `r` (≤4 per chord's fundamental cycle).

| symbol | meaning | dim |
|---|---|---|
| `z = C_L x_L` | the K DDs as a signed cycle combination of local ambiguities | K |
| `C_{L,r}` | the block's signed sub-matrix on station r's columns | K×n_r |
| `Pz` | carried DD prior this epoch (last epoch's posterior, after any `q_z·Δt`) | K×K |
| `B = Σ_r C_{L,r} N_rr⁻¹ C_{L,r}ᵀ` | **data** conditional-on-border DD cov (block-sparse) | K×K |
| `G = Σ_r N_sr N_rr⁻¹ C_{L,r}ᵀ` | border propagation | n_s×K |
| `Σ ≡ Σ_ss` | datum-frame shared cov (`datumFrameSharedCov`) | n_s×n_s |
| `M = (Pz + B)⁻¹` | the fold middle inverse | K×K |

Dense reference path = today's `NetworkInvariantCarryDriver` + kernel `foldReducedDdPriors` / `ddPriorPosterior{Mean,Covariance}` / `ddPriorInnovation` = the ORACLE (kept compiled behind the toggle). "Block-sparse" = the sparsity of `B`: `B_{ij}≠0` iff chords i,j share a station. In the 305-station giant component (`K_c≈3,900`, cycles pervasively share stations) `B` is already near-dense.

## 1. The exact WITHIN-EPOCH factorization (proven, real, but epoch-local)

`ddPriorPosteriorCovariance` (kernel `NetworkNormalEquations.cpp:947`) is exactly

```
Pz_post = A_post + U_post Σ U_postᵀ                                        (1)
  A_post = Pz − Pz M Pz = (Pz⁻¹ + B⁻¹)⁻¹      (conditional-given-x_s posterior)   K×K
  U_post = Pz M Gᵀ                             (border-coupling rows)              K×n_s
```

For a **born** (diffuse-prior) chord `Pz→∞`, `M→0`, `PzM→I`:

```
Pz_post^born = B + Gᵀ Σ G                                                  (2)
  A = B      (block-sparse — the perf card's per-station N_rr⁻¹ blocks)
  U = Gᵀ,  S = Σ                             (rank-n_s border term)
```

**(2) is exactly the perf card's `A + U S Uᵀ`, and it is cheap: block-sparse `A` plus a rank-`n_s` border term.** `jointConditionalCovariance` builds exactly this. The perf card's mistake is assuming (2) is the CARRIED form. It is not — it is the posterior of a SINGLE epoch's born chord.

## 2. THE OBSTRUCTION — the carried prior densifies every epoch (impossibility)

The carry accumulates: epoch t's prior `Pz` is epoch t−1's posterior (1).

### 2.1 The block-sparse (conditional) part stays bounded

`A_post = (Pz⁻¹ + B⁻¹)⁻¹`. The DD is estimable from the local ambiguities whose per-station data information `N_rr` is block-diagonal, so the conditional-on-border DD information ACCUMULATES block-sparsely: after t epochs the summed `Σ_e B_e⁻¹`-class term has the fixed sparsity of the network's DD-adjacency graph (chords sharing a station), **bounded by topology**, not by t. ✅

### 2.2 The border-coupling part accumulates UNBOUNDEDLY → dense

The satellite border `x_s` (clocks/biases) is **re-estimated fresh every epoch and is NOT a carried constant** — only the DDs `z` are constant. So each epoch's border term `G_eᵀ Σ_e G_e` is w.r.t. a DIFFERENT, epoch-specific `x_s`, and to carry the DD alone the border must be **marginalized** each epoch. Marginalizing the shared border couples EVERY DD of the component to every other (they all see the same satellites), contributing one **rank-`n_s`** dense term per epoch:

```
Pz_post = (block-sparse conditional part, §2.1)  +  Σ_{e=1..t} (rank-n_s border term_e)
```

The border sum does not collapse (independent `Σ_e`, `G_e`), so its rank grows `t·n_s` → **either carried as an unbounded-rank sum, or collapsed into a dense K×K.** Either way the carried `Pz` is **dense K×K** for t beyond `K/n_s` epochs. In the giant component (`K/n_s ≈ 3900/534 ≈ 7` epochs) `Pz` is dense within seconds of run time.

### 2.3 Corollary — D-a, D-b, D-c all fail to give an exact bounded carry

- **D-a (carry dense `A_post`):** exact, but K×K — no win.
- **D-b (block-sparse `A` + border rank, re-tap on fill):** the border term is dense from §2.2, so forcing `A` block-sparse **drops real DD correlations every epoch** — an approximation, not exact (contradicts a 1e-8 gate; graded by field only).
- **D-c (info form, carry `J = Pz⁻¹`):** `J` is dense too — §2.2 in information coordinates is the same obstruction (the marginalized border is a dense rank-`n_s` information coupling per epoch; `J_post = J_prior + (B + GᵀΣG)⁻¹` and `(B+GᵀΣG)⁻¹` is dense because `GᵀΣG` couples all DDs). Info form does NOT escape the border.

**Theorem (informal).** For any component whose DDs couple through a shared per-epoch satellite border that must be marginalized to carry the DDs alone, the accumulated DD prior covariance/information is irreducibly **dense O(K²)**. No exact, bounded, sub-K² factored or info-form recursion exists. The dense K×K is the physics, not a representation choice. *(Read §6.1 before quoting this: the impossibility is CONDITIONAL on a dense `Σ_ss`.)*

## 3. What this means for the perf card

The `≈34 s/ep` driver cost is dense O(K²)–O(K³) **because K_c is huge** (one giant component), and that density is EXACT — it cannot be factored away losslessly. The levers are therefore either **exact-but-not-factored** (attack the constant, not the order) or **approximate / K-reducing**, and INC-C0 measures which pays:

| lever | class | exact? | what INC-C0 must show to pick it |
|---|---|---|---|
| **L1** dense linear algebra: parallelize the K×K fold/read-back (`parallel_carry` already exists), faster BLAS, avoid re-materializing `Pz` when only `M` is needed | exact | ✅ | which op dominates — the INC-C0 per-op timers `basis_project_ms` / `fold_minv_ms` / `post_cov_ms` |
| **L2** component-size cap / partition the giant component; drop weak cross-partition DD correlations | approx | field | `max_component_kc` huge + geographically separable ⇒ the dropped correlations are weak |
| **L3** carry a spanning SUBSET of DDs; re-tap the rest fresh (bounds K directly) | approx | field | `max_z_dim` growth + `chords_marginalized` rate (is churn already bounding K?) |
| **L4** low-rank / block-sparse truncation of `Pz` (= D-b) with a re-tap escape | approx | field | correlations concentrate (spectrum of `Pz` decays fast) — needs a diag run |
| **L5** born-form exact read-back (2) — skip materializing K×K when a component is (near-)all-born this epoch | exact | ✅ | `chords_born` fraction per epoch (high born fraction ⇒ (2) applies often) |

**Only L1 and L5 are exact.** L1 is the `parallel_carry` / D1 track. L5: when a component's chords are all born this epoch (common after churn) its posterior is exactly (2), so assemble `Pz` as block-sparse `B` + `Gᵀ Σ G` and skip the dense `W Pz Wᵀ` / K×K read-back. The next epoch it densifies again; the per-epoch born-fraction (INC-C0 census) says how often L5 fires.

### 3.1 L1 (exact parallel) — current state, and the residual serial wall

Auditing the wiring (`ApplicationNetwork.cpp` `CarryEigenScope`, `setCarryParallelThreads`, kernel `foldReducedDdPriors` `station_threads`), **the exact GEMM parallelization of all three dense-K carry stages is ALREADY built** under `network.parallel_carry`:

| stage | dense op | timer | parallelized today | how |
|---|---|---|---|---|
| build (`basis_project`) | `W Pz_old Wᵀ` | `basis_project_ms` | ✅ Eigen-threaded | `CarryEigenScope` un-pins Eigen around `buildCarryDdPriors` |
| fold (`fold_gmg`) | `G M Gᵀ` | — | ✅ Eigen-threaded | `Eigen::setNbThreads(station_threads)` around the GEMM |
| fold (`fold_minv` SOLVE) | `M = (Pz+B)⁻¹` back-subst | `fold_minv_ms` | ✅ column-parallel, **byte-identical** | Identity columns split across threads (each RHS independent) |
| install (`post_cov`) | `Pz M …`, `PzMGᵀ Σ …` | `post_cov_ms` | ✅ Eigen-threaded | `CarryEigenScope` around `installPosterior` |

Because Eigen threads each GEMM across all cores whether there is one giant component or N, **across-component seam parallelism buys ~nothing** (two components' GEMMs run sequentially each using all cores ≈ both at once each using half) — do NOT add it.

**The residual is the K×K Cholesky/LDLᵀ FACTORIZATION** of `Pz+B` inside `fold_minv` (O(K³/3), serial: the column-parallel step above is the SOLVE against the factor, not the factor). Eigen does not multithread dense Cholesky, so this is the one dense op L1 cannot speed up **exactly**. The only levers are a **ULP-relaxed blocked/parallel Cholesky** under `parallel_carry`'s existing byte-id relaxation license (new code, byte-id-relaxing) or a **threaded-BLAS Eigen backend** (MKL — a build-config change, no source code).

**"Avoid re-materializing Pz" (perf card §1) does NOT help** — by §2 the carried `Pz`/`A_post` is dense, so `W Pz_old Wᵀ` and the K×K posterior are intrinsically O(K²·)–O(K³) whether carried as a matrix or "from the factor."

**⇒ L1 is essentially complete; the only remaining exact-ish win is the relaxed parallel Cholesky, and ONLY if the INC-C0 `fold_minv_ms` timer shows the serial FACTOR (not the already-threaded GEMMs) dominates.**

## 4. Proposed `network.carry_invariant_factored` (default-OFF) — NEVER BUILT (§8)

Ship the **exact L5 born-form fast path** behind the toggle; hold L2–L4 (all approximate) for the field-grade decision INC-C0 enables. Store: unchanged dense `Pz` for carried (densified) components, a factored `{B-blocks, Gᵀ, Σref}` variant for components that are ALL-born this epoch. Build: for an all-born component emit the prior directly from (2) without `W Pz_old Wᵀ` (no old carry to project — every chord is fresh). Read-back: install `Pz_post = B + Gᵀ Σ G` in factored form without materializing the K×K. **OFF ⇒ the dense oracle runs untouched (byte-identical).**

### 4.1 Verification contract

Per the standing rule in §7.5 (exact algebra with a different op order ⇒ byte-identity is not the standard; keep the dense path compiled as the ORACLE; ≤1e-8 class tolerances), specialized to L5: toggle default-OFF, requires `network.carry_invariant`, OFF byte-identical. **Unit gate** (`tests/test_network_invariant_carry_driver.cpp`): the L5 factored born-form carried `z`/`Pz` matches the dense oracle to ≤1e-8 over an all-born lifecycle segment (reuse the DatumFreedom oracle) — L5 is EXACT, so 1e-8 is the right bar. **Field gate** (if L2–L4 are later armed): dd-frac window grade + D5/PB/re-base counters ON-vs-OFF within noise on a 30 s 30 m run; wall-clock from the INC-C0 per-op timers. All GRADED verdicts at 30 s (`project_dev300s_tier`).

## 5. The decision INC-C0 unblocks (why C0 was correctly gated first)

The exact-bounded factored carry the perf card hoped for is impossible (§2), so the real choice is L1 vs L2–L4 vs L5 — decided by the §3 table's last column, from the INC-C0 census and per-op timers specified in §7.2. Ship L5 (exact) behind the toggle; gate the approximate levers on the field grade.

## 6. INC-C0 MEASURED (Jul-15, Fable) — the card closes here

Runs `run_c0_dev300s`, `run_c0_30m` (exit 0, 20 threads, `parallel_carry` ON):

- **INC-C1 → CLOSED.** Census = **case (a)**: the projection kill path fires; `K_c` is bounded; marginalization is non-zero. No kill-path bug — the "K never marginalized" finding was giant *live* components, not retained dead chords. The growth-bound warn stays as the 12 h robustness guard (silent in case (a)).
- **L1 → DONE / capped.** The parallel Cholesky ceiling is **1.06× at 30 s** (the serial factor is NOT the wall after all — the already-threaded GEMMs dominate). **Skip it.** The optional LDLᵀ→LLT swap is a few-% kernel change, not worth it at sufficient perf.
- **Next measured card = P1 sweep fusion** (dd-frac + t1a + iono-diag share ONE recon sweep): ~14–21 % wall, byte-identical contract, spec'd in `SPEC_dev_tier_perf_cards.md` §INC-P1. Deferred (perf deemed sufficient).

### 6.1 Honest correction to §2 — the impossibility is CONDITIONAL, not absolute

§2.2 claimed the marginalized border couples every DD densely "because they all see the same satellites." That holds for a **regional** network (all-see-all + dense `Σ_ss`); it is **not established for a global 305-station one**. The border term is `Gᵀ Σ_ss G`:

- `G` sparse (short cycles / regional visibility) — Fable's `max_cycle_len` probe came back **short/sparse at both tiers**, confirming this. But `G` sparse is *necessary, not sufficient*.
- The binding gate is **`Σ_ss` (satellite covariance) structure**, which the cycle-length probe never measured. If `Σ_ss ≈ diagonal + fixed-low-rank datum common-mode`, the border term is sparse+low-rank and the accumulated `Pz` stays sparse+low-rank (the datum subspace is ~fixed across epochs ⇒ bounded rank, not growing) ⇒ **D-c viable**. If `Σ_ss` is dense (global co-visibility — most satellite pairs share some station), it densifies `Pz` and the impossibility stands.

**D-c door status: AJAR, not slammed.** The exact-bounded info-form carry is possible *iff* `Σ_ss` is diagonal+low-rank. Global co-visibility makes dense `Σ_ss` the likely case, so it is NOT reopened now (perf sufficient). **If perf ever binds again, the correct probe is `Σ_ss` sparsity/rank — NOT cycle length.**

---

## 7. Origin — the perf card this spec was mandated by

*(Absorbed 2026-08-15 from `docs/HANDOFF_carry_driver_perf.md`, written 2026-07-14 (Fable), now `docs/attic/HANDOFF_carry_driver_perf.md`. Its `INC-C0`, `INC-C1`, `INC-C2` labels are preserved verbatim here.)*

### 7.1 The attribution that started the card (the P0 pivot)

The perf card superseded the per-LOS focus of `SPEC_dev_tier_perf_cards.md` after that card's own **INC-P0 sub-timers attributed ~79 % of "other" to the invariant-carry DD driver** (dense **K ≈ 7834** DD covariance) — **not** the per-LOS sweeps (**14.5 %**, cov-consumers, the separate D1 card). **P1/P2 of that handoff are PARKED** pending a post-pivot re-measure; its P0 instrumentation stays live.

### 7.2 INC-C0 — reconcile the finding with the design FIRST

The driver header claims `Pz` is dense per (const,signal) COMPONENT (`ZComponent::Pz`), chord death **= marginalization** (`ZComponent::killChord`, "the covariance sub-block"), and counter `chords_marginalized` exists. "K ≈ 7834 never marginalized" therefore means one of:

(a) components merged into a few network-spanning giants (305-sta graph: **K_c ≈ #arcs − #nodes + 1 ≈ 2,600–3,900 per signal — two GPS signals ≈ 7,800 live chords**, all genuinely alive for hours ⇒ marginalization is working and simply has nothing to kill), or
(b) the `killChord` path doesn't fire when it should (dead arcs retained), or (c) both.

**INC-C0 deliverable** (extend the P0 sub-timers + counters, log-only): per-component census each emit epoch (`K_c` live, `chords_marginalized`, `chords_tapped`, K growth over the run); per-OP timing split inside the driver — (i) basis projection / unimodular T (`basis_project_ms`), (ii) time update (+B), (iii) the fold `M=(Pz+B)⁻¹` + `GMGᵀ` (`fold_minv_ms`, `foldReducedDdPriors`), (iv) posterior read-back — mean + the K_c×K_c covariance install (`post_cov_ms`), (v) D5 gate work; run at BOTH tiers, `_net305_3card_dev300s_gps_d8_em10_smoke.json` (fast harness; D5 re-taps ~463/ep churn K there) and one 30 s 30 m config (the golden-relevant cost).

Everything downstream is chosen by C0's per-op ranking — the same rule that caught the wrong ranking applies to that list. *(Answer: case (a). See §6.)*

### 7.3 INC-C1 — exact quick wins (no representation change)

- **If (b):** fix the kill path; deletion of a departed chord's row/col in COVARIANCE form is exact marginalization; a returning arc is re-tapped fresh (C3c first-sighting semantics), so retention has no functional role. **If deleting a dead chord changes ANY output, that is a latent bug to surface, not a tolerance to accept.**
- **Growth bound:** whatever the cause, add a run-end counter line for max `K_c` and assert (warn) on monotone unbounded growth — also a long-run (12 h) robustness issue, not just wall-clock. *(Kept as the 12 h guard; silent under case (a).)*
- **Avoid dense materialization where an op doesn't need it** (e.g. if the posterior install writes a full K_c² block that the next epoch only reads through `(Pz+B)⁻¹`, consider computing the fold from the factor the kernel already holds — but only if C0 shows the install itself is a top op). *(Refuted by §3.1.)*

Verification: these are exact/no-op-class changes — demand byte-identity on emitted products + counters (same build, both tiers).

### 7.4 INC-C2 — the proposal AS ORIGINALLY WRITTEN (**FALSIFIED by §2**)

Reproduced so the refutation has its target on the page. The perf card argued: the solve covariance Σ is arrowhead, so the DD posterior covariance is EXACTLY `Pz = A + U S Uᵀ` with A block-diagonal (per-station conditional blocks, the `N_rr⁻¹` terms), U = K_c × n_s the border-propagation rows (`m_c = N_sr N_rr⁻¹ e_c` per chord), S = shared free covariance — an identity already implemented in `NetworkNormalEquations::jointConditionalCovariance` — so "dense K_c² is a representation choice." Restructure the z-store to CARRY the factored form:

- **Storage:** A (block-sparse by station), U (K_c × n_s), S (n_s × n_s, one per epoch, shared across components).
- **Time update:** `+B` (diagonal `q_z·dt`) → `A+B` keeps the form.
- **Fold:** `M = (Pz+B)⁻¹` by Woodbury on `(A+B) + U S Uᵀ`: **O(K_c·n_s² + n_s³) instead of O(K_c³). At K_c ≈ 3,900, n_s ≈ 534 that is a ~50–100× flop cut on the fold.** ← **NOW FALSIFIED**: §1 shows this is the single-epoch BORN posterior, not the carried form, and §2 shows the carried form is irreducibly dense.
- **Marginalization:** delete rows of U + entries/blocks of A — trivial.
- **The one messy part — unimodular T** (chord re-basis on tree-edge death): `A → T A Tᵀ` is no longer station-block-diagonal in general. Constraints that keep this tractable: T is component-local and touches few chords per event; allow bounded fill-in in A (block-sparse, not strictly block-diagonal), or re-tap the affected chords when fill-in exceeds a bound (a re-tap is the existing, S-legal reset primitive). This is the clause that mandated the present spec.

### 7.5 The verification-contract change (a reusable standing rule)

**C2 is exact algebra with a different float op order → byte-identity is NOT the standard.** State it, don't blur it. The standard is the Card-4 battery + field grade: unit — filter==batch equivalence, basis-invariance (gate 4), unimodular-T exactness, re-tap/marginalize lifecycle, all re-run against the factored store at **≤1e-8 class tolerances vs the dense reference implementation**, with **the dense path kept compiled behind the toggle as the ORACLE**; toggle default-OFF (`network.carry_invariant_factored`), **OFF = dense path byte-identical**; field — dd-frac window grade + D5/PB/re-base counters ON-vs-OFF within noise on a 30 s 30 m run, wall-clock table from the C0 timers.

### 7.6 Two explicit prohibitions

1. **Do NOT additionally approximate across stations.** Per-component independence is already the design (components are separate Pz blocks); **dropping the `U S Uᵀ` coupling is NOT licensed** — it would break the basis-invariance property the unit gates protect.
2. **Do NOT add across-component seam parallelism** (§3.1): Eigen already threads each GEMM across all cores, so the seam buys ~nothing.

### 7.7 Tier note and payoff envelope

**300 s tier:** D5 re-tap churn **~463/ep** there directly stresses chord lifecycle — the tier is the cheap perf harness, but **all GRADED verdicts are at 30 s** (tier rules, memory `project_dev300s_tier`). **Expected payoff envelope (sanity, not a promise):** driver **≈34 s/ep** of the **~43 s "other"** at the dev tier → **C1+C2 target ≤5–8 s/ep**, i.e. epoch **~52 → ~20 s at 305 stations**; a 3 h 30 s golden run **~5 h → ~2 h**.

### 7.8 Pointers (as given by the perf card, with dead links repaired)

`include/filter/NetworkInvariantCarryDriver.hpp` (`ZComponent::Pz` dense-per-component, `killChord`, `Counters`); `NetworkNormalEquations::{foldReducedDdPriors, ddPriorPosteriorMean, jointConditionalCovariance}` (the factored-form identity lives here); `docs/SPEC_card4_invariant_carry.md` plus **`tests/test_network_invariant_carry_driver.cpp`** — the unit-gate battery to re-run. *(The perf card said "phase-B/C handoffs"; no `HANDOFF_card4_phaseB`/`phaseC` exists in `docs/` or `docs/attic/`, so the concrete gate file is named instead.)* Memory: `project_m1_card4_phaseC` (fold algebra, M=(Pz+B)⁻¹), `project_m1_card4_d5_innovation` (re-tap semantics), `project_dev300s_tier` (tier rules + the P0 measurement chain that led here).

## 8. Status — what is and is NOT built

- **`network.carry_invariant_factored` does not exist.** Grepped over `src/`, `include/`, `tests/`, `config/`, `scripts/`: zero hits. L5 was never shipped; §4 and §4.1 remain a proposal.
- **`network.parallel_carry` IS the realized L1 win** (`GnssConfigTypes.hpp`, `parallel_carry{false}`), capped at the §6 ceiling. It is mutex with `parallel_reduce` — the loader forces `parallel_carry` OFF with a warning when both are armed.
- ✅ **All three in-code comments that asserted the falsified CYCLE-LENGTH gate are FIXED (2026-08-15, serial cleanup pass).** They said J is sparse iff cycles are short, which §6.1 shows is necessary-but-not-sufficient; the binding probe is `Σ_ss` sparsity/rank:
  1. `include/filter/NetworkInvariantCarryDriver.hpp` — the `max_cycle_len` doxygen. **FIXED**: now reads "G-SIDE SCREEN ONLY … NECESSARY BUT NOT SUFFICIENT … the BINDING probe is Σ_ss sparsity/rank, which this counter never measures", citing §6.1. The `SPEC_carry_factored_pz_C2 D-c` label is character-identical.
  2. `src/core/filter/NetworkTeardown.cpp` — the run-end "D-c viability" log line. **FIXED**: comment and the emitted string both now say "sparse G: NECESSARY, not sufficient — the binding probe is Σ_ss sparsity/rank"; the line is labelled "D-c viability screen (G-side only)" and its verdict token moved `SPARSE-J likely`/`DENSE-J risk` → `SPARSE-G likely`/`DENSE-G risk` (no consumer greps it; branch condition unchanged). The `D-c` label is character-identical.
  3. `include/config/GnssConfigTypes.hpp` — the `parallel_carry` doxygen. **Was ALREADY FIXED in the working tree before this pass** (found at `:1030-1033`): it now reads "was PROVEN IMPOSSIBLE for a dense Σ_ss (§2 there), and INC-C0's census closed as case (a) (§6) — so there is no pending O(K·n_s²+n_s³) cut behind this knob", and the `HANDOFF_carry_driver_perf.md` citation is gone. No edit was needed.
- The `D-c` label at sites 1 and 2 stayed character-identical through the fix; only the surrounding claim changed.
