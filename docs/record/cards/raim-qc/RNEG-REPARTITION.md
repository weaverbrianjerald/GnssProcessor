# RNEG: the r-negative re-partition of the prior-residual census

**STATUS**

- **id:** RNEG
- **topic:** raim-qc
- **status:** closed
- **summary:** Readout-only re-partition BUILT: r-negative rows leave the |t| ladders for an RNEG class read in metres, and the worst |t| on the `fldf3_pl2` posture falls from 2.48e8 to 1.26e5.
- **dates in the text:** 2026-09-09
- **knobs / posture:** none (readout only; 12 RNEG columns appended to `network_prior_residual.csv`)
- **hashes named in the text** (commit / exe / byte-cert): `be523008c181e487` (`products.txt` sha256, identical pre and post)
- **run dirs named in the text:** `runs/fldf3_pl2`, `runs/fldf3_on2`, `runs/fldf3_pl2_ctl_new`
- **source:** `docs/archive/SPEC_prior_residual_census.md` §17 (17.1 to 17.4)

**Outcome.** A `post_var > fold_var` row is the coupling-carry case: the harvested posterior is a conditional variance, so r floors at 1e-9 and |t| inflates. On `runs/fldf3_pl2` 927 of the 957 rows above |t| 1e4 were r-negative; on `runs/fldf3_on2` the run's worst |t| row, BSHM00ISR/G22 @271200, was the gap re-entry poison case, a real defect hidden in that population. The re-partition moves rows without changing products. The numerator alignment of §17.4 (the census ê omits the coupling regressor term) is owed as its own solve-adjacent card, which no OPEN_ITEMS row lists.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_prior_residual_census.md -->
## 17. The RNEG re-partition (2026-09-09)

> Numbering note: the card that ordered this work called it "§15"; §15 (BUILD RECORD B) and §16
> (pointer-drift ledger) were already taken, so it lands here. Cross-references from
> `docs/SPEC_slip_qc_hardening.md` §12 point at **§17**.

### 17.1 What a `post_var > fold_var` row is

`r` is the row's redundancy contribution. It is computed from the harvested posterior against the
folded prior (`include/filter/NetworkPriorResidual.hpp:747-751`), and when the posterior EXCEEDS the
prior it goes negative before the numerical floor:

```cpp
r_negative = r < 0.0;                        // coupling-carry conditional > prior
if (r < kRedundancyFloor) { r = kRedundancyFloor; r_clamped = true; }   // 1e-9, :1207
```

That is not a corrupted row. It is the documented **coupling-carry** case: the harvested posterior is
a CONDITIONAL variance (conditioned on the Card-1 coupling regressors), and a conditional variance
can legitimately exceed the prior it was folded against. What breaks is the STATISTIC — the pair
(prior, posterior) no longer describes ONE fold, so D{ê} is not that fold's dispersion, `r` slams
into `kRedundancyFloor = 1e-9`, and |t| inflates by √((p+v)/(εp)). The constraint-aware σ̂₀_global
rider has always dropped these rows (`kGlobalRnegDrop`, `NetworkPriorResidual.hpp:543`); the ladder
did not, so an artifact sat at the top of every readout.

### 17.2 The measured artifact (field, `pl` family, both leveling-vehicle arms)

Counted directly off the PRIORRES identity rows, `logs/network_events.csv`, with the r-negative test
applied numerically (`post_var > fold_var` parsed from the row's own printed fields, so the count
does not depend on the pre-fix text token):

| arm (12 h, DOY-129, 63 sta) | PRIORRES rows | rows \|t\| > 1e4 | of those, r-negative | worst \|t\| of the run | worst \|t\| EXCLUDING r-negative |
|---|---|---|---|---|---|
| `runs/fldf3_pl2` (converging leveling posture) | 26 855 | **957**, all family `pl` / label `CLEAN` | **927** (96.9 %) | **2.4819e8** at `pl/CLEAN OAFA00ARG/G25/L2X @263460` | **5.925e4** (`JPLM00USA/G32 @276150`) |
| `runs/fldf3_on2` (+F3) | 23 193 | **346**, all `pl` | **306** (88.4 %) | **7.261e7** at `pl/CLEAN BSHM00ISR/G22 @271200` | **7.068e5** (`SGOC00LKA/G13 @294780`) |

Two readings, both load-bearing:

1. **The old headline was an instrument artifact, not a stiff prior.** Four to five orders of
   magnitude of the "worst |t| of the run" came from the floor, not from the data.
2. **On `fldf3_on2` the run's worst |t| is BSHM00ISR/G22 @271200 — the gap-re-entry poison case**
   (`docs/SPEC_slip_qc_hardening.md` §12.1). A real defect was sitting inside a population that the
   readout had already made unreadable.

**The re-partition's effect on the same posture, measured.** `runs/fldf3_pl2_ctl_new` is
`runs/fldf3_pl2`'s configuration re-run on the post-change exe; its `products.txt` is **byte-identical**
(sha256 `be523008c181e487…` both), i.e. the change is readout-only. Run-end lines:

| | `runs/fldf3_pl2` (pre) | `runs/fldf3_pl2_ctl_new` (post) |
|---|---|---|
| `worst \|t\| of the run` line | 2.4819e8 at `pl/CLEAN OAFA00ARG/G25/L2X @263460` | **1.2649e5** at `pl/CLEAN COCO00AUS/G06/L1C @276150` (RNEG excluded) |
| RNEG headline | (did not exist) | **32 177 row(s)**, worst \|ê\| **139.44 m** at `pl/CLEAN INEG00MEX/G08/L2X @299490` |
| `rneg=1` identity rows emitted | 0 (token did not exist) | **3 012** |

The identity stream reports only rows clearing the census's per-epoch hit criterion, so 3 012 emitted
identities against 32 177 census rows is a sample, not a discrepancy.

### 17.3 What was built

| piece | where |
|---|---|
| the class | `include/filter/NetworkPriorResidual.hpp:1155-1156` — `rneg_epoch` / `rneg_run`, one Ladder per label, alongside the existing arrays; the re-partition itself at `:781-784` (`const bool to_rneg = r_negative;`) |
| ordering rule | the class is kept by **\|ê\| in metres**, not \|t\| (`keepsMaxByE`, `:628-631`): inside the class the floor can invert the \|t\| ranking of a loud residual on a stiff prior and a quiet one on a loose prior |
| what it touches | **nothing** in the CLEAN/RESPONDED/FRAME bins, in `sum_log_t2`, or in the worst-\|t\| line; `n_r_negative` in the label ladders is therefore 0 by construction and is retained only so existing readers keep parsing (`:345-351`) |
| CSV | 12 columns **appended at the end** of `network_prior_residual.csv` so every reader keeps parsing by position — `rneg_n_rows, rneg_n_raw_formal, rneg_n_no_sigma0, rneg_max_abs_e_m, rneg_station, rneg_sat, rneg_signal, rneg_mech, rneg_max_abs_t, rneg_fold_var_m2, rneg_post_var_m2, rneg_gap_s` (`include/logging/NetworkRunTraces.hpp:136-152` doc, `:153-166` header; emitted `src/core/filter/NetworkPriorResidual.cpp:193-200`) |
| row survival | a (family, label) whose ONLY rows this epoch were r-negative still emits — the guard is `L.n_rows == 0 && N.n_rows == 0` (`NetworkPriorResidual.cpp:169`), so the re-partition MOVES rows instead of deleting them |
| event token | ` rneg=1`, a SEPARATE greppable token from the pre-existing conditional `r_clamped=1 r_negative=1` text (`NetworkPriorResidual.cpp:236-246`) — the pre-fix text could not mark an r-negative row that did not also clamp, which is why §17.2 counts numerically |
| run-end, per family/label | a per-(family, label) RNEG line: row count, worst \|ê\| with its full identity, the fold_var / post_var / gap that produced it, and the \|t\| it *would* have read — stated so the exclusion is auditable (`NetworkPriorResidual.cpp:375-400`); a label with 0 ordinary rows says how many r-negative rows it did have |
| run-end, global | the worst-\|t\| line now says "(RNEG rows EXCLUDED — see the next line)" and a second headline reports the RNEG class **in metres** (`NetworkPriorResidual.cpp:569-589`), deliberately adjacent: a reader who remembers the 2.5e8 needs to see where it went |
| unit tests (uncommitted with the change; not re-run here) | `tests/test_prior_residual_census.cpp` — `RNegativeRowsAreClassedApartAndMoveNoLadderStatistic`, `RNegClassIdentityFollowsTheLargestResidualNotTheLargestT`, `UnfoldedRowsWithLargePosteriorsStayInTheirLabelLadder` |

**Reading rule (binding).** Quote the RNEG class on `rneg_max_abs_e_m` (metres). Its `rneg_max_abs_t`
is retained for diagnosis only and means nothing as a test statistic. Any "worst |t| of the run"
quoted from a run older than 2026-09-09 on a leveling vehicle is the pre-partition number and must be
re-read against §17.2 before it is compared with anything.

### 17.4 OPEN — the numerator card (not part of this change)

The census's ê is formed as the raw carried-minus-solved difference at the harvest site:

```cpp
const double innov = oc->second.value_abs - value_abs;   // src/core/app/NetworkRunContext.cpp:3279
```

Under the Card-1 conditional coupling the carried prior's RHS is **not** `value_abs` alone: the
coupling rows contribute Σ M_rj·ŝ_prev,j to it —

```cpp
rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j   src/core/filter/NetworkCoupling.cpp:238
```

— so on a coupled posture this ê and the ê the fold actually saw differ by the regressor term. The
DENOMINATOR side of the same mismatch is what produces the r-negative population in the first place
(the harvested posterior is a conditional variance). **Aligning the numerator was deliberately left
out**: it moves every coupled family's ê and therefore its σ̂₀, i.e. it is solve-adjacent measurement
work with its own A/B, not a readout re-partition. Until it is done, an ê on a coupled family is an
UNCONDITIONED residual and the frame it is expressed in is not the fold's — read magnitudes, not
significance, and remember `project_network_xlin_invariance`'s rule that a screen reading a linearization
frame measures an arbitrary quantity.
