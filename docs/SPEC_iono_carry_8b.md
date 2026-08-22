# SPEC — QUEUE 8(b): the per-LOS slant-iono carry (`network.carry_iono`)

**Session 2026-08-20/21 (continuation of the slip-relevel campaign).  Status:
EXECUTED AND CLOSED — the solve-fed carry is falsified by intervention at both
scopes (§6.1/§6.3) and the CS-GF code-anchored successor is falsified at both
scopes too (§6.5); the closure reading is §6.6.  All knobs remain default-OFF
research toggles; the OFF path is byte-certified (§5).  Companion build in the
same session: bridge v3 (`SPEC_slip_relevel_walk.md` §8.3) — validated,
integer-clean, and NOT implicated by this card's falsifications.**

QUEUE item 8(b) — "the per-LOS ionosphere representation, now with an
instrument" — is the owner of the stationary ~2.3–3.1 m ZWD plateau (and
3.57 m interaction) that survives the slip-relevel fix
(`SPEC_slip_relevel_walk.md` §8).  This card names the representation defect
precisely, diagnoses what the plateau is made of on existing run data, and
builds the temporal accumulation channel the record itself had already asked
for twice (`project_global_sh_iono` "the per-LOS deviation carry is precisely
the missing time-link"; `SPEC_iono2_mechB_v3.md` "NL needs a temporal
accumulation channel, here for the per-LOS ionosphere").

## §1 The representation, stated exactly (code facts, verified)

On the network path the per-(station,satellite) slant iono is **white per
epoch** — it has no temporal prior at all:

- The per-station `StateManager` vector is a frozen LINEARIZATION POINT
  (`StateInitializer.cpp` "SCOPE HONESTY"); the per-epoch absolute network
  solve carries NO Kalman prior on the local nuisances; x_lin shifts are exact
  solve no-ops (x_r absorbs).
- The KF iono process-noise block is gated `ProcessingMode::PPP_RTK` — the
  network mode never reaches it (`filter.process_noise.ionosphere_q` is a
  PHANTOM knob here, `project_adaptive_iono_q_network`).
- The GF-code seed (`StateInitializer::computeIonosphereSeed`,
  `iono_seed = ((P1−mod1)−(P2−mod2))/denom`) runs at **arc birth only** —
  never on refloat, never on slip.  Its value carries the station's receiver
  GF hardware bias (systematic, station-common) but is solve-inert as a frame.
- The only time-linked per-LOS element is the optional mech-B GM deviation
  about the SH field (`rep_error_tau_s`, default off, SH-posture-only, erased
  on every slip-funnel entry).  The campaign posture (GF-datum, no SH) has
  none of it.

Consequence: each epoch the per-LOS iono is determined by that epoch's GF code
(σ 0.66 m zenith → 1.5+ m at cutoff), the SH tie row when armed (σ ~0.8 m),
and the phase rows **through the carried levels**.  Mid-arc the zero-q levels
(`pl_q_amb = 0`) transport the LOS's iono realization at phase precision — the
per-epoch code rows carry weight fraction ~1e-2–1e-5 against them.  So the
iono error is a **per-event constant transported by the levels**, re-drawn
from one epoch of GF code at every re-level event.  "Chord churn IS the iono
representation" (`SPEC_lane_spectrometer.md`), now with the mechanism explicit.

## §2 What the plateau is made of (measured on existing arms, no new runs)

Instrument: `scratchpad plateau_decompose.py`-class join (per-event interaction
jumps, station-common time series, arc-age strata), join gates byte-matched to
the recorded graders (F2 ZWD settled rms 2.8451 ≡ grade.json; hourly ≡ SPEC
§8 table; interaction 3.5678 ≡ simnet_iono_interaction).

On `runs/camp60_f2` (slipped fixture, relevel fix armed):

- **The error persists THROUGH re-levels**: detector events (n=4 782)
  corr(post,pre) = **0.90**, per-event jump p50 |Δ| = **0.12 m** (rms 2.17,
  heavy-tailed).  The re-drawn level re-absorbs nearly the same error.
- **The plateau is not arc-local**: arcs with NO event in the whole settled
  window still carry r rms **3.57 m** — equal to the fleet average.
- **It is a displaced station-common exchange state**: the time-VARYING
  station-common component of the interaction (which the two-way fit's
  constant a(sta) does NOT remove) has σ **0.8–4.1 m per station**, and is
  **anti-correlated with the ZWD error** — median corr −0.46 across the
  fleet, −0.80…−0.92 at the worst stations (S27: ZWD rms 9.8 m, common σ 4.1,
  635 events).  This is the documented ZWD⇄iono exchange, live.
- **Events sustain it**: fleet corr(log ZWD rms, log event count) = **0.74**
  (the walk-era corr with slip count was 0.11 — the fix changed the regime
  from integrating to event-sustained).
- **T4 is the ÷K attenuator, confirmed at the cell level**: T4 events
  (n=7 805) fire on the worst cells (pre rms 6.05) and barely improve them
  (post 5.72, corr(post,pre) 0.83).
- Age strata decay 5.47 → 2.75 m over ~20 min is a **selection effect**
  (T4 re-erases hot arcs; the survivors are the good draws), not convergence.

The **quiet twin** (`runs/camp60_quiet_f2`) shows the SAME structure at ~5×
smaller amplitude: worst-station exchange corr −0.84…−0.90, common σ 0.2–0.45,
identical event-count correlation (0.74), same age-decay shape, ZWD 0.229
converging.  The mechanism is intrinsic to the white-per-epoch representation;
the slip rate is an amplitude multiplier on it.

**Why nothing drains the displaced state**: the code channel observes
iono+CB absolutely every epoch, but nothing INTEGRATES that evidence — levels
are stiff, iono and ZWD are white, so each epoch is a fresh compromise in
which the levels win.  Every re-level then copies the current displaced state
into the new level (corr(post,pre) 0.9).  The missing piece is an
accumulator on the iono side of the exchange — exactly O2's time-link.

## §3 The design (panel-reviewed; adversary corrections adopted)

**`network.carry_iono`** — a per-LOS slant-iono carry, the ZWD-carry (B9) /
rx-CB-carry shape: post-solve harvest stashes {value_abs = x_lin + x_r, var}
per (station, sat) **every epoch**; PASS-B injects a diagonal information
prior on the iono column, `var_eff = max(var + q·gap, floor)`,
`η = λ·(value_abs − x_lin(inject-time))`.

Knobs (all `network.*`): `carry_iono` (false), `iono_carry_q` (1e-4 m²/s via
`NetworkQClass::IonoCarry`, honest quiet-time slant rate ~5.5 cm/30 s),
`iono_carry_scope` ("relevel" | "always", default "relevel"),
`iono_carry_var_floor` (0.04 m² = (0.2 m)²), `iono_carry_max_gap_s` (300 —
deliberately LONGER than `pl_max_gap_s`: the post-gap re-birth is where the
carry beats a fresh code seed hardest).

Panel-decisive points (adversarial review, all verified in source):

1. **The inject-side variance floor is load-bearing** (the design's one
   missing guard as first briefed): the harvested marginal is
   phase-locked-tiny mid-arc, an unfloored prior out-weighs fresh GF code
   ~80:1, and a wrong carried value is invisible to every armed instrument —
   T4 pools phase rows only, phase is self-consistent with a level re-leveled
   against the wrong carry, the prefit census is x_lin-framed.  At the floor
   the per-event code adoption is ~10%: a wrong carry heals over ~10 events
   while the churn kill (adoption² in variance) survives.  Companion
   instrument: prires family `"iono"` (the one instrument that CAN see it).
2. **scope=relevel's inject predicate must catch the T4-erased population**:
   `free_ambs` alone misses the majority flux (7 876 T4 erases vs ~4.5k
   detector latches, and T4's mark is consumed by the PASS-B pre-pass — the
   same dead-check trap the relevel card documents).  The predicate is:
   detector-freed (latch hit) **OR** present-but-carryless on every band
   (post-T4-erase / gap-re-birth / datum-station mass reseed) — carry-absence
   is the only readable T4 signature at the only site and epoch it exists.
3. **Frame-exactness needs nothing**: store `value_abs` only and form η
   against the CURRENT x_lin read at inject time (post-`ensureRequiredStates`)
   — exact across gap re-birth reseeds and any x_lin rewrite.  Storing the
   delta or a stash-time x_lin is the defect.  Bonus: the re-birth code seed
   and the carried value live in the SAME realization ι̃ = ι + d_r,GF − d^s_GF
   (both embed the receiver GF bias) — disagreement is noise-class.
4. **Event edges (invariants #1/#7)**: KEEP through a slip whose bands were
   freed this epoch (the physical ionosphere is continuous through a receiver
   cycle slip; the free epoch disarms the stale-level aliasing channel);
   ERASE on un-freed funnel entries; ERASE on every T4/consensus arc erase
   (attribution cannot split level error from carried-iono error — the T4
   statistic reads their sum); retap-KEPT arcs keep their iono carry; gap
   eviction.  The code-datum re-realization edge (GF-pin re-election /
   cb_excursion_reinit) ships BARE with the declared-zero counter
   `n_iono_carry_refused_datum` — **open-valve inventory entry** (see
   `SPEC_prefit_census_observability.md` §13 discipline).  Phase-datum events
   (refsat rebase, anchor handover) deliberately do NOT touch it: the carried
   quantity is a code-frame object.
5. **Ratchet-proofing** (the F4/mech-B lesson): on the GF-datum posture the
   quasi-null partners (rx-CB, sat-CB) are closed by fixed-target datum rows
   re-asserted identically every epoch — a ratchet needs two MOVING time-linked
   channels, so loop gain < 1 strictly and a demean would DELETE real
   station-common ionosphere (no SH field owns the common mode here).  Watch,
   don't actuate: lump-drift a(sta)/b(sat) per arm + the σ̂₀-family split
   (prior-quiet/data-loud divergence is the over-retention signature).
   **Posture-scoped**: under SH the a00 channel moves and the gain<1 argument
   fails — loader disarms under `estimate_iono_sh` until a demean-analog
   derivation exists.  Mutex with mech-B (`rep_error_tau_s > 0`): one temporal
   channel per physical column.  Disarmed under `carry_invariant` (the
   relevel trigger population is structurally empty there; the
   scope=always/carry_invariant pairing is the future O2 card, unbuilt).
6. **Fingerprint**: conditional-append `ionocarry:<scope>:<q>:<floor>:<gap>`
   (solve-affecting — never flip mid-chain; the rxcb/shl2/iono2 pattern).
   The carry map itself is NOT snapshotted (relevel_pend precedent): an
   armed→armed warm resume cold-starts it, one epoch of today's white-iono
   behavior — the conservative direction.
7. **Diagonal fold only** — even under `carry_conditional_coupling` (the
   rx-CB rationale: a coupling row hands a strictly-local state a
   shared-border footprint and pins the OLD gauge across datum
   re-realizations).  A local diagonal prior is invisible to the S-basis
   plan, the datum-graph census, and the prefit census; pre-registered W5
   check: `n_gate_withheld` must stay 0 on the first ON run.

**Pre-registered expectation (corrected by the adversary — do not grade
against more)**: the win is the **re-draw-event churn kill** — at each event
the freed level re-levels against the transported pre-event iono instead of
one epoch of GF code + the systematic receiver-GF bias.  Target: interaction
3.57 → 0.7–1.2 m plausible, ZWD churn kill, T4 storm shrink (the
re-level→tension→T4 feedback broken).  Explicitly NOT dm-class iono and NOT
O2/NL delivery on a leveled posture (mid-arc the levels transport iono at
phase precision regardless — the code's per-epoch information cannot move a
level-locked iono; O2 delivery is the future scope=always arm on the
carry_invariant lineage).  §2's persistence finding adds: scope=always is
expected to out-perform scope=relevel on the DISPLACED-state drain (the
always-on accumulator is what integrates the code evidence between events);
both arms are run.

## §4 Implementation map (landed)

- Carry map `NetworkCarryState::iono_carry` (`NetworkRunContext.hpp`, beside
  `zwd_carry`; `LocalCarry` reused).  Knobs snapshot in `NetworkKnobs`.
- Inject: `ApplicationNetworkAssembly.cpp` PASS-B, own block after the ZWD
  carry inside the widened `legacy_local_inject || … || iono_on` bracket;
  per-sat re-level census (freed / seen / carried sets) collected in the
  pl_amb walk; diagonal tuples join the same sorted `inj` fold →
  `est.addStationPrior`.
- Harvest: `NetworkRunContext.cpp networkHarvestLocalCarries`, RPB-shaped
  plural-column loop + prune (own gap) + prires `"iono"` family (folded =
  ground truth from the injector).
- Slip edges: serial PASS-B pre-pass beside the mech-B funnel block
  (KEEP-if-freed / ERASE-else); T4 hook in `NetworkRaim.cpp refloat_sat_at`
  beside the `local_carry.erase` (after the retap-KEPT `continue`).
- Config: `GnssConfigLoadNetwork.cpp` parse beside `carry_zwd`; interlocks at
  the END-of-network-section site (the Card-1g parse-order pattern);
  `NetworkQClass::IonoCarry` table row (append-only, count 11→12 — the run
  manifest gains one process-noise field, excluded from byte-cert).
- Counters `n_iono_carry_{injected,kept_through_slip,erased_slip,erased_t4,
  evicted,floor_hits,refused_datum}` + teardown line (both edges, invariant
  #4/#7).
- Tests: `tests/test_iono_carry_config.cpp` (21 cases in two suites:
  `IonoCarryConfig` ×17 — defaults, the four disarm interlocks + mech-B mutex,
  scope/source semantics, hard rejects, Q-table routing, the two bridge-v3
  rider interlocks; `IonoCsKernel` ×4 — the pure CS-GF recursion: honest-var
  code seed, 200-epoch convergence under a 2 cm/ep ramp, slip/gap resets,
  unusable-sample refusal).

## §5 Byte-certification

OFF-path: `_camp60_f4.json` knob-verbatim on the new exe vs the `runs/camp60_f4`
reference — `network_zwd.csv`, `network_events.csv`, `closed_loop_trace.csv`,
`network_solution.csv`, `network_ionosphere.csv` all **byte-identical**.
Product-file identity: certified against `runs/camp60_refprod` (the pre-8b exe
snapshot `gnss_processor_fable_run.exe` = 843405FA re-run with output
redirected) — see §6.  ⚠ Process note: the first byte-cert config edited
`files.product_output_path`, which is UNUSED — the live knob is
`network.product_output_path` — so the run overwrote `runs/camp60_f4/products.txt`
(solve CSVs proved identical; the reference product was regenerated from the
archived exe snapshot).  The arm-config generator now edits the network-side
knob; any future arm derivation must too.

**bytecert4 (the CS-GF-fixed exe, MD5 6B3935C386B16A82D8583A5E7C319044 —
the D1+D2+D3b miswire fix, `gf_code_m`/`gf_denom` now assigned into
`RawSlipObs` at `NetworkRawObservables.cpp:114`)**: `runs/camp60_bytecert4`,
`_camp60_bytecert4.json` verified knob-verbatim vs `_camp60_f4.json` (masked
`_comment` + `product_output_path` only).  `products.txt` byte-identical to
`runs/camp60_refprod` (17 081 070 B, sha256 192C7E07…) and all five solve CSVs
byte-identical to `runs/camp60_f4/logs`; run manifest differs in `loop_ms`
(wall clock) + config path/hash only — no counter moved; no carry teardown
line on the OFF path (gated).  Suite on the same source: 1469/205, 1468 pass +
the one fixture-gated skip.

## §6 Validation (slipped 60-sta fixture + quiet twin, single-knob ladder)

### §6.1 i1 (scope=relevel) — FAIL against pre-registration, mechanism-decisive

`runs/camp60_i1` (exe E102C7D7, `_camp60_i1.json`, single knob over the
byte-certified OFF path):

| channel | OFF (f2/f4 class) | i1 | pre-reg gate | verdict |
|---|---|---|---|---|
| ZWD settled rms / p50 | 2.845 / 0.578 | **3.099 / 0.660** | ≤ 2.0 | **FAIL** |
| ZWD hourly | 2.34 2.34 3.03 3.04 3.09 2.35 | 2.32 2.27 3.44 3.54 3.23 2.42 | flat | flat-ish, mid-hours worse |
| interaction rms | 3.568 | **3.800** | ≤ 2.0 | **FAIL** |
| consumer 3D rms / p50 | 4.06→3.26 class | **1.901 / 1.707** | ≤ 3.26 | **PASS (large)** |
| clock contrast | ~13 | 13.5 | unchanged | as expected (not this card) |
| census | 96.8 % | 96.9 % (2340+68 / 78 silent) | ≥ 96 % | PASS |
| T4 arc events | 7 876 | 8 098 | shrink | FAIL (flat) |

Counters (teardown): 4 890 injected (4 748 at the var floor — as designed),
3 620 kept through freed slips, 0 erased at un-freed funnel entries (expected
on this posture), **8 165 erased by T4**, 1 133 evicted, 0 datum refusals.

**The two findings that make this a clean falsification rather than a null:**

1. **The churn kill happened and the plateau rose anyway.**  Per-event
   detector jump p50 |Δ| 0.122 → **0.094** m, station-mean event step 0.37 m —
   the re-draw noise the panel's model targeted is measurably reduced — while
   the persistence signatures ALL strengthened: corr(post,pre) 0.90 → 0.92,
   never-re-leveled arcs 3.57 → **3.90** m, corr(ZWD rms, event count)
   0.74 → 0.81.  The plateau is **drain-limited, not churn-limited**; §2's
   displaced-exchange reading wins over the churn model, now by intervention
   rather than correlation.  The consumer's 40 % improvement is the churn
   kill showing on the wire — real, and worth keeping.
2. **The T4-erase edge and the inject predicate cancel each other.**  8 165
   of the ~12.5 k re-draw events erased the iono carry at exactly the epoch
   the inject predicate would have served them — the majority flux never
   received a prior.  The conservative erase choice (attribution cannot
   split level error from carried-iono error) forfeits the design's main
   population.  Any successor must resolve this pair (retap-keep semantics
   for the iono carry at T4, or a T4-attribution split), not merely re-tune.

### §6.2 The re-derivation this falsification writes (the successor card)

The carry's harvest source is the defect: it stashes the SOLVED per-LOS iono,
which is **level-locked** mid-arc (§1) — so the accumulator faithfully
accumulates the displaced consensus and re-asserts it at every event.  The
code channel's absolute evidence never enters the carried value at all.

The successor is the same carry seam fed from a **level-independent,
code-anchored per-LOS observable**: the carrier-smoothed GF-code slant iono
(Hatch-smoothed `GF_code/(α₂−α₁)` with `GF_phase` deltas as the smoother's
carrier, per (station,sat), slip-reset via the existing funnel).  Its error
shrinks as 1/√K toward the code channel's realization ι̃ = ι + d_r,GF − d^s_GF
— the same realization the datum pins anchor — and it is structurally immune
to the leveling carry.  This is `project_global_sh_iono` lever 2
("carrier-smooth the code GF seed") promoted from seed hygiene to the carry's
data authority, and it is the per-LOS analogue of the level card's
"per-epoch data-derived authority" (`QUEUE_principled_replacements.md` §5).
Prediction it must beat: the smoothed-GF authority at the re-level edge
drains the station-common displacement at √K-accumulated code weight — the
quantity neither the white representation nor the solve-fed carry possesses.
Not built this session; filed as the named follow-on with this SPEC as its
derivation.

### §6.3 i2 (scope=always) — PRE-REGISTERED before reading its results

Written while the arm ran, after §6.1: mid-arc lag-1 pinning adds temporal
stiffness to the iono side of the exchange every epoch, so the station-common
BREATHING (common_std 0.8–4.1 m) should damp — but the carry still harvests
the level-locked consensus, so the DISPLACEMENT should not drain.  Predicted:
interaction 3.0–3.9 (no drain), ZWD 2.6–3.1, common_std down vs i1, consumer
≤ i1-class.  If i2 instead collapses (interaction ≤ 1.5), the §6.2 drain
model is wrong and solve-fed accumulation suffices — RESULTS BELOW.

**Measured (`runs/camp60_i2`, 672 540 priors injected — the always-on channel
was fully live): interaction 3.726, ZWD 3.140, consumer 2.61, hourly
2.04→3.29 band.  Squarely inside the no-drain prediction band; no collapse.
The drain model is CONFIRMED by intervention: solve-fed accumulation cannot
drain the displacement at either scope.**  (The breathing-damping
sub-prediction did NOT realize — common_std and the exchange corr are
i1-class; the floor-weighted prior is too weak against the levels mid-arc to
damp the joint wander.  Consumer sits between OFF and i1 — the churn-kill
benefit partially survives.)

### §6.4 Quiet twin + the field smoke

- **`runs/camp60_quiet_i1`** (carry ON, quiet fixture): ZWD settled rms 0.263
  vs 0.229 control (+15 %, concentrated in mid-hours; h5 0.137 vs 0.133 ≈
  neutral; p50 0.025 m), interaction 0.808 vs 0.78, consumer 0.614.
  **Near-neutral with a small mid-hour penalty** — the FA-event injections
  re-assert carried states on a healthy fixture at bounded cost.  Honest gate
  reading: PASS on h5/p50, marginal on rms.
- **Field smoke (relevel knobs, the §8 "field arm owed")** —
  `runs/fld63_relevel_{off,on}`, ext63 1 h leveled posture with
  `carry_invariant: false` (⚠ posture note: the FT63/ext63 lineage arms
  carry_invariant, under which the relevel knobs are structurally inert; the
  field arm therefore rides a DIFFERENT lineage, and pulling carry_invariant
  out drags six dependent knob families — innov gate/calibrate riders, the
  WL-2 lanes, the UWL tier — all disabled in the `_fld63_relevel_*` configs).
  Single knob-set A/B on exe E102C7D7: availability 121/121 both, zero
  withholds, detections unchanged (T0 64/63, T1 37/37), T4 flat (654→677),
  free-epoch latches 203 / inject skips 123 / 29 pends → 3 replaced (2 via
  the v3 GF route — first FIELD applies), **post-fit phase σ̂₀ 2.405 → 1.697
  (−29 %)**.  Every internal instrument neutral-to-better: **field smoke
  PASS**; the ≥4 h escalation is the named follow-on (smoke-first cadence).

### §6.5 i3/i4 (CS-GF code-anchored authority) — the drain model is ALSO falsified

Pre-registered in `_camp60_i3.json` before either arm ran: interaction ≤ 2.0
REQUIRED (0.8–1.5 aspiration), ZWD ≤ 1.5, consumer ≤ 1.9-class, census ≥ 96 %,
CS blend/reset counters live, T4 shrink expected; **interaction > 3 ⇒ the
code-authority drain model is also wrong ⇒ stop, record, no further variants.**

Provenance: exe MD5 6B3935C3… (byte-certified OFF-path, §5), configs
`_camp60_{i3,i4}.json` single-knob-set over the certified OFF path, graders
identical to §6.1 (grade / iono_interaction / slip_census / plateau JSONs in
the run dirs).  ⚠ The first i3/i4 attempt (pre-23:09 exe) was a MISWIRE —
`gf_code_m`/`gf_denom` computed into locals and never assigned, all CS
counters zero, outputs equal to the OFF path; those numbers were never graded
into any record and must never be quoted.  Invariant #4 (counters, not logs)
is what caught it.

| channel | OFF (f2/f4) | i1 solve/relevel | i3 CS-GF/relevel | i4 CS-GF/always |
|---|---|---|---|---|
| interaction rms | 3.568 | 3.800 | **3.592** | **3.181** |
| ZWD settled rms / p50 | 2.845 / 0.578 | 3.099 / 0.660 | 2.852 / 0.583 | **3.656 / 0.748** |
| ZWD hourly | 2.34→2.35 flat | 2.32→2.42 | 2.27 2.22 2.71 2.99 3.13 2.37 | 2.00 1.96 3.03 3.34 3.91 3.70 **ramp** |
| consumer 3D rms / p50 | 4.06→3.26 class | 1.901 / 1.707 | 4.481 / 1.856 | 4.279 / 2.656 |
| T4 arc events | 7 876 | 8 165 | 8 105 | **13 982** |
| census (det+resp / silent) | 96.8 % | 96.9 % | 96.8 % (2340+67 / 79) | 97.1 % (2340+73 / 73) |
| det jump p50 \|Δ\| | 0.122 | 0.094 | 0.140 | 0.143 |
| corr(post,pre) det / T4 | 0.90 / 0.83 | 0.92 / 0.82 | 0.876 / 0.812 | 0.863 / **0.918** |
| clock contrast rms | ~13 | 13.5 | 11.3 | (same class) |

Counters (teardown, both arms live — this is a genuine intervention):
i3 4 893 injected (2 164 floor-clamped) / 3 618 kept through freed slips /
8 105 T4-erased / 1 131 evicted; i4 667 021 injected (453 732 floored) /
3 589 kept / **13 982 T4-erased** / 1 126 evicted; 0 datum refusals both.
CS-GF authority identical across the pair — 677 848 blend steps, 11 149
resets, 1 979 LOS states live at run end — the authority advance is
scope-independent (PASS-A), as designed.  prires family `iono` live (worst
|t| 8.6e6 at a CS-var-floor stash — measurement only, nothing wired to it).

**Verdict: falsified at both scopes.**  Three findings:

1. **The §6.1 structural objection is resolved and it bought nothing.**  The
   T4-erase/inject-predicate cancellation that starved i1's majority flux
   cannot starve CS-GF (the authority persists through every T4 erase and
   re-stashes next epoch; i3 injected the same ~4.9 k events as i1 with the
   authority behind them) — and the plateau is byte-for-byte the OFF value
   (3.592 vs 3.568, ZWD 2.852 vs 2.845).  The re-draw source is irrelevant.
2. **i4 shows conservation, not drainage.**  The always-on pin bought the
   ladder's largest interaction move (−11 %, 3.57 → 3.18) and paid +28 % ZWD
   (2.85 → 3.66, RAMPING through the run — the h5/h6 bins are the worst) and
   a ×1.78 T4 storm (7 876 → 13 982, corr(post,pre) 0.918).  The displaced
   station-common state relocated across the ZWD⇄iono exchange; the per-epoch
   floor-weighted prior cannot beat the levels mid-arc, and its accumulated
   tension surfaces as refloats.  Net harmful.
3. **The i1 consumer win is source-specific and does NOT survive CS-GF**
   (1.90 → 4.48/4.28 rms, excursion-dominated).  Mechanism: the solve-fed
   carry re-asserts the consensus — i.e. wire CONTINUITY — at events; the
   CS-GF authority injects a value that disagrees with the level-locked
   consensus, and the disagreement steps the wire.  Do not quote i1's
   consumer number as a validated benefit of any armed posture; it has no
   surviving carrier.

### §6.6 Closure — what this campaign now excludes, and where the plateau lives

Four falsifying interventions on one certified fixture/exe lineage: solve-fed
× {relevel, always} (§6.1/§6.3) and code-anchored × {relevel, always} (§6.5),
plus the i1 churn kill that measurably reduced re-draw noise while the plateau
rose.  Together they exonerate the EVENT channel entirely: the plateau is not
event-fed and no re-draw-time authority — however good — can drain it.  It is
the stationary equilibrium of the per-epoch ZWD⇄iono⇄level exchange under
mid-arc level lock-in (§1's zero-q transport), which is precisely the
representation defect this card was opened to name.  The accumulator the
record asked for is not wrong to want — it is wrong to attach at events, and
too weak at the var floor to matter mid-arc against phase-precision levels.

Disposition: `network.carry_iono` + `iono_carry_source` stay default-OFF
research knobs (byte-certified OFF; conditional fingerprint); the stop rule
forbids further variants this session, and the queued quiet-twin CS-GF arm is
DROPPED as moot (a neutrality certification of a mechanism no posture will
arm).  The CS-GF kernel itself (`networkIonoCsUpdate` + the RawSlipObs GF
fields) stays: it is level-independent instrumentation with unit-pinned
semantics, and any future per-LOS work wants it.  The surviving next question
belonged to the O2 / `carry_invariant`-lineage card (scope=always on a
representation whose mid-arc transport is NOT the leveling carry) — **DERIVED
AND CLOSED UNBUILT 2026-08-21, `SPEC_o2_carry_invariant_accumulator.md`.  ⚠ Read
its §1 before quoting this section's mechanism forward: on the `carry_invariant`
posture the ZWD carry is INERT (it rides `carry_warm = pl_on && …`) and the
leveling carry is structurally empty, so BOTH ingredients of the ZWD⇄iono⇄level
equilibrium named above are absent there.  The 8-family is closed; the one live
lead is §7 of that card — the measured cross-band residual direction is
common-cycle, not ionospheric.**  Plus the mechanism question §6.5(3) opens —
none of it in this card's scope.  The
session's validated survivors are `relevel_free_epoch` (load-bearing),
bridge v3 (§8.3 — 74 % yield, 0 integer-class wrong), and the §6.4 field-smoke
σ̂₀ −29 % with its ≥4 h escalation owed.
