# SPEC — RTB-7(e)-RR: revisions to `REPORT_carry_blob_diagonal.md` (report-of-record hardening)

**Handoff**: written 2026-07-23 by the Fable session that reviewed
`docs/REPORT_carry_blob_diagonal.md`. Self-contained — do not assume access to
any prior conversation. Read `CLAUDE.md` and the report itself before starting;
`docs/REPORT_warm_resume_event.md` (RS card) is useful context for §5.4/§6 but
not required.

**This is a DOCUMENTATION-ONLY card.** The deliverable is a revised
`docs/REPORT_carry_blob_diagonal.md`. The review found the RTB-7(e) work itself
sound — every gate, every measured number, and the full-vs-diagonal decision
stand. What it found is that the report under-reads its own tables in one place
(the σ̂₀ column), over-compresses one headline, and has four mechanical nits.

Binding constraints:

- **NO code changes.** The tree is touched read-only. (Exception: a throwaway
  analysis script in the scratchpad for E3-N4 is fine; nothing under `src/`,
  `include/`, `tests/`, `scripts/`.)
- **NO producer runs of any kind** — not even smoke. Every question below is
  answerable from the tree, existing run artifacts, or the serializer source.
  If an artifact is missing, derive the answer from code; do not regenerate it.
- **No measured value in the report may change.** Additions are interpretation,
  accounting, and labeling only.
- Every new mechanism claim added to the report **MUST carry a file:line cite**
  (the report's existing register). Match its voice — the honest-corrections
  style of its §0.5 / §5.1 / §5.3 is the house standard here.
- Nothing is committed without the user's say-so (everything RTB-7 is
  uncommitted anyway).
- MUST items are binding; CANDIDATE wording/mechanics are free to deviate with
  a stated rationale. If a MUST seems wrong, stop and ask the user.

---

## 0. Evidence base (verified in tree, 2026-07-23 — re-verify lines before citing; concurrent sessions edit these files)

### 0.1 The D5 flag counter and σ̂₀ are not independent statistics

- The D5 gate threshold is `(k·σ̂₀)²`:
  `src/core/filter/NetworkInvariantCarryDriver.cpp:1172-1173`, with the comment
  at `:1168-1171` stating it outright — the gate is "a TRUE Nσ test:
  |v_i| > k·σ̂₀·√Cov(v)_ii". σ̂₀ comes from `innovInflationSigma0()`
  (`:392-394`), the M2 LS-VCE inflation accumulated through the previous epoch.
- The accumulator is fed by the standardized innovations of **carried** chords
  (`:1205-1208` split shape, `:1237-1240` joint shape).
- A D5 re-tap re-seeds the flagged chord's variance at `σ̂₀²·var`
  (`:1213`, `:1245`).
- Consequence: a *uniform* over-tightening of the carried prior raises σ̂₀,
  which widens the very threshold the flag counter is scored against — the
  tightening is normalized **out of the flag count** and **into σ̂₀** by
  construction. Only a *localized* anomaly (a few chords far off) must still
  flag before the single global scalar (one family: `kInflationFamily` /
  `kInflationSat`) can absorb it.
- Precision guard for the edit: v3 preserves per-chord blob **variances**
  bit-exact (report §2 unit gate), but `Cov(v)` inside the fold can still shift
  through the joint geometry when cross-covariances are dropped. The
  code-level, unambiguous fact is the threshold scaling; the full causal chain
  (diag → hotter carried innovations → higher σ̂₀ → wider gate → fewer flags)
  is a *reading*, directionally supported, single-realization. The report edit
  states the coupling as fact and the chain as reading — do not overclaim.

### 0.2 The σ̂₀ column moved adversely, at both scales, ordered with stress — and the report never reads it

From the report's own §3 / §5.2 counter tables (final σ̂₀):

| arm | 50 sta | 305 sta |
|---|---:|---:|
| inline | 1.76 | 1.35 |
| cold | 1.82 | 1.10 |
| full | 1.88 | 1.48 |
| diag | **1.91** | **1.56** |

- Diag is highest in both experiments; the diag-vs-full gap grows with the
  fraction of correlation discarded (+1.6% at 50 sta / 1,214 carried DD →
  +5.4% at 305 / 7,869). That is the predicted anti-conservatism (§1's own
  `1+ρ → 1` analysis) showing up — absorbed by the VCE as global inflation
  rather than surfacing in the flag counter (per §0.1 above).
- Corroboration that σ̂₀ measures internal consistency, not accuracy: at 305
  the **cold** arm has the *lowest* σ̂₀ (1.10) while sitting at the *highest*
  error level vs inline — fresh diffuse priors match fresh errors.
- None of this overturns the report: the external CLK/CB grading vs the inline
  reference is the arbiter and favors diag; a severe localized tightening
  would still spike flags. But §1's closing line — "§3's counters are where
  that risk would have shown up" — points at the σ̂₀-coupled counters and
  skips the one that responded.

### 0.3 The MEANS slogan is one rung beyond the ablation

Both warm arms restored bit-exact variances; the A/B isolates the
**cross-covariances only** (worth ≤3% at 50, ≤0 at 305). Whether the PD
variances are load-bearing was not tested. The missing rung would be a
means-only arm (restore `z`, seed the diagonal by the existing re-tap rule
`σ̂₀²·var`, `NetworkInvariantCarryDriver.cpp:1213`) — **OUT OF SCOPE for this
card and not worth running**: PD is 158,424 B = 28% of the 554 KB blob, so the
size motivation is exhausted; it becomes a mandatory prerequisite only if a
future card wants to *modify* restored variances. The report should carry that
triage in §6 instead of implying the rung was climbed.

### 0.4 Mechanical audit results (all verified)

- Ratios recompute: 477.75× / 89.66× / 56.94×; 50-sta non-blob residual
  47,622 B consistent between blob and snapshot rows; dense share at 305 =
  99.85%.
- Slopes are **least-squares fits over all epochs in the window** —
  `fit_slope`, `scripts/warm_resume_sd_metric.py:61-62` (default 20..60).
  Endpoint arithmetic on the report's 5-row table samples does NOT reproduce
  them (e.g. 50-sta full: endpoints give 33.96 vs the stated 30.18 mm/ep).
  A reader who recomputes will conclude the table is wrong; it isn't.
- §5.1 size table: each column leaves **7,165 B unaccounted** (DIAG rows sum
  559,817 vs total 566,982; FULL 270,871,387 vs 270,878,552). The residual
  being identical across columns says it is shared framing (row tags /
  headers / per-component rows), not a P-row artifact — but the table's last
  row claims "every other snapshot layer", so the columns should sum.
- Resume-epoch labeling is inconsistent between experiments: 50 sta grades the
  resume epoch itself ("15 (resume)"; 46 epochs = 15..60 inclusive; 450 s /
  30 s = 15), while §5 says "resume from +330 s", §5.2 says "resume at ep11"
  (330/30 = 11), yet the first graded row is "12 (resume)" (31 = 12..42).

---

## 1. Card E1 — read the σ̂₀ column (MUST)

Weave §0.1 + §0.2 into the report. MUST-level content (wording CANDIDATE):

- **(a)** §0 headline 3 ("Nothing degraded internally.") — qualify: the flag /
  re-birth counters improved, and the one counter that moved adversely is σ̂₀
  (+1.6% / +5.4%), read in §3/§5.2.
- **(b)** §1 trade paragraph, closing sentence ("…§3's counters are where that
  risk would have shown up.") — replace/extend: the flag counter is
  σ̂₀-coupled (threshold `(k·σ̂₀)²`,
  `NetworkInvariantCarryDriver.cpp:1172-1173`), so a uniform tightening is
  absorbed into σ̂₀ rather than the flag count; σ̂₀ is therefore where the
  anti-conservatism *did* show up, at the few-percent level, and the flag
  counter guards the localized case only.
- **(c)** §3 "Reading", "No hidden cost." bullet — add the 50-sta σ̂₀ reading
  (1.91 vs 1.88): the anti-conservatism did not vanish; it was absorbed by the
  VCE, magnitude ~1.6%, and the external grading is unaffected.
- **(d)** §5.2 counters paragraph ("all three carry-health tallies move the
  same way") — add: the fourth tally, σ̂₀, moves the other way (+5.4%),
  scale-ordered with the discarded-correlation fraction, and the three
  co-moving tallies all sit behind the σ̂₀-scaled gate. Include the cold-1.10
  consistency-not-accuracy observation here or in a footnote.
- **(e)** §5.4 — add the complementary, code-grounded mechanism alongside the
  existing staleness hypothesis: the VCE re-scales carried-covariance
  confidence every epoch (gate + re-tap seeding both ∝ σ̂₀²), so *scalar*
  miscalibration introduced by dropping correlations self-corrects within
  epochs; what cannot self-correct is direction-specific geometry — which the
  existing hypothesis argues was stale anyway. Note the diag advantage at 305
  is present by ep13, before the arms' σ̂₀ have diverged much, so the early
  window still favors the staleness story. Keep the section's
  "offered, not proven" register; both mechanisms are single-realization
  readings.

## 2. Card E2 — MEANS slogan precision + missing §6 row (MUST)

- **(a)** §0 headline 1: "the resumed branch lives in the MEANS" overstates
  per §0.3. CANDIDATE replacement: "the resumed branch lives in the means and
  variances — not in the correlations." Sweep the report for other instances
  of the bare "lives in the means" phrasing (§3's "The cross-covariance is
  not where the branch is." is already precise — keep it).
- **(b)** New §6 deferral row: means-only (v4) arm — **struck**, with the §0.3
  rationale (size motivation exhausted at 28% of 554 KB; decision unaffected;
  becomes mandatory only if a future card modifies restored variances — a
  family §6 already strikes). This closes the question the headline invites
  instead of leaving it unasked.

## 3. Card E3 — mechanical nits (MUST, each small)

- **N1** §0 headline 2: "It costs 98.2% of the snapshot." — the antecedent
  binds to the diagonal blob from headline 1; it means the dense Pᶻ.
  CANDIDATE: "The dense Pᶻ was 98.2% of the snapshot."
- **N2** Slope-method footnote at the first slope table (§3): slopes are LS
  fits over all epochs in the window (`scripts/warm_resume_sd_metric.py`,
  `fit_slope`); the tables show sampled epochs, so endpoint arithmetic will
  not reproduce the fitted values.
- **N3** Align the resume-epoch labeling between §3 and §5. Ground truth from
  existing artifacts only: the resume arms' run logs (`"WARM RESUME from"`
  lines; paths per `scripts/run_warm_resume_arm.sh` and the
  `config/test_suite/_dcb305_*.json` snapshot/log settings) establish whether
  the ep-11 boundary epoch is graded or grading starts at ep12. Then either
  fix the "12 (resume)" label or add one sentence stating the convention
  (e.g. "the restore epoch itself is ep11; grading starts at the first full
  post-restore epoch"). If the logs are gone, state the convention from the
  config arithmetic and label accordingly — do NOT re-run anything.
- **N4** Make §5.1's columns sum. Attribute the 7,165 B residual by parsing an
  existing snapshot file (paths in the `_dcb305_*` configs; a scratchpad
  line-class byte tally is the whole job) or, if the files are gone, by
  enumerating the writer's row classes in `serializeCarry`
  (`NetworkInvariantCarryDriver.cpp:~125-190`) and the snapshot section
  writers (`NetworkRunContext.cpp` write site near `:767`,
  `src/core/products/NetworkStateSnapshot.cpp`). Then add a labeled framing
  row to the table (or fold it into a renamed last row). The FULL and DIAG
  columns MUST each sum to their stated totals afterwards.

## 4. Acceptance gate

Re-read the revised report end-to-end and check, in order:

1. **No measured number changed** anywhere (diff the tables against the
   original if in doubt).
2. Every added mechanism claim carries a file:line (re-verified in the tree at
   edit time, not copied blind from this spec — the driver is under active
   concurrent edit).
3. §5.1 columns sum to their totals; the slope footnote is present; the epoch
   labels agree between §3 and §5 or the convention is stated.
4. The report still reads in one voice — the additions should be
   indistinguishable in register from §5.1/§5.3's existing corrections.

Out of scope, explicitly: any code or test change; any producer run; the
≥60-ep 305-window follow-up (already correctly deferred in report §6); running
a means-only v4 arm; committing anything.
