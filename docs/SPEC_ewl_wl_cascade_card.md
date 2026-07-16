# SPEC — EWL-first tri-frequency cascade (M5 card "WL-2")

**Status:** DRAFT (Jul-15 2026, Fable; user-directed after the F6 attribution AND the
Opus `debug_r_scale` probe — both are inputs, see §1). Implementation target: Opus.
Verification: Fable. Default-OFF, byte-identical when off. The (B) loose-WL carry
refactor is HELD per the user's call; it remains the documented fallback (§3.3).

**One sentence:** fix the extra-wide lane from single-epoch-class MW(L2,L5) /
MW(E5b,E5a) statistics within ~3 epochs of arc birth (the third band deliberately
uncarried, so those rows win with no fight), bridge phase-only from the fixed EWL
to a fast wide-lane integer decision, and INJECT the WL integer into the Card-4
carry through a lane-targeted reseed primitive (set the cross-band difference,
preserve the NL) — so the stiff per-band carry holds the truth instead of fighting
it, and the NL cascade proceeds on the carry as designed.

**Supersedes / relates:** inverts `SPEC_mw_wl_card.md` §3.5 (Phase D "EWL cascade,
after B") to the FRONT of the ladder; implements the F6 fix (HANDOFF_mw_wl_card.md
§6d + the probe; memory `mw-wl-f6-fix-directions`) as an NL-preserving gated reseed
— option (C)'s trigger with the refloat replaced by a lane-targeted value edit;
(A) is unnecessary (probe) and (B) is HELD as the fallback; demotes WL-1's Phase-A
float pseudo-obs rows to a diagnostic (see §3.0); replaces WL-1's Phase-B
posterior-gated fix surface (never implemented) with statistic-gated fixing.

---

## 1. Motivation — F6 + the tri-frequency escape (do not relitigate)

- **F6 (proven, §6d dump):** the Card-4 carry time-links ∇ΔN₁ and ∇ΔN₂ in SEPARATE
  per-(const,signal) groups, each stiff (σ≈0.06–0.09 cyc by ep 11). Two independent
  per-band DDs pin WL = ∇ΔN₁−∇ΔN₂ at σ≈0.085 cyc — at the code-seeded (garbage)
  value, since no WL information ever entered (the Jul-15 structural gap). Any
  per-epoch WL information — the 0.22-cyc MW pseudo-obs, or an exact joint fold —
  is outvoted; the residual pressure trips D5 (369→1628) and shreds arcs.
- **The `debug_r_scale` probe (Opus, Jul-15 eve; scratchpad
  `network_mw_wl_f6_probe.csv`):** at 100× WL-row stiffness the healthy cycles SNAP
  to z_mw within 0.05 cyc at the FIRST arming epoch — so the §4 fold composition was
  never the blocker (no joint block needed) and birth-seed quality is irrelevant
  (a loose lane converges from garbage seeds the moment a strong measurement arms).
  But the stiff-row-vs-stiff-carry override CHURNS (D5 1628→1779): value transfer by
  row battle sprays carry innovations. Binding consequences: the WL entry mechanism
  must avoid the battle entirely (§3.3), and station-1 cycles — which failed to
  converge even at full authority — are a REAL independent bug (§7 pre-flight).
- **Why three frequencies dissolve the gap (tri-freq sats):** with two bands, each
  LOS gives 2 phase equations against {N₁, N₂, I} — underdetermined; the WL is
  code-limited and needs the cross-epoch accumulator the per-epoch producer lacks.
  With a third band and the EWL integer fixed, 3 phase equations meet
  {N₁, N₂ (N₃ tied), I}: the WL becomes determinable through PHASE at iono-model
  precision, per epoch. No code averaging needed at all.
- **The λ-protection asymmetry (the load-bearing subtlety):** the MW(L2,L5)
  OBSERVABLE is λ_EWL-protected — narrow-lane code noise divided by 5.861 m gives
  single-epoch DD σ ≈ 0.05–0.12 cyc, so its integer is trustworthy almost
  immediately. But CARRIED per-band values are never λ_EWL-protected: a lane formed
  from two stiffly-carried bands is ε_a/λ_a − ε_b/λ_b with per-band seed errors
  ε ~ 0.3–2 m (code noise + the −2I iono sign-flip) over λ ≈ 0.25 m ⇒ multi-cycle
  garbage held at σ≈0.085 — the F6 wall, for ANY lane whose bands are BOTH carried.
  Consequences: (a) the third band is NOT carried in v1 (§3.1) so the EWL lane has
  no stiff opposition and needs no special mechanism — its rows simply win; (b) the
  WL lane (L1, L2 both carried — NL continuity requires it) cannot be entered by
  rows at any stiffness (float loses, stiff churns — the probe) and gets the INJECT
  primitive (§3.3). The third frequency's contribution is TIME: trustworthy WL
  targets at ep ~4–6 via the bridge instead of ep ~25–50 via plain MW averaging.
- Wavelengths: GPS L2−L5 λ = c/51.15 MHz = **5.861 m**. Galileo E5b−E5a λ =
  c/30.69 MHz = **9.768 m** (every FOC sat, quad-freq). Galileo E5b−E5 / E5−E5a
  (AltBOC, 15.345 MHz) λ = **19.54 m** — a luxury rung; the fixture data DOES carry
  L8Q, but 9.768 m is already single-epoch-trivial. GPS L5 flies on the IIF/III
  subset (~⅔ of the fleet); dual-freq GPS sats take the §3.6 fallback.
- **MEASURED (Jul-15 eve, `scripts/ewl_census.py` — estimator-free star DDs, raw
  RINEX 2024-129, 309 sta, uniform channel groups):** GPS EWL (C2W/L2W×C5Q/L5Q,
  186 sta): per-epoch arc noise σ = 0.027 cyc; SINGLE-epoch DDs med|f| = 0.042,
  P(|f|<0.15) = 0.94, circR = 0.88; n=2 → 0.96/0.90; n=30 → med|f| 0.020, circR
  0.97. Galileo EWL (7,5): n=1 circR 0.97, P(<0.25) = 1.00. Galileo 19.5 m
  pairs: n=1 med|f| ≈ 0.012–0.014, P(<0.15) = 1.00. Reference lanes on the same
  data confirm the asymmetry: GPS WL (1,2) n=30 circR 0.55, Gal WL (1,5) 0.49 —
  code-limited, exactly the gap this card exists to bypass. The §3.2 defaults
  (n_lag ≥ 2, frac < 0.20, σ < 0.10) are comfortably inside these margins.
- **MEASURED, bridge arc (Jul-16, `ewl_bridge_oracle.py` — nearest-neighbor DDs
  bucketed by baseline, EWL fixed at n=2 then phase-bridged):** EWL gate skips
  5–8% at EVERY span (iono-free ⇒ span-independent ✓). The UNCORRECTED bridge
  beats code-MW only inside ~150 km (n=1 med|f| 0.124 vs 0.209 at <50 km) and
  plateaus iono-bias-limited (≈0.5 cyc/m × ~0.2 m DD iono) — never FFR-grade;
  ≥400 km it is uniform. Hence the §3.4 mandatory iono correction from the
  estimator's lagged per-LOS posterior. The closure guard tracks real failures
  (flag rate 2% local → 37% far). Caveat: 2024-129 is a solar-max stress set —
  quiet-iono margins will be wider; design to these numbers anyway.

## 2. Estimability

Identical invariance class to WL-1 §2: every quantity constrained is a closed-cycle
(DD) functional on the (station,sat) arc graph — receiver and satellite bias lumps
cancel by balanced signs; no pivot, no second S-system; gauge directions have zero
component along the rows ⇒ the W5 null space is untouched (FIREWALL inherited; §7
re-proves mechanically for the (2,5) rows). The INJECT is estimator-side but equally
legal: it edits carried z's — themselves estimable DD functionals — by setting one
invariant linear combination (the lane difference) to an integer while leaving the
complementary combination (the phase-determined NL/sum) untouched. That is a
measurement-update-like act on invariants inside the network's one S-system, not a
datum act. Frame discipline carries over unchanged: ZD-consistent `arcZdMean` per
pair, mixed-BIA-frame cycles skipped, per-cycle channel consistency (§3.1).

## 3. Mechanism

### 3.0 Two architectural principles (F6 lessons, binding)

1. **Decision source:** every integer decision (round, FFR, inject trigger) reads
   DETECTOR statistics — never the float posterior. The posterior's WL/EWL fracs are
   carry-contaminated (F6: deceptively tight σ passes the gate on garbage values).
2. **No rows against stiff-carried lanes:** folding pseudo-obs — at ANY stiffness —
   against a lane whose bands are both stiffly carried either loses (float, §6d) or
   churns (stiff, the probe). On such lanes statistics act ONLY through the inject
   primitive + a post-inject hold row (innovation ~zero by construction — nothing
   fights). On UNCARRIED lanes (the v1 EWL, §3.1) ordinary rows are fine and win
   cleanly. WL-1's float layer stays in the tree as a default-OFF diagnostic
   (`mw_wl.enable`), useful for A/B forensics; the loader warns if both it and
   `wl_inject_enable` are armed.

### 3.1 Third-band inputs (mostly already built)

- **Detector:** `buildUncombinedPairs` already forms ALL cross-band pairs,
  frequency-descending (f1 > f2, the signed-Δf convention), with same-channel code
  matching (`matchChannel`) — MwSlipDetector.cpp ~437–606. Under
  `mw_frequency_mode = Mirror` the band scope follows the KF; Triple/All widen it.
  The per-arc stats map is keyed `sat_sig1_sig2` and `arcZdMean` is pair-generic
  with the F1 lagged snapshot — so MW(2,5)/(7,5) statistics exist as soon as the
  bands are in scope. **No new MW math.**
- **Estimator:** the network solve must PROCESS the third band (the EWL row needs a
  live N₅ column): network configs move to FrequencyMode Triple for GPS (+Galileo
  where enabled). This grows the local blocks (~+⅓ ambiguity columns for GPS) —
  perf note §8. The known L5 receiver-ATX phase inflation is irrelevant to EWL
  integer content; keep L5 out of NL-bearing rows (unchanged from WL-1 SPEC §3.5).
- **The third band is NOT carried (v1, deliberate):** the (const, third-band-signal)
  groups are excluded from the invariant-carry driver (they still participate in the
  eq34 election/datum as any signal does — §11 Q1b). N₅ stays per-epoch loose, so
  the EWL lane never acquires an unearned stiff prior: EWL rows win cleanly against
  per-epoch code, no churn, no inject needed on that lane (§1 probe logic). Carrying
  the third band (for its own NL content) is a later card, gated on this one.
- **Pair selection per constellation (hardcoded v1 table, not user knobs):**
  GPS EWL = (band 2, band 5) on the estimator's chosen channels; Galileo EWL =
  (band 7, band 5); BDS/QZSS deferred; GLONASS excluded (FDMA, never in the carry).
  Per-cycle channel discipline as WL-1: every arc's statistic on the SAME channels
  the estimator uses at that station; mixed-channel cycles skipped + counted (the
  mw_census DCB-residue lesson, now on three bands).

### 3.2 EWL statistic rows → integer decision (ep ~3)

Ride the Card-4 chord basis exactly as WL-1: for each primary-sig1 fundamental
cycle whose arcs all expose the constellation's EWL pair, assemble
z_EWL = Σc·MW̄₂₅/λ_EWL (cycles), R from the lagged Welford variances (+ shared-arc
covariance within a component, the F2 form), correction-frame re-expressed per F5
against the (col₂, col₅) linearization values. Eligibility: n_lag ≥
`ewl_min_arc_epochs` (default 2 ⇒ first decision at ep 3 under the F1 lag).

Because the third band is uncarried (§3.1), these rows face only per-epoch code
information on the lane — they win without a fight: fold them as ordinary
pseudo-obs (float pre-fix, integer-hard post-fix). No inject, no D5 exemption on
this lane.

**§3.2 CORRECTION (measured, Jul-16 F-A attribution — the rows are NOT folded in
v1):** folding the fixed EWL rows into the solve proved to be pure cost: the WL
cascade consumes the integers from the DETECTOR statistic (never the posterior),
and 147 σ0.01 integer rows perturb the shared clock/PB border enough to elevate
D5 ~2× NETWORK-WIDE (679 vs control 342 at 15 ep; 24% of flags on
secondary-channel groups the EWL never touches — border-mediated, not local).
With the fold removed (`ewl_fold_rows=false`, the v1 default) D5 returns to
control (348–358) with the cascade fully intact. The EWL lane is therefore
DECISION-ONLY in v1: statistic → FFR → integer → bridge. Folding (this section's
original design, for N₅ conditioning / future L5-NL work) is retained behind the
`ewl_fold_rows` study knob and must come back, if ever, at honest softness or
inside the (B) lane-coordinate carry.

**FFR on the statistic row:** fix when σ_row < `ewl_fix_sigma_cyc` (0.10) AND
|frac(z_EWL)| < `ewl_fix_frac_cyc` (0.20). Rounding is stateless — re-derived each
epoch from the (stable, slip-reset) statistic, with a hysteresis margin (once
consistent, keep while |z_EWL − N_fix| < 0.35) and a `fix_flips` counter. A slipped
member arc resets its stats ⇒ the gate fails ⇒ the fix evaporates by construction —
the per-epoch re-derivation IS the un-fix wiring (force-fire test mandatory, §7).

### 3.3 The WL inject primitive (the F6 fix on the carried lane)

The WL lane is the one lane whose two bands are BOTH stiffly carried (NL continuity
requires it), so the probe's verdict applies with full force: a float row loses, a
stiff row transfers the value but churns (D5 1628→1779), and a raw refloat —
option (C) as re-tap — throws away the NL the carry spent epochs earning. The
remaining NL-preserving, battle-free entries are (B) — the lane-anisotropic carry
basis, HELD per the user's call, kept as the documented fallback below — and the
v1 choice:

`injectLaneTarget(cycle, lane, target_int, stat_var)`: given a fundamental cycle
and a band pair (a,b), adjust the carried DD values so the cycle's lane difference
∇ΔN_a/λ_a − ∇ΔN_b/λ_b equals the integer target while the complementary sum
(the phase-determined content) is PRESERVED exactly. Mechanics:

- The two bands live in SEPARATE carry groups. v1 applies the primitive only when
  the cycle maps 1:1 in both groups (same chord arc, identical cycle support under
  each group's tree — the common case when both bands' graphs coincide; the
  deterministic election then picks the same tree). Non-1:1 cycles are skipped +
  counted (`inject_skip_basis`); the general cross-basis solve (express the cycle
  in each group's chord basis via the driver's own reconcile algebra, minimal-norm
  adjustment over the touched chords) is the documented v2 extension.
- Solve per band pair in cycle units: with w = target (cycles) and
  s = current sum preserved: z_a′/λ_a − z_b′/λ_b = w, z_a′/λ_a + z_b′/λ_b = s.
- Covariance: reseed the lane direction's uncertainty to the statistic's honest
  variance; the two groups cannot represent the cross-band correlation (they are
  independent blocks), so the HOLD ROW (below) supplies the cross-group stiffness
  the carry cannot.
- **D5 exemption, one epoch:** the inject deliberately moves each band's carried
  value by up to ~half the garbage gap — per-epoch code-limited z_free will
  disagree at a few σ. The affected chords enter a `lane_injected` exemption set
  for that epoch (mirrors the `carry_slipped_ambs` funnel plumbing); D5 resumes
  normally next epoch. Without this the inject re-creates the storm it cures.
- **Trigger (stateless, self-limiting):** inject when the lane's FFR passes AND
  |statistic − carried lane value| > `inject_disagree_min_cyc` (0.75). Post-inject
  the disagreement is ~0, so it cannot re-fire; a later slip/re-tap regrows the
  disagreement and legitimately re-triggers — option (C)'s gated reseed, minus the
  NL destruction (the refloat is replaced by the lane-targeted value edit).

**Hold row:** after a successful inject, fold the lane integer as a hard row
(σ = `fix_weight_sigma_cyc`, 0.01, soft-hard — rollback-able) on the cycle's
(col_a, col_b) columns, re-derived each epoch while the FFR + hysteresis hold.
Its innovation is ~zero (the carry now agrees), so nothing fights; it pins the
lane between injects and across benign re-taps. Integer transport across basis
re-expression is automatic where v1 applies (1:1 chords; W unimodular).

**Fallback (documented, HELD):** if the A/B shows inject's discrete corrections
still churn or thrash on flips, the durable form is the probe-endorsed (B) — carry
each chord in (NL, WL) lane coordinates with anisotropic Pz (stiff NL, loose WL)
so the WL rows own the lane continuously with no fight. It is the statistically
purest fix and the probe's own recommendation, held only because it refactors the
carry's per-(const,signal) group structure (the cross-band coupling); this card's
statistics, FFR gates, telemetry, and validation harness all carry over to it
unchanged if the escalation is needed.

### 3.4 WL bridge statistic (tri-freq lane 2, ep ~4–6)

New per-arc accumulator in the detector, mirroring `MwArcStats` (Welford + F1 lag +
slip reset from the same funnel): the phase-only geometry-free bridge
GF_b = φ_WL − φ_EWL (meters; both raw phases, ambiguities constant along the arc).
Cycle assembly in the layer: Σc·GF_b + λ_EWL·∇ΔN_EWL^fixed =
λ_WL·∇ΔN_WL + (β_EWL−β_WL)·Σc·Ī₁ + ε_φ, with β_EWL−β_WL ≈ 0.435 ⇒ the iono residue
is ≈ 0.50 cyc per meter of cycle-DD iono (arc-window mean); amplified phase noise
≈ 0.08 cyc. So z_WL^bridge = [Σc·GF_b + λ_EWL·N_EWL]/λ_WL is a WL determination
whose error is IONO-limited, not code-limited: for adjacent-station cycles
(0.05–0.3 m DD iono) that is 0.03–0.15 cyc — FFR-grade within a few epochs, vs
n≈25–50 for plain MW(1,2).

- Requires the cycle's EWL integer (from §3.2/§3.3) — the bridge lane arms only on
  EWL-fixed cycles.
- **Iono handling is MANDATORY, not optional (measured, Jul-16 oracle —
  `scratchpad ewl_bridge_oracle.py`, stamped in §1):** on the 2024-129 fixture
  (solar max, 2 days before the G5 superstorm) the UNCORRECTED bridge carries a
  ~0.5·DD-iono cyc bias that plateaus at med|f| ≈ 0.07–0.12 even on <50 km
  nearest-neighbor cycles (P(<0.15) only 0.60–0.81; implied DD iono ~0.2 m), and
  is uniform garbage beyond ~400 km. Welford pricing alone cannot reach
  FFR-grade. Therefore: the cycle statistic is assembled IONO-CORRECTED —
  subtract (β_EWL−β_WL)·Σc·Î₁ using the estimator's LAGGED per-LOS iono
  posterior (the statistic stays decision-side; no row fights any carry), and R
  gains the iono posterior variance term. The Welford σ still prices residual
  scatter; the `bridge_max_sigma_cyc` guard stays; `bridge_span_max_km`
  optionally gates fixes by cycle station-span until the A/B measures the
  post-correction residual. Fallback if correction underperforms: WL injects
  source from plain MW(1,2) at maturity (§3.5) — the card degrades to classical
  UPD cadence, never to wrong fixes.
- WL decision + inject: FFR σ < `wl_fix_sigma_cyc` (0.10), |frac| <
  `wl_fix_frac_cyc` (0.15) on z_WL^bridge → `injectLaneTarget(cycle, (1,2), N_WL)`
  + hold row. Cross-check (integrity, free): plain MW(1,2) must agree within its
  own σ — disagreement counts + blocks the inject (`inject_skip_closure`).

### 3.5 Dual-freq fallback (GPS non-L5 sats)

Plain MW(1,2) statistic matures at σ ≈ 0.5–0.7/√n cyc ⇒ FFR-grade at n ≈ 25–50
epochs. Same FFR + inject + hold path, no bridge. Partial fixing propagates through
the cycle graph: tri-freq fixes constrain mixed neighbors' search space (no special
mechanism in v1 — the hold rows do it through the solve).

### 3.6 NL endgame (unchanged, the original design intent)

With WL injected+held, the carried per-band DDs are WL-consistent; NL is the
phase-determined direction the stiff carry already transports superbly. The netar
probe (ils_max_dim capped) reads the constrained posterior for NL fixability — no
new mechanism in this card; grading tells us when NL is reachable.

## 4. Config surface (all `network.mw_wl.*`, loadNetworkSection_)

| key | default | meaning |
|---|---|---|
| `ewl_enable` | false | Master for the EWL lane (stats scope, rows, statistic-gated fix; no inject — the lane is uncarried). Requires `carry_invariant`. OFF ⇒ byte-identical. |
| `ewl_min_arc_epochs` | 2 | Lagged Welford n before an EWL cycle is decision-eligible. |
| `ewl_fix_sigma_cyc` / `ewl_fix_frac_cyc` | 0.10 / 0.20 | FFR gate on the EWL statistic row. |
| `wl_inject_enable` | false | Master for the WL lane (bridge on EWL-fixed cycles; MW(1,2) fallback). Requires `carry_invariant`; bridge path additionally requires `ewl_enable`. |
| `wl_fix_sigma_cyc` / `wl_fix_frac_cyc` | 0.10 / 0.15 | FFR gate on the WL statistic (bridge or MW12). |
| `bridge_max_sigma_cyc` | 0.15 | Residual-honesty guard: no WL fix from a bridge statistic wider than this (post iono correction). |
| `bridge_span_max_km` | 0 | Optional cycle station-span gate on bridge-sourced fixes (0 = off); insurance until the A/B measures the post-correction residual. |
| `inject_disagree_min_cyc` | 0.75 | Inject trigger threshold on \|statistic − carried lane\|. |
| `fix_weight_sigma_cyc` | 0.01 | Hold-row σ (reused key; soft-hard, rollback-able). |

Deprecations: `fix_enable` / `fix_sigma_cyc` / `fix_frac_cyc` (WL-1 Phase-B
posterior-gated surface, never implemented) — loader warns REPLACED by the
statistic-gated keys above. `mw_wl.enable` (WL-1 float rows) becomes
diagnostic-only; warn if armed together with `wl_inject_enable`. Add the new
toggles to the summary log line + `isLoggerEnabled` per convention; extend the
detector-construction gate to `|| ewl_enable || wl_inject_enable`.

## 5. Lifecycle / slip response (Invariant #1 pre-flight)

- Rows and fixes are RE-DERIVED each epoch from live (lagged) detector statistics —
  a slipped/reset arc fails eligibility next epoch and its fixes/holds vanish; no
  stiff assertion survives its evidence. Force-fire test per lane (§7.3).
- The inject writes carried VALUES — but the carry's existing lifecycle already
  guards those: detector slips funnel via `carry_slipped_ambs` (chord re-tapped
  fresh), D5 covers fine slips, and a re-tap regrows the inject trigger (§3.3),
  which re-seeds — the (A)-at-re-tap role. The one NEW wiring is the one-epoch D5
  exemption on inject; it must be force-fire tested (inject → assert exemption
  consumed → assert D5 live again next epoch).
- EWL fix flip (statistic wandering across a rounding boundary): hysteresis + the
  `fix_flips` counter; a flip triggers a re-inject only through the normal
  disagreement gate.
- Coordinate with Card-4 unchanged: a chord refloat does NOT invalidate MW/bridge
  statistics; a DETECTOR slip invalidates both.

## 6. Telemetry (Invariant #4)

Run-end lines per lane: cycles eligible / EWL rows decided / fixed / flips /
unfixed (by reason) / injects (count, median |Δ| cyc) / holds active at end /
skips {not-primary, missing band-3 column, missing MW pair, young arc, mixed
frame, mixed channel, basis-not-1:1, closure-disagree, bridge-too-wide} / mean R
per lane. Per-build counters accumulate on the ACCEPTED install only (WL-1
precedent). Config echo gains the new keys. The `f6_dump` instrument stays and
gains lane columns — it re-fires for every fix's A/B.

## 7. Validation plan (Fable; QUICK-ONLY per user directive Jul-15)

All on the `_wlver_*` 15-ep 50-sta GPS-only harness (clone for 100/150 sta at
most) + unit gates. **NO long/graded runs until the user commands.**

1. **Byte-identity OFF** (same-build; all new toggles false) — including under the
   Triple-frequency config, so the band-scope change alone is proven inert when
   the card is off... it is NOT: FrequencyMode Triple changes the estimator. So
   two OFF baselines: (a) existing dual-freq config byte-identical vs pre-card
   exe; (b) Triple-freq config with card OFF as the NEW control for every ON A/B.
2. **Unit battery:** (2,5)/(7,5) row math + λ table; bridge assembly hand value
   (incl. the λ_EWL·N_EWL term); primitive exactness (lane hits target, sum
   preserved to 1e-12, values transport under a basis re-expression); FFR +
   hysteresis; D5-exemption consume/expire; closure-disagree block.
3. **Falsifier FIRST (force-fire):** inject a synthetic slip mid-window on a
   member arc — assert the EWL fix and WL hold evaporate next epoch, the re-tap
   re-triggers a fresh inject, and NO wrong integer is ever asserted (external
   truth: `scripts/mw_census.py` extended to (2,5) star DDs — cheap).
4. **15-ep A/B (the gate):** ON (ewl+inject) vs Triple-OFF control. PASS =
   (a) EWL fixes by ep ~3–4 on the L5 subset, WL injects by ep ~6–10, zero
   closure violations; (b) D5 flag rate at OFF level (the storm is the failure
   mode this card exists to avoid); (c) KF-WL circR on injected cycles →
   integer-clustered (≫ OFF's uniform), per the dd-frac trace; (d) station-1
   x_lin pathology quarantined or fixed first (below).
5. Optional same-window 100/150-sta scale point.

**Pre-flight fix (blocking, small):** station index 1's ~256 km ambiguity
linearization seeds (§6d secondary finding) make correction-frame lane rows
ill-conditioned for its cycles — and the probe UPGRADED this from cosmetic to real:
station-1 cycles failed to converge even at 100× row authority (off by 8–14 cyc).
**ROOT-CAUSED (Jul-16, background investigation):** stations[1] = JPLM00USA
(SEPT POLARX5, free-running clock ≈ 0.8557 ms at t=0; position ruled out, 0.45 m).
The COLD ambiguity seed (`StateManager::computeAmbiguitySeed`, StateManager.cpp
~1269) subtracts `clk_term = x(clock)` — which is 0 at the first network epoch
(no code-clock pre-solve exists on the network path), so the full c·dt_rcv lands
UNIFORMLY in every ambiguity seed. Cold-seed-only (mid-arc reseeds subtract the
converged clock correctly); magnitude-driven per site — EVERY unsteered receiver
in the fleet has this pro rata. Fix direction (flag-gated, default-off for
byte-identity): pre-seed a robust per-station code-clock estimate (median of code
residuals, StateInitializer already has the residual helper) into x(clock) before
the first ambiguity loop in `ensureRequiredStates`; the existing clk_term then
removes it. Until landed: quarantine high-|clock| stations from lane rows. Open
sub-question: the affine argument says the float solve absorbs the seed benignly,
yet the probe saw non-convergence at full authority — verify what else the cold
seed contaminates (co-cold iono seeds are the prime suspect) during the fix's A/B.

## 8. Perf notes

Triple-frequency processing is the real cost (+⅓ GPS ambiguity columns, more
measurement rows; Galileo more) — measure with D0/P0b at 50 sta before any talk of
305. Detector pair count grows with bands (all-pairs) — Mirror/Triple scope bounds
it; stats are O(pairs) trivial. Lane rows are ≤ #chords each, 4-term rows — the
fold absorbs them like DD priors (WL-1 measured small). `max_rows_per_epoch`
extends per lane if needed (count drops — no silent caps).

## 9. Deferrals (triaged per Invariant #3)

- **Field-inert:** Galileo 19.54 m AltBOC pairs (9.768 m already single-epoch;
  data has L8Q when wanted); BDS/QZSS lanes; iono columns in bridge rows (v2;
  Welford σ + guard price it in v1); general non-1:1 cross-basis inject (counted;
  1:1 covers the common case); EWL→WL→NL netar coupling changes (read-only
  consumer); GLONASS (excluded upstream).
- **Field-blocking if skipped:** the D5 inject exemption (without it the card
  re-creates the F6 storm); the force-fire battery (§7.3 — stiff assertions with
  untested un-fix violate Invariant #1); the station-1 seed quarantine (§7
  pre-flight); the Triple-OFF control baseline (§7.1b — without it every A/B
  confounds band scope with the card).

## 10. Verified anchors (Jul-15 eve tree, uncommitted; re-grep before editing)

- Detector all-cross-band pair builder, frequency-descending, same-channel code
  matching — `MwSlipDetector.cpp` ~437–606; band-scope knobs
  `mw_frequency_mode` (Mirror/Dual/Triple/All) + `mw_channel_mode`.
- Stats keyed `sat_sig1_sig2`; `arcZdMean` pair-generic, F1-lagged, ZD-consistent
  (`MwSlipDetector.cpp` ~179–234); MAD threshold already EWL-aware ("L5X/L8X at
  λ ≈ 19.5 m", `MwSlipDetector.hpp` ~842).
- WL-1 substrate to extend (not fork): layer `NetworkMwWlLayer.{hpp,cpp}` (row
  build, F2 shared-arc Pz, F5 correction frame); wiring
  `ApplicationNetwork.cpp` ~6562–6693 (revmap / arc_signals / primary pair /
  λ cache / `lin_of`); `DdPriorBlock::Term` general rows + fold
  `ũ += G·M·(z_free−ẑ)` (`NetworkNormalEquations.hpp` ~349, ~379).
- Carry driver owns cycle algebra + `installPosterior` + D5 + the slip funnel
  (`NetworkInvariantCarryDriver.hpp`) — the primitive and exemption live there.
- Fixture data (2024-129, sampled ABMF/ABPO/ADIS/AGGO/AIRA/AJAC): GPS C5Q|X/L5Q|X
  everywhere sampled; Galileo C5Q/C7Q + C8Q/L8Q present. `_wlver_50sta_15ep_*`
  = the harness (incl. the `_f6` and `_probe` ON variants); diagnostics
  `network.mw_wl.f6_dump` + `network.mw_wl.debug_r_scale` (probe-only, loud warn,
  NOT production) — both uncommitted, KEEP; dumps saved to scratchpad
  (`network_mw_wl_f6_ep11.csv`, `network_mw_wl_f6_probe.csv`).
- F6 evidence: HANDOFF_mw_wl_card.md §6d; memory `mw-wl-f6-fix-directions`
  (includes the probe result + the held (B) scope sketch).

## 11. Open questions for the implementer (answer in the handoff back)

1. **Triple band scope:** what `getAllowedBandDigits` returns per constellation
   under Triple, and the Galileo third-band choice for the ESTIMATOR (E5b vs E6)
   vs the EWL pair (7,5) — they need not coincide; the resolver just needs live
   columns for bands 5 and 7.
   **Q1b — third-band carry exclusion seam:** confirm the (const, third-band)
   groups can be dropped from the carry driver's GroupInput list while still
   passing through the eq34 election/datum normally (the driver is fed groups
   explicitly, so this should be a caller-side filter), and that OFF byte-identity
   is preserved when `ewl_enable=false` under a Triple config (the §7.1b control).
2. **Bridge iono (ANSWERED by the Jul-16 oracle — Welford-only pricing does NOT
   suffice; correction is mandatory, §3.4):** remaining implementation questions:
   the seam for reading the LAGGED per-LOS iono posterior per (station,sat)
   inside the statistic assembly (value + variance for R), and whether
   `bridge_span_max_km` is still needed once corrected (measure in the A/B:
   log per-fix the iono correction magnitude and the post-correction frac).
3. **Primitive placement:** driver-level API vs ApplicationNetwork seam; how the
   1:1 cycle-support check reads both groups' bases cheaply.
4. **Hold rows vs re-inject:** are hold rows needed at all once injects are
   correct (the stiff carry may hold fine alone between slips), or do they earn
   their keep across re-taps? Ship with holds; measure without in the A/B.
5. **WL-1 float-row disposition:** keep `mw_wl.enable` as diagnostic (recommended)
   or fold its telemetry into the lane counters and retire the toggle.
6. **Dual-freq inject maturity:** confirm the σ(n) law on-fleet (census says
   0.5–0.7/√n) and whether n≈25–50 injects are worth it inside short windows, or
   dual-freq sats simply wait for graded runs.
