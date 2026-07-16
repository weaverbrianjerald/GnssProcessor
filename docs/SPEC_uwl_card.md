# SPEC — Ultra-Wide-Lane tier (M5 card "UWL-1", the WL-4 slot)

**Status:** IMPLEMENTED increments 1–5 (Jul-16 2026, Fable) + the v2
per-family pair groups + smoke-gated; see `docs/HANDOFF_uwl_card.md` for gate
results and the measured deviations (joint pair census replacing the falsified
per-band census; cold-fix aggregation math; unlagged decision-only serve
semantics; steered-pair family admission replacing the falsified raw-census
count).  Increment 5 landed as `network.mw_wl.uwl_dual_top` — band-8 PREFERRED
per cycle with (5,7) fallback, both siblings uncarried, dual lane instances
with walk-level dedupe.  **The §1b dependent-sibling closure was MEASURED and
its naive hard gate is a NO-GO: 2·N(8,5) − N(7,5) is a clean per-cycle integer
constant ≠ 0 (per-signal ambiguities acquire independently; the AltBOC
coherence holds only modulo acquisition) — `uwl_sib_gate` ships default-OFF,
telemetry-only.**  The v2 census recovery landed as
`network.mw_wl.uwl_family_pairs` (E minority family recovered: 15-ep
skip_basis 139 → 0, constrained lane population ×2.8 at circR 0.813).
Deferred to the graded session: the §6.5 Galileo force-fire + cold latency
A/B.  Default-OFF, byte-identical when off; GPS-only configs stay
BIT-identical even when the card is armed (verified — the tier only exists
where a constellation has a close-frequency pair).

**One sentence:** give the cascade a per-constellation TOP tier — the
closest-frequency "ultra-wide" lane (Galileo/BDS-3 (E5a,E5b)/(B2a,B2b),
λ = 9.768 m) whose integer is decidable from a SINGLE epoch of raw code — and
generalize the WL-2/WL-3 machinery (statistic → FFR → bridge → lane carry) from
its hardcoded GPS band digits to a per-constellation band map, so Galileo enters
the wide-lane cascade with a stronger seed than GPS ever had.

**Relation:** WL-2 built the cascade (EWL statistic → FFR integer → GF bridge →
WL target) and WL-3 made the carry its accumulator (pair blocks, per-epoch
increments, idempotent lane re-pin, inject retirement).  Both are VALIDATED
GPS-only — and, as coded, are structurally GPS-only (§1 fact 3).  This card is
the multi-constellation generalization plus the one new mechanism the UWL's
wavelength earns: the single-epoch cold fix (§3.3).  The lane-trace grader, FFR
gates, closure check, quarantine, force-fire harness and the WL-3 telemetry all
carry over UNCHANGED as measurement sources and acceptance instruments.

---

## 1. Motivation (measured facts + physics, do not relitigate)

1. **The GPS EWL is band-starved on real networks:** 4272 missing-band skips
   over 15 ep at 50 stations (L5 is tracked by a station subset) vs 4797
   primary cycles inspected — the cascade's seed exists on barely half the
   graph.  Galileo E1/E5a/E5b is tracked by the SAME fixture (C5Q/C7Q/C8Q
   confirmed in 2024-129) and sits idle.
2. **λ decides everything.**  Galileo/BDS-3 close pair Δf = 30.69 MHz ⇒
   λ_UWL = 9.768 m.  A raw DD code combination at σ_code ≈ 0.3–0.7 m is
   0.03–0.07 UWL cycles — FFR-decidable at n = 1, no Welford warm-up.  GPS's
   best pair is (L2,L5) Δf = 51.15 MHz ⇒ 5.861 m (the current "EWL"); GPS has
   NO pairwise ultra-wide lane (the (E1-band,L5) pair is 0.751 m — narrower
   than WL).
3. **The current code is structurally GPS-only, silently.**  Three hardcoded
   band digits: `band5_of` scans `sig[1]=='5'`, so for Galileo it returns E5a —
   the SAME signal as the primary sig2 — and the "EWL" pair degenerates to
   (L5Q, L5Q), which the detector has no arc for ⇒ every Galileo cycle dies as
   `skip_missing_mw`.  `updateBridgeArc` picks bands {1,2,5} by digit — Galileo
   has no band '2', so no bridge arc ever forms.  Only `isUncarriedThirdBand`
   already knows Galileo's third band is '7'.  Nothing warns (invariant #4
   violation in spirit: the lane looks "on" and folds nothing).
4. **Slip re-warm latency is the measured residual defect of WL-3:** the
   force-fire shows evaporate → ~2–3 epochs of no-arc re-warm → re-fix, and the
   15-ep lane trace's only dirty window (eps 14–15, med|f| 0.18/0.31) is
   end-of-run slips caught mid-re-warm.  A UWL cold fix (n = 1) collapses that
   window to ONE epoch on constellations that have the tier — and via the
   bridge, the WL lane re-pins the epoch after.
5. **Known Galileo hazards are on file and must be pre-flighted, not
   rediscovered:** the 305-station attribution's poison pair (UFPR/POAL × Gal,
   cured by the H1 iono-deweight gate — armed in the lineage configs); the MW
   detector's per-arc frame bookkeeping (below — a counter to watch, NOT a
   product dependence); GLONASS stays excluded (FDMA); BDS-3 has the same
   9.768 m pair but is BLOCKED by the open BeiDou network clock divergence
   (memory `beidou-network-clock-divergence`) — deferred increment, not
   silently armed.

   **The lanes have ZERO information dependence on the .BIA** (the
   self-contained-estimation directive holds): every lane statistic is a
   closed-cycle DD, and receiver + satellite MW hardware biases cancel
   identically around a closed cycle, corrected or not.  The only .BIA
   interaction is detector PLUMBING: ApplicationNetwork sets
   `setCorrections()` on the per-station detectors, so arcs where OSB entries
   happen to exist are stored in the `bia_corrected` (SD-pivoted) frame and
   arcs without them raw-ZD; cancellation needs ONE frame per cycle, so
   mixed-frame cycles are skipped.  Uniform absence (e.g. no L8Q/C8Q OSBs
   anywhere) ⇒ the whole pair runs no-BIA, zero skips, nothing lost.  The
   hazard is PARTIAL per-satellite coverage within a pair — some satellites'
   arcs corrected, others not — which silently thins the pair graph via
   `skip_mixed_frame`.  Pre-flight = watch that counter (invariant #4), never
   "require the product".

## 1b. Integer-accessibility accounting (the point of the ladder)

The card's "information" is DECISION information: N ∈ ℤᶠ is worth F discrete
constraints per cycle, each inaccessible until its direction's float σ crosses
the FFR threshold (~0.1·λ_eff).  The ladder is the complete basis — F−1 lane
rungs + 1 narrow-lane rung — and each rounded rung injects a constraint the
Gaussian float never contained (the WL-3 lane re-pin is the fold mechanism).
Galileo with {E1, E5a, E5b, E5-AltBOC} (F = 4):

| rung | λ | n=1 frac σ | accessible |
|---|---|---|---|
| E5a−E5 (or E5b−E5, AltBOC) | 19.537 m | ~0.01 cyc | single epoch |
| E5a−E5b | 9.768 m | ~0.03–0.07 cyc | single epoch |
| E1−E5a (WL) | 0.751 m | — | epochs, bridged from the rung above |
| NL (the last integer per cycle) | c/(f₁+f₅) = 10.9 cm | — | σ_DD ≲ 1 cm — NOT this card |

Only two of the three {5,7,8} differences are independent: decide on the widest
CLEAN pair per arc, use a dependent sibling as the closure gate (integrity, not
folds), and only ever re-pin the F−1 independent lanes — a third dependent lane
row is the F-A refold class again.  The NL rung (asserting the remaining
individual integer) stays the M5 network-AR endgame, gated on DD convergence
per the standing memory: this card collapses the per-cycle search 4-D → 1-D and
stabilizes the float the NL rung must converge, nothing more.

## 2. Representation (no new algebra)

**A lane TIER is a per-constellation (band_hi, band_lo) pair plus a third-band
role**, replacing the three hardcoded digits with one table:

| cons | primary pair (WL tier) | top tier (statistic/bridge seed) | top-tier λ | uncarried band(s) |
|---|---|---|---|---|
| G | (1, 2) | EWL (2, 5) | 5.861 m | 5 |
| E | (1, 5) | **UWL (5, 8) where band-8 tracked, else (5, 7)** (§7.5; the untaken sibling = closure) | 19.537 / 9.768 m | 7, 8 |
| C (deferred) | (1, 5) | UWL (5, 7) | 9.768 m | 7 |
| R | excluded (FDMA) | — | — | — |

Everything downstream is ALREADY generic in (λ_a, λ_b): `arcZdMean`/`arcZdLast`
key on explicit signal pairs; the bridge's `iono_coeff` is computed from actual
frequencies; the WL-3 pair carry, census, lane functional and re-pin never
mention band digits.  The card threads the table through exactly four seams:
`band5_of` → `bandTopOf(cons)`, the EWL resolver's pair selection,
`updateBridgeArc`'s per-band signal pick (+ `setBridgePreferredChannels`
lookups), and the run-end telemetry (per-constellation lane lines — a GPS EWL
line and a Galileo UWL line, never a pooled one; the F-A dump taught us pooled
lines hide which lane is storming).

The carried structure is untouched: Galileo pair groups (E1,E5a) already form
under WL-3's census (band digits '1' and '5' are its two lowest); the E5b band
is already carry-excluded by `isUncarriedThirdBand`.  The UWL integer, like the
EWL integer, is DECISION-side only (`ewl_fold_rows=false` stays the law — the
F-A refold class does not get a second chance).

**CORRECTION (measured, Jul-16 impl):** "Galileo pair groups already form" was
FALSE — the WL-3 census elects per-band channel majorities independently, and
Galileo channels come in correlated receiver families ((L1C,L5Q) vs (L1X,L5X)),
so the census composed the phantom pair (L1C,L5X) that no station tracks and
ZERO Galileo pair arcs formed.  The card therefore includes a JOINT pair census
(elect the majority (sig1,sig2) pair per constellation over the raw
two-lowest-carried-band pairs; ties lexicographic), gated under `uwl_enable`
for byte-identity with the validated WL-3 shape.  The minority family stays
per-band in v1 (~half the Galileo pair space; per-family pair groups are the
v2 recovery).  See HANDOFF deviation 1.

## 3. Mechanism

### 3.1 Per-constellation band map (the generalization)

One constexpr table (cons → {b1, b2, b_top}) consulted by the four seams in §2.
GPS behavior must be BIT-identical (same digits fall out of the table).  A
constellation absent from the table contributes no top-tier lane and WARNS once
if the lane master is on while its cycles are being skipped (fact 3's silent
death becomes loud).

### 3.2 UWL statistic + bridge on the existing machinery

Nothing new: `NetworkMwWlLayer` builds MW(5,7) rows for Galileo exactly as it
builds MW(2,5) for GPS once the resolver hands it the right pair; the FFR gates
(`ewl_fix_sigma_cyc`, `ewl_fix_frac_cyc`) apply unchanged — at λ 9.768 m they
are LOOSE gates, which is the point.  The bridge becomes
GF_b = φ_WL(E1,E5a) − φ_UWL(E5a,E5b) with its iono coefficient from the real
frequencies (already generic); the WL-3 lane rows and re-pin then run verbatim
on the Galileo pair groups.

### 3.3 UWL cold fix (the one genuinely new mechanism)

The current eligibility (`count ≥ ewl_min_arc_epochs = 2`) exists because a
variance-of-mean needs two samples — a CODE-noise argument that λ 9.768 m makes
obsolete.  Under `uwl_cold_fix`, a top-tier arc with n = 1 is FFR-eligible
using an A-PRIORI per-sample σ (config, default 0.7 m ⇒ 0.072 UWL cyc; the
Welford variance takes over at n ≥ 2, and the a-priori floor never applies to
tiers with λ < a threshold that excludes GPS's 5.861 m EWL — the cold fix is a
UWL privilege, earned by wavelength, deliberately NOT extended to GPS in v1).
Effect: a slipped Galileo cycle re-enters the cascade the FIRST epoch its arcs
re-form — statistic-gated as always, closure-checked against MW(1,5), and the
§3.4-style self-heal shortens from ~3 epochs to ~1.  Scope guard: the cold fix
changes DECISIONS only (which integers the bridge may use); it folds nothing.

**CORRECTIONS (impl, Jul-16):** (a) the a-priori σ is per ARC, and the DD row
aggregates √(Σc²)·σ/λ — an ALL-cold 4-arc cycle (~0.14 cyc at the default)
correctly self-gates at the 0.10 FFR σ gate; the target case is ONE re-warming
arc inside a mature cycle (~0.072 cyc, passes).  (b) The cold reads are served
UNLAGGED (`arcZdMeanCold` n≥1, `arcBridgeCold` n≥2) — the F1 lag would postpone
the n=1 decision by an epoch; safe because they are decision-only and nothing
folds (cold rows are suppressed even under the `ewl_fold_rows` study shape).
Post-slip timeline: statistic decision at t+1, bridge re-pin at t+2.  (c) In a
cold window the MW(1,5) closure arc has usually reset too (absent ⇒ not
blocking) — cold decisions are frac-gated + D5-backstopped + idempotently
re-decided; the §6.5 force-fire must show zero wrong integers.

### 3.4 Lifecycle / slips

Identical to WL-3: the funnel (MW/LLI/IF-TD/RAIM) refloats arcs; pair chords
union-refloat under the lane shape; the D5 gate is the fine-slip backstop; the
bridge gross-jump guard (0.4 m) is λ-checked (at λ_UWL the guard is 0.04 cyc —
still comfortably below a 1-cycle slip at 9.768 m ⇒ keep the absolute meters).

## 4. Estimability / invariants pre-flight

Same closed-cycle class; no datum change; no new states.  Standing invariants:
(1) every new assertion is statistic-gated + slip-funnled + D5-backstopped —
the cold fix asserts nothing stiff (decision-only) and the force-fire battery
(§6.5) proves the response on a GALILEO satellite this time; (2) Release-only;
(3) deferrals triaged in §7 (BDS field-blocking on its own open memory, AltBOC
study inert); (4) per-constellation run-end counters — a lane that decides 0
cycles on an armed constellation is a loud line, not silence; (5) grading on
datum-invariant quantities (lane trace, dd-frac) split per constellation;
(6) default-OFF; GPS-only configs bit-identical (the band table reproduces the
old digits); shared-path edits (the four seams) re-verified against the WL-3
byte-identity reference exe.

## 5. Config (`network.mw_wl.*` — the tier lives with the lanes it seeds)

| key | default | meaning |
|---|---|---|
| `uwl_enable` | false | Arm the top tier for constellations with a close pair (currently E; C blocked, see §7).  Requires `ewl_enable` (shared machinery + telemetry).  OFF ⇒ byte-identical, including under multi-GNSS `active_systems`. |
| `uwl_cold_fix` | false | §3.3 n=1 FFR eligibility on tiers with λ ≥ `uwl_cold_min_lambda_m`. |
| `uwl_cold_sigma_m` | 0.7 | A-priori per-sample DD statistic σ (m) used as R until the Welford variance matures (n ≥ 2). |
| `uwl_cold_min_lambda_m` | 8.0 | Wavelength floor for cold-fix eligibility (excludes GPS EWL 5.861 m by design). |

Galileo itself arms via `processing.active_systems` (no new toggle); the
existing FFR/bridge/quarantine knobs apply per lane unchanged.

## 6. Validation (quick-only until commanded otherwise)

1. **Byte-identity ×2:** (a) all-OFF vs the WL-3 reference exe on the GPS-only
   harness (the four generalized seams are shared-path); (b) `uwl_enable=false`
   with `active_systems=[GPS,Galileo]` vs the same config on the pre-card
   build — arming a constellation without the card must change nothing.
2. **Unit battery:** band-table round-trip (GPS digits reproduce bit-exact;
   Galileo yields (5,7)/(1,5)); degenerate-pair guard (fact 3's (L5Q,L5Q) can
   never form); cold-fix eligibility (n=1 eligible iff λ ≥ floor; GPS EWL
   ineligible; Welford takeover at n=2); bridge λ/iono coefficients for
   (E1,E5a,E5b) hand values.
3. **10-ep GPS+GAL smoke** (`_wlver_50sta_10ep_uwl.json`): Galileo UWL line
   decides > 0 cycles with mean R ≪ the GPS EWL's; GPS EWL line unchanged vs
   the GPS-only run; no warnings/NaN; D5 within family of the SAME-build
   GPS+GAL control (new control leg — adding a constellation moves the
   baseline; never compare against the GPS-only 358/373).
4. **15-ep A/B + lane trace per constellation:** Galileo constrained
   population circR ≥ the GPS one at the same epoch count (the stronger seed
   should show), injects ≈ 0, per-cons med|f| ≤ 0.1 post-fix.
5. **Force-fire on a Galileo satellite** (synthetic slip, the WL-2 §7.3
   recipe): with `uwl_cold_fix` ON, the re-fix latency after re-warm is
   measured ≤ 1 epoch vs ~3 without (the A/B pair IS the cold-fix acceptance);
   zero wrong integers (closure + census truth).
6. **Poison-pair watch:** if UFPR/POAL (or any 305-attribution culprit) is in
   the 50-sta list, confirm the H1 deweight engages rather than letting Gal
   condition-collapse; report the deweight counters in the run summary.
7. Perf: expect +1 detector pair + 1 layer pass per Galileo station; D0/P0b
   brackets, no budget concern expected.

## 7. Increments

1. **Band-map generalization + loud-skip warn** — the four seams, GPS
   bit-identity gate, unit battery.  (The risky increment; it touches the
   shared path.)
2. **Galileo arming + control legs** — GPS+GAL harness configs, new same-build
   controls, the 10-ep smoke (§6.3).
3. **UWL statistic/bridge/lane validation** — 15-ep A/B + per-cons lane trace
   (§6.4) + force-fire (§6.5, without cold fix).
4. **Cold fix** (§3.3) + its force-fire latency A/B.
5. **Galileo AltBOC top rung** (user-directed, Jul-16): promote (E5a,E5) /
   (E5b,E5) — Δf = 15.345 MHz ⇒ λ = 19.537 m, AltBOC code ~10–20 cm ⇒ n=1 frac
   σ ~0.01 cyc — to the tier table as Galileo's PREFERRED top rung where band
   '8' is tracked, with (E5a,E5b) as the fallback top rung AND the dependent-
   sibling closure gate (§1b).  Pre-flight: fixture C8Q coverage (confirmed
   present at least partially) and the §1.5 frame-uniformity counter — NO .BIA
   dependence (uniformly absent L8Q/C8Q OSBs simply run the pair no-BIA, which
   is fine in closed cycles); only PARTIAL per-satellite OSB coverage matters,
   and only as `skip_mixed_frame` thinning to watch.
6. (deferred) BDS-3 (B2a,B2b) — BLOCKED on `beidou-network-clock-divergence`;
   do not arm until that memory closes.  (study) the GPS triple-frequency
   integer combination (−1,8,−7)·(L1,L2,L5), f_c = 10.23 MHz ⇒ λ = 29.31 m,
   phase noise √(1+64+49)·σ_φ ≈ 0.1 combo-cyc and iono coefficient in cycles
   SMALLER than the WL's (−0.068 vs −0.180 per GHz⁻¹ terms) — a plausible GPS
   "UWL substitute", but it is a three-signal combination (new statistic, new
   bridge identity, no pairwise MW) — a separate card if the Galileo tier
   proves the ladder is worth a third rung on GPS.

## 8. Open questions for the implementer

1. Galileo primary-pair census under `frequency_mode=all`: E1 channels (L1C vs
   L1X) and E5a channels (L5Q vs L5X) — confirm the WL-3 census picks the
   majority channels and the fixture actually pairs (E1,E5a) at most stations
   (the WL-3 fragmentation lesson says measure before assuming).
2. Frame uniformity per pair (§1.5 — NOT a product dependence): is the OSB
   coverage the fixture happens to load all-or-nothing PER SATELLITE for each
   Galileo pair?  Uniform (either way) ⇒ zero mixed-frame skips.  Partial ⇒
   `skip_mixed_frame` thins the pair graph; if measured non-trivial, the clean
   cure is to force the lane-facing pairs uniformly no-BIA (drop the correction
   for those detector arcs) rather than to demand fuller products — the lanes
   never needed the OSBs (closed cycles cancel the biases exactly).
3. The closure cross-check for the UWL decision: MW(1,5) is the natural
   iono-free-ish check (the analogue of MW(1,2) for GPS) — confirm its Welford
   arcs exist under `mw_frequency_mode` on the fixture, else the closure gate
   starves and the card loses its wrong-integer backstop (invariant #1).
4. Does the WL-3 lane re-pin need a per-cons `fix_weight_sigma_cyc`?  At
   λ(E1,E5a) = 0.751 m the same 0.01 cyc is 7.5 mm — probably fine; check the
   D5 per-cons flag rates before inventing a knob.
