# SPEC — INC-C2 DD-carry covariance representation (`network.carry_invariant_factored`)

*The pre-coding algebra spec `docs/HANDOFF_carry_driver_perf.md` §2 mandates
("SPEC THIS PRECISELY BEFORE CODING … a FABLE review gate on the algebra before
implementation").  Written 2026-07-15 (Opus).  **Headline: the handoff's exact
`A + U S Uᵀ` factored carry does NOT exist as a bounded recursion — the shared
satellite border densifies the DD prior every epoch.  The win must come from an
approximation or a K-reduction, and which one needs the INC-C0 measurement.
This spec proves the obstruction and enumerates the viable levers.***

## 0. Notation (fixed — the handoff overloads "B")

Per connected (const,signal) component this epoch: `K` chords (carried DDs), `n_s`
shared satellite params, stations `r` (≤4 per chord's fundamental cycle).

| symbol | meaning | dim |
|---|---|---|
| `z = C_L x_L` | the K DDs as a signed cycle combination of local ambiguities | K |
| `C_{L,r}` | the block's signed sub-matrix on station r's columns | K×n_r |
| `Pz` | carried DD prior this epoch (last epoch's posterior, after any `q_z·Δt`) | K×K |
| `B = Σ_r C_{L,r} N_rr⁻¹ C_{L,r}ᵀ` | **data** conditional-on-border DD cov (block-sparse) | K×K |
| `G = Σ_r N_sr N_rr⁻¹ C_{L,r}ᵀ` | border propagation | n_s×K |
| `Σ ≡ Σ_ss` | datum-frame shared cov (`datumFrameSharedCov`) | n_s×n_s |
| `M = (Pz + B)⁻¹` | the fold middle inverse | K×K |

Dense reference path = today's `NetworkInvariantCarryDriver` + kernel
`foldReducedDdPriors` / `ddPriorPosterior{Mean,Covariance}` / `ddPriorInnovation`
= the ORACLE (kept compiled behind the toggle).  "Block-sparse" = the sparsity of
`B`: `B_{ij}≠0` iff chords i,j share a station.  In the 305-station giant
component (`K_c≈3,900`, cycles pervasively share stations) `B` is already near-dense.

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
  A = B      (block-sparse — the handoff's per-station N_rr⁻¹ blocks)
  U = Gᵀ,  S = Σ                             (rank-n_s border term)
```

**(2) is exactly the handoff's `A + U S Uᵀ`, and it is cheap: block-sparse `A`
plus a rank-`n_s` border term.**  `jointConditionalCovariance` builds exactly this.
The handoff's mistake is assuming (2) is the CARRIED form.  It is not — it is the
posterior of a SINGLE epoch's born chord.

## 2. THE OBSTRUCTION — the carried prior densifies every epoch (impossibility)

The carry accumulates: epoch t's prior `Pz` is epoch t−1's posterior (1).  Write the
posterior's two parts as they compound.

### 2.1 The block-sparse (conditional) part stays bounded

`A_post = (Pz⁻¹ + B⁻¹)⁻¹` — but expand it another way.  The DD is estimable from the
local ambiguities whose per-station data information `N_rr` is block-diagonal.  The
conditional-on-border DD information ACCUMULATES block-sparsely: after t epochs the
summed `Σ_e B_e⁻¹`-class term has the fixed sparsity of the network's DD-adjacency
graph (chords sharing a station), which is **bounded by topology**, not by t.  ✅

### 2.2 The border-coupling part accumulates UNBOUNDEDLY → dense

The satellite border `x_s` (clocks/biases) is **re-estimated fresh every epoch and is
NOT a carried constant** — only the DDs `z` are constant.  So each epoch's border term
`G_eᵀ Σ_e G_e` is w.r.t. a DIFFERENT, epoch-specific `x_s`, and to carry the DD alone
the border must be **marginalized** (integrated out) each epoch.  Marginalizing the
shared border couples EVERY DD of the component to every other (they all see the same
satellites), contributing one **rank-`n_s`** dense term per epoch:

```
Pz_post = (block-sparse conditional part, §2.1)  +  Σ_{e=1..t} (rank-n_s border term_e)
```

The border sum does not collapse (independent `Σ_e`, `G_e`), so its rank grows `t·n_s`
→ **either carried as an unbounded-rank sum, or collapsed into a dense K×K.**  Either
way the carried `Pz` is **dense K×K** for t beyond `K/n_s` epochs.  In the giant
component (`K/n_s ≈ 3900/534 ≈ 7` epochs) `Pz` is dense within seconds of run time.

### 2.3 Corollary — D-a, D-b, D-c all fail to give an exact bounded carry

- **D-a (carry dense `A_post`):** exact, but K×K — no win.
- **D-b (block-sparse `A` + border rank, re-tap on fill):** the border term is dense
  from §2.2, so forcing `A` block-sparse **drops real DD correlations every epoch** —
  an approximation, not exact (contradicts a 1e-8 gate; graded by field only).
- **D-c (info form, carry `J = Pz⁻¹`):** `J` is dense too — §2.2 in information
  coordinates is the same obstruction (the marginalized border is a dense rank-`n_s`
  information coupling per epoch; `J_post = J_prior + (B + GᵀΣG)⁻¹` and `(B+GᵀΣG)⁻¹`
  is dense because `GᵀΣG` couples all DDs).  Info form does NOT escape the border.

**Theorem (informal).** For any component whose DDs couple through a shared
per-epoch satellite border that must be marginalized to carry the DDs alone, the
accumulated DD prior covariance/information is irreducibly **dense O(K²)**.  No exact,
bounded, sub-K² factored or info-form recursion exists.  The dense K×K is the physics,
not a representation choice.

## 3. What this means for the perf card

The `≈34 s/ep` driver cost is dense O(K²)–O(K³) **because K_c is huge** (one giant
component), and that density is EXACT — it cannot be factored away losslessly.  The
levers are therefore either **exact-but-not-factored** (attack the constant, not the
order) or **approximate / K-reducing** (attack K or the correlations), and INC-C0
measures which pays:

| lever | class | exact? | what INC-C0 must show to pick it |
|---|---|---|---|
| **L1** dense linear algebra: parallelize the K×K fold/read-back (`parallel_carry` already exists), faster BLAS, avoid re-materializing `Pz` when only `M` is needed | exact | ✅ | which op dominates (`fold_minv` vs `post_cov` vs `basis_project` — the INC-C0 per-op timers) |
| **L2** component-size cap / partition the giant component; drop weak cross-partition DD correlations | approx | field | `max_component_kc` huge + geographically separable ⇒ the dropped correlations are weak |
| **L3** carry a spanning SUBSET of DDs; re-tap the rest fresh (bounds K directly) | approx | field | `max_z_dim` growth + `chords_marginalized` rate (is churn already bounding K?) |
| **L4** low-rank / block-sparse truncation of `Pz` (= D-b) with a re-tap escape | approx | field | correlations concentrate (spectrum of `Pz` decays fast) — needs a diag run |
| **L5** born-form exact read-back (2) — skip materializing K×K when a component is (near-)all-born this epoch | exact | ✅ | `chords_born` fraction per epoch (high born fraction ⇒ (2) applies often) |

**Only L1 and L5 are exact.**  L1 is the `parallel_carry` / D1 track (already partly
built).  L5 is a real, safe, bounded-scope win: when a component's chords are all born
this epoch (common after churn), its posterior is exactly (2) — assemble the carried
`Pz` as block-sparse `B` + `Gᵀ Σ G` and skip the dense `W Pz Wᵀ` / K×K read-back.
The next epoch it densifies again, but the per-epoch born-fraction (INC-C0 census)
says how often L5 fires.

### 3.1 L1 (exact parallel) — current state, and the residual serial wall

Auditing the wiring (`ApplicationNetwork.cpp` `CarryEigenScope`, `setCarryParallelThreads`,
kernel `foldReducedDdPriors` `station_threads`), **the exact GEMM parallelization of all
three dense-K carry stages is ALREADY built** under `network.parallel_carry`:

| stage | dense op | parallelized today | how |
|---|---|---|---|
| build (`basis_project`) | `W Pz_old Wᵀ` | ✅ Eigen-threaded | `CarryEigenScope` un-pins Eigen around `buildCarryDdPriors` |
| fold (`fold_gmg`) | `G M Gᵀ` | ✅ Eigen-threaded | `Eigen::setNbThreads(station_threads)` around the GEMM |
| fold (`fold_minv` SOLVE) | `M = (Pz+B)⁻¹` back-subst | ✅ column-parallel, **byte-identical** | Identity columns split across threads (each RHS independent) |
| install (`post_cov`) | `Pz M …`, `PzMGᵀ Σ …` | ✅ Eigen-threaded | `CarryEigenScope` around `installPosterior` |

Because Eigen threads each GEMM across all cores whether there is one giant component or
N, **across-component seam parallelism buys ~nothing** (two components' GEMMs run
sequentially each using all cores ≈ both at once each using half) — do NOT add it.

**The residual is the K×K Cholesky/LDLᵀ FACTORIZATION** of `Pz+B` inside `fold_minv`
(O(K³/3), serial: the column-parallel step above is the SOLVE against the factor, not the
factor).  Eigen does not multithread dense Cholesky, so this is the one dense op L1 cannot
speed up **exactly**.  The only levers are:
- a **ULP-relaxed blocked/parallel Cholesky** under `parallel_carry`'s existing byte-id
  relaxation license (the `reduce`-style opt-in) — new code, byte-id-relaxing;
- a **threaded-BLAS Eigen backend** (MKL) — a build-config change, no source code.

**"Avoid re-materializing Pz" (handoff §1) does NOT help** — the impossibility (§2): the
carried `Pz`/`A_post` is dense, so `W Pz_old Wᵀ` and the K×K posterior are intrinsically
O(K²·)–O(K³) whether carried as a matrix or "from the factor."

**⇒ L1 is essentially complete; the only remaining exact-ish win is the relaxed parallel
Cholesky, and ONLY if the INC-C0 `fold_minv` timer shows the serial FACTOR (not the
already-threaded GEMMs) dominates.**  This is precisely what the INC-C0 per-op split now
measures.

## 4. Proposed `network.carry_invariant_factored` (default-OFF)

Ship the **exact L5 born-form fast path** behind the toggle; hold L2–L4 (all
approximate) for the field-grade decision INC-C0 enables:

- **Store:** unchanged dense `Pz` for carried (densified) components; a factored
  `{B-blocks, Gᵀ, Σref}` variant for components that are ALL-born this epoch.
- **Build:** for an all-born component, emit the prior directly from (2) without
  `W Pz_old Wᵀ` (there is no old carry to project — every chord is fresh).  *(This is
  already partly what a born chord does — its diffuse prior; L5 makes the whole-block
  born case skip the dense assembly.)*
- **Read-back:** for an all-born component, install `Pz_post = B + Gᵀ Σ G` in factored
  form without materializing the K×K.
- **OFF ⇒** the dense oracle runs untouched (byte-identical to today).

### 4.1 Verification contract

- **Toggle** default-OFF; requires `network.carry_invariant`.  OFF byte-identical.
- **Unit gate** (`tests/test_network_invariant_carry_driver.cpp`): assert the L5
  factored born-form carried `z`/`Pz` matches the dense oracle to ≤1e-8 over an
  all-born lifecycle segment (reuse the DatumFreedom oracle).  This is EXACT (L5),
  so 1e-8 is the right bar.
- **Field gate** (if L2–L4 are later armed): dd-frac window grade + D5/PB/re-base
  counters ON-vs-OFF within noise on a 30 s 30 m run; wall-clock from the INC-C0
  per-op timers.  All GRADED verdicts at 30 s (`project_dev300s_tier`).

## 5. The decision INC-C0 unblocks (why C0 was correctly gated first)

The exact-bounded factored carry the handoff hoped for is impossible (§2).  The real
choice is L1 (exact, parallelize the dense cost) vs L2–L4 (approximate, reduce K /
correlations) vs L5 (exact, born-fast-path), and it turns on numbers only a run
produces, now instrumented:

- **`max_component_kc`, `max_z_dim`** — is there truly one giant K_c≈3900 component,
  and does K grow unbounded (L2/L3 needed) or is churn already bounding it?
- **INC-C0 per-op timers** (`basis_project_ms`, `post_cov_ms`, `fold_minv_ms`) — which
  dense op dominates (picks L1's target or confirms L5's payoff).
- **`max_cycle_len` / mean** — short cycles would have hinted at sparse structure;
  §2.2 shows the border defeats it regardless, but the metric confirms the diagnosis.
- **`chords_born` fraction** — how often L5's all-born fast path fires.
- **marginalization totals** — the §0 (a)-vs-(b) kill-path reconciliation.

**Recommendation:** run INC-C0 (the instrumentation shipped this session) on the
`_dev300s_gps_d8_em10_smoke` tier + one 30 s config, read the per-op ranking + census,
then choose L1 vs L2/L3/L4 vs L5 with data.  Ship L5 (exact) now behind the toggle;
gate the approximate levers on the field grade.

## 6. INC-C0 MEASURED (Jul-15, Fable) — the card closes here

Runs `run_c0_dev300s`, `run_c0_30m` (exit 0, 20 threads, `parallel_carry` ON):

- **INC-C1 → CLOSED.** Census = **case (a)**: the projection kill path fires; `K_c` is
  bounded; marginalization is non-zero.  No kill-path bug — the "K never marginalized"
  finding was giant *live* components, not retained dead chords.  The growth-bound warn
  stays as the 12 h robustness guard (silent in case (a)).
- **L1 → DONE / capped.** The parallel Cholesky ceiling is **1.06× at 30 s** (the serial
  factor is NOT the wall after all — the already-threaded GEMMs dominate).  **Skip it.**
  The optional LDLᵀ→LLT swap is a few-% kernel change, not worth it at sufficient perf.
- **Next measured card = P1 sweep fusion** (dd-frac + t1a + iono-diag share ONE recon
  sweep): ~14–21 % wall, byte-identical contract, spec'd in
  `HANDOFF_perlos_parallel_sweeps.md` §INC-P1.  Deferred (perf deemed sufficient).

### 6.1 Honest correction to §2 — the impossibility is CONDITIONAL, not absolute

§2.2 claimed the marginalized border couples every DD densely "because they all see the
same satellites."  That holds for a **regional** network (all-see-all + dense `Σ_ss`); it
is **not established for a global 305-station one**.  The border term is `Gᵀ Σ_ss G`:

- `G` sparse (short cycles / regional visibility) — Fable's `max_cycle_len` probe came
  back **short/sparse at both tiers**, confirming this.  But `G` sparse is *necessary,
  not sufficient*.
- The binding gate is **`Σ_ss` (satellite covariance) structure**, which the cycle-length
  probe never measured.  If `Σ_ss ≈ diagonal + fixed-low-rank datum common-mode`, the
  border term is sparse+low-rank and the accumulated `Pz` stays sparse+low-rank (the
  datum subspace is ~fixed across epochs ⇒ bounded rank, not growing) ⇒ **D-c viable**.
  If `Σ_ss` is dense (global co-visibility — most satellite pairs share some station),
  it densifies `Pz` and the impossibility stands.

**D-c door status: AJAR, not slammed.**  The exact-bounded info-form carry is possible
*iff* `Σ_ss` is diagonal+low-rank.  Global co-visibility makes dense `Σ_ss` the likely
case, so it is NOT reopened now (perf sufficient).  **If perf ever binds again, the
correct probe is `Σ_ss` sparsity/rank — NOT cycle length.**
