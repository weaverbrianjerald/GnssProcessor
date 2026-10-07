# F3 field (results): the field F3 ladder and the converging leveling posture

**STATUS**

- **id:** F3 field (results)
- **topic:** network-vehicle
- **status:** closed
- **summary:** F3 is sound on real data but KEEP OFF on the field: P2 FALSIFIED on the shipped chain, PARTIAL on the converging leveling posture (WL12 R 0.562 vs null 0.450).
- **dates in the text:** 2026-09-09 (smokes 17:4x to the second round at 19:20)
- **knobs / posture:** `network.phase_leveling_local_block` (F3); `network.carry_zwd` (arm 0z, KEEP OFF on the shipped vehicle); the converging posture = `network.carry_coupling_gauge_project` plus `phase_datum_eq34: false`, `phase_datum_prioritized`, `phase_datum_handover_continuous`, `phase_anchor_carry_reref`, `phase_datum_min_stations: 2`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/fldf3_ci_zwd`; arms `fldf3_ci`, `fldf3_pl`, `fldf3_on`, `_fldf3_pl2`, `_fldf3_on2` and seven 3 h probes `_fldf3_pl_*`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` §6.15.4

**Outcome.** On the shipped FLD63/EXT63 chain the leveling vehicle diverged (S-basis withholds from epoch 101, ZWD spans of 10² m); F3 fired with 0 fallbacks and mitigated it without a cure, its DD fractions at the null. `carry_zwd` on the shipped vehicle diverged too. Probes found the converging posture: it retires the shipped (I, N, ZWD) trade (DD wander 10.2 → 0.12 cyc) and gives the float consumer 3.8 m against 6.6; F3 on it is PARTIAL, consumer flat. That posture became the vehicle of [card PV](../network-vehicle/PV-vehicle-test.md) and the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) frozen posture (F3 off) until the [float vehicle](../../refound/GRB_golden_rebase_2026-09-22.md) replaced the leveling carry on 2026-09-22.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
### 6.15.4 Results — filled as they land (nothing above is edited)

**Arm 0 `fldf3_ci` — landed 18:03 (exit 0, 1440 epochs, 27 min wall under a third session's concurrent run):**
the same-build control REPRODUCES the 09-02 control on every instrument: L1C as-traced med|f| 0.250 (null 0.246),
R 0.464 (0.472), ref-free gap +0.012, near 19.6 %; WL12 (37,913 paired chords) R 0.487 (0.482), gap +0.006;
arc stability e1/e2/wl MAD 10.24 / 10.26 / 0.56 cyc, cross-band slope 1.004 (p10 0.962, p90 1.048, corr 0.999),
ref-free/raw 0.65 vs null 0.77; iono leg identical (DD-iono MAD 1.25 m, A on iono +1.66 corr 0.90, B +0.09,
baseline split 0.74 → 1.58 m with a flat A slope).  Not byte-identical to the 09-02 run (carry census
marginalizations 18,456 vs 18,535, T1 flags 322 vs 343, consumer path differs) — the IF-TD pool-scale unbias
(§6.14.6) landed between the two builds, as expected; every graded reading is the same to the quoted digit.
Run-end: prires worst |t| 178 (rxcb), block-Baarda 34/1441 (2.4 %), rx-CB 46,426 folded / 41,297 withheld;
T3/T4 0 / 0 (post-fit RAIM inert on this vehicle, as pre-registered).  In-process AR consumer settled (≥ h6)
3D rms 9.6 m / p50 8.6 — the documented pathology, reported not leaned on.  D0 median 1,072 ms (overlapped).

**Arm 1 `fldf3_pl` — landed 18:13 (exit 0; only 1,122 D0 epochs of 1,440 — 209 `gate_withheld`, 165
`phase_heal`, sbc engage/refuse/release 15/10/13; D0 median 289 ms):** the phase-leveling vehicle WITHOUT
F3 is numerically sick on the field.  ZWD (settled, 61 stations): per-station span median **120 m**, max
1,305 m, |rate| p50 8.8 m/h, formal σ 0.019 m — the station-common over-count of §3 at a scale the sim never
showed (sim F1 rms 0.19 m).  Prior-residual worst |t| 1.2e8 (pl/CLEAN), block-Baarda 5,877 flags, T2
within-epoch trigger 259/974 (27 %), T3 43,714/481,017 (9.1 %), T3c 5,087, T4a 10,601 — the live pool
refloating continuously.  DD instruments: at the null on every signal (L1C R 0.465 vs 0.468, gap +0.008;
L2W 0.449 vs 0.450; WL12 0.478 vs 0.480, 28,304 paired chords).  Arc stability WORSE than the control: e1/e2
MAD **24.2 / 24.8 cyc** (p90 141), drift 63 / 67, WL 4.8 cyc (vs 0.56), cross-band slope 1.080 (p10 0.85,
p90 1.28, corr 0.994) — between cycle-common and iono; ref-free/raw 0.38 vs null 0.70 (the reference carries
a third of it).  Iono leg: DD-iono MAD **3.7 m** (3× the control), and the ambiguity↔iono coupling has MOVED
INTO THE IONO-SHAPED CHANNEL — B MAD 16.8 cyc, B on DD-iono slope +0.78 (corr 0.88), A +0.31 (corr 0.44):
on this vehicle the ambiguities absorb iono error the way the model does it (band-shaped), at ~17 cycles.
In-process AR consumer settled 9.9 m (pathology).  **Reading (peer review, 18:18): arm 1 is not degraded, it
is DIVERGED — and that is itself the result.**  The 199 `skip_epoch` withholds are the `NetworkSbasis`
null-space gate reporting "observed 0 (near-)zero eigenvalues" against its declaration (a datum/rank
mismatch: under the leveling vehicle the chain's `sbasis_complete` + eq34 datum pins MORE than it declares),
i.e. a gauge failure, not a noise one; ZWD spans of 10² m are the same tell.  Consequences: (i) the card's
graded delta (arm 2 ↔ arm 1) is F3 on a diverged baseline unless arm 2 itself converges, so arm 2 is read on
its OWN convergence first (withholds, ZWD span, prires |t|, T3/T4 rates) and only then against arm 1;
(ii) arm 1's B-channel reading (17 cyc, iono-shaped) is a property of a diverging solve and is kept OUT of
the mechanism discussion until a converged leveling arm exists; (iii) the vehicle question underneath §6.15's
opening — WHY every field lineage rides carry_invariant — now has a quantitative answer: on this lineage the
diagonal leveling carry does not converge on real data, so item 21's "does F3 help the field" becomes "F3's
vehicle is unreachable on the field until the leveling posture's datum declaration is fixed" — unless arm 2
shows that the divergence WAS the over-count (F3's whitened block is precisely a conditioning fix for the
diagonal rows' B×n_LOS assertion of the station-common directions, which on the sim took the ZWD from 0.42 m
to 0.009 m), in which case F3 is the fix for the vehicle itself.  Structural next checks, not tuning: the two
arms' declared dimension vs observed null-space census and datum-graph census; and whether the knobs the
vehicle switch had to force off (carry_invariant sub-family, EWL/UWL lanes) removed something load-bearing.
⚠ **ZWD trace caveat that also touches the CONTROL:** `network_zwd.csv` `zwd_m` = `x_lin + x_r(zcol)` in
metres with `x_lin ≡ 0` (`NetworkIonoDiag.cpp:137-141`, zcol via `h2local[getTropoIndex()]`), and on the
control arm it ALSO reads unphysically — per-station settled span median 21 m (BSHM00ISR −74 … +20 m) — on a
vehicle where the ZWD is re-estimated per epoch (not carried).  Either the shipped field posture's per-epoch
ZWD really is a 10-m-class sink (a large finding about the field products; the E3 pair never traced ZWD) or
the trace's local-column mapping differs by vehicle (the same `h2local` question as the DD trace; the sim F3
arm's ZWD grade — 0.009 m rms, |err|/σ 0.94 — came through this trace, so the mapping is right on the
leveling vehicle).  **Checked (18:25, on disk):** the ZWD values do NOT coincide with any of the same
station-epoch's iono x_r values (0 of 20,000 rows beyond chance on ci, pl and the sim), so the ZWD column is
not landing in the iono block; and the σ beside the value is scale-consistent on the control — `std_m` p50
0.57 m (p10 0.11, p90 1.0) beside |zwd| p50 1.3 m — i.e. the shipped posture's per-epoch ZWD correction is
genuinely a weakly-determined metre-class state (not carried, not emitted; it wanders ~40σ over 6 h, so the
σ is not honest either), while on the diverged pl arm the σ is 0.016 m beside |zwd| 1.7 m — the diagonal
leveling vehicle's over-confidence (§3's station-common over-count) in its rawest form; the sim F3 arm reads
0.12 m ± 0.007 (honest).  A shared-`h2local` mapping miss is therefore NOT supported for the ZWD column;
the ambiguity column's split remains untested (the DD-trace column addition).
**The fourth member, PRE-REGISTERED (18:30, before running; peer derivation):** the control's per-epoch ZWD is
a metre-class sink, so DD troposphere on this posture is METRE-scale and band-common in metres — the shape of
the leftover.  With a band-common term T beside ΔI and a cycle-common A, the two-band system λ₁A − ΔI + T = 0,
λ₂A − 1.6469·ΔI + T = 0 closes EXACTLY at A = 2.283·ΔI_cyc, T = −1.283·ΔI (at the control's ΔI MAD 1.25 m that
is 1.6 m of DD tropo per arc, reachable from a 1.3 m per-station |zwd| through four mapping terms).  The
"no physical DD term closes it" premise is RETRACTED (it assumed a centimetre tropo).  Predictions, testable on
disk (`tropo_arc_wander.py`: DD_T = Z_a(mf_a^i − mf_a^ref) − Z_b(mf_b^i − mf_b^ref), mf = 1/sin el, ZWD and
elevations from the run's own traces): (i) the DD-ZWD deviation regressed on the DD-iono deviation has slope
≈ −1.28 with high |corr|; (ii) the joint fit e₁ = a·ΔI_cyc + t·ΔT_cyc raises a from the attenuated 1.7 toward
2.28 and puts t near −1.  If both hold, §6.15.3a's cycle-common channel is the PROJECTION onto the ambiguity
block of (I, N, ZWD) moving together along a weakly-determined direction of this posture — consistent with a
1.6 cm fit, i.e. the estimability reading with its fourth member; and independently of F3, the shipped field
posture's tropo state is a metre-class per-epoch sink with a σ that does not know it.
**RESULT (18:33, field control, 584 arcs, `tropo_arc_wander.py`):** DD-tropo arc MAD **1.22 m** — the same
scale as the DD-iono's 1.25 m.  (i) DD-tropo deviation on DD-iono deviation: slope p50 **−0.87** (p10 −1.49,
p90 0.00), corr **−0.85** — the predicted anti-correlation, at ~70 % of the exact −1.283 with a crude 1/sin el
mapping and attenuation.  (ii) joint fit e₁ = a·ΔI_cyc + t·ΔT_cyc: **a = +0.73** (p10 +0.08, p90 +1.09),
**t = −1.17** (p10 −1.68, p90 −0.89) — i.e. ≈ (+1, −1), which is the band-1 phase equation itself
(λ₁ΔN₁ = ΔI − ΔT).  ⚠ Prediction (ii)'s "a → 2.28" was mis-stated: 2.28 is the SINGLE-regressor coefficient
when T is exactly −1.283·ΔI (the measured single-regressor 1.72 is its attenuated version); in the joint fit
the structural coefficients are (+1, −1), and that is what was measured.  So the band-1 budget CLOSES with
the ZWD as the fourth member.  ⚠ Read the (+1, −1) correctly (peer): it is the band-1 phase equation, which
the traced states MUST satisfy if they are what the solve fit to 1.6 cm — so it is not evidence for the
near-null direction (that would be circular); what it IS is the RECONSTRUCTION CHECK marked OWED above, done
in aggregate and PASSED: the traced `x_lin + x_r` pairs satisfy the observation model, branch (b) is closed
EMPIRICALLY (not only by source read), and the 0.75 m gap was an artifact of omitting T.  The evidence for
the near-null direction is the COVARIANCE STRUCTURE: three states that should be small and near-independent
are metre-class (ΔI 1.25 m, ΔT 1.22 m, A ~10 cyc), strongly mutually correlated (tropo↔iono −0.85), and
carry a σ that does not know it (40σ wander).  On that evidence the cycle-common channel is the projection
onto the ambiguity block of **(I, N₁, N₂, ZWD) moving together along a weakly-determined direction of the
shipped field posture** — per-LOS iono (SH-pinned), per-epoch un-carried ZWD (a metre-class sink), and per-station
ambiguities trade against each other while the phase fit stays at 1.6 cm.  The DD "ambiguity" of this
posture is therefore not an estimable-invariant quantity in practice; its 10-cycle-per-hour wander is the
tropo–iono trade riding through it.  On the sim golden the ZWD is CARRIED (honest σ, F3-whitened) and e₁ does
not follow I (slope 0.001) — the same estimator with the fourth member pinned.  Fix direction (not built
here): pin the fourth member — carry the ZWD on the field vehicle (B9 `zwd` carry with a physical q) — and
re-read the DD wander; a single-knob arm on the SHIPPED vehicle, independent of F3.
**Arm 0z `fldf3_ci_zwd` — PRE-REGISTERED 18:40 (queued behind the probes; smoke first):** the control +
`network.carry_zwd: true` (B9 per-station ZWD random-walk carry at its default q; the chain never set it —
`carried` = 0 on every row of every arm so far, and the sim golden does not carry it either but has a FREE
per-LOS iono and F3, which is why its per-epoch ZWD is honest at 9 mm).  Smoke gate: `network_zwd.csv`
`carried` = 1 on warm epochs, else the carry is inert on this vehicle (the inject sits under `carry_warm`)
and the arm is not run.  Predictions if it injects: ZWD per-station settled span < 0.5 m with an honest σ
(≥ 1 cm); DD-ambiguity arc wander e₁ MAD from 10.2 cyc to ≤ 2; the A-on-iono co-motion slope from +1.7 toward
0 (the tropo–iono trade loses its free member); DD fractions NOT predicted to leave the null; post-fit phase
σ̂₀ unchanged (~1.6 cm).  Falsifier: ZWD pinned but the DD wander and the co-motion unchanged ⇒ the ZWD was
a passenger, not the free member, and the weakly-determined direction has another member.
**RESULT (19:00, `runs/fldf3_ci_zwd`, 12 h, exit 0):** the smoke gate PASSED — the B9 carry injects on the
shipped vehicle (`carried` = 1 on 2,012 of 2,439 smoke rows; the peer's "inert" reading was wrong, as the
golden's `phase_leveling: true` predicted) — and the 12 h arm DIVERGED, a third outcome neither branch
named: 1,322 solved of 1,440 with **118 S-basis withholds on the SHIPPED vehicle** (the control had 0),
ZWD per-station span **54 m** (control 21 — pinning made the sink worse, σ 0.057), prires worst |t| 2.7e7 on
the ZWD prior itself, DD wander e1 MAD **39 cyc** (control 10), DD-iono MAD 3.8 m (control 1.25), and an
in-process consumer that left the building (h4 220 m).  The joint fit still reads the band-1 equation
(a +1.00, t −0.99) — the traces stay faithful while the solution goes off the rails.  Reading: the ZWD IS a
member of the weakly-determined direction — constraining it with a diagonal random-walk prior does not pin
the direction, it moves the tension onto the other members and closes the declared null direction (the same
gate the leveling arms trip); and a diagonal ZWD carry is §3's over-count in its purest form (a station-common
direction asserted n_LOS-fold per epoch, σ collapsing onto a metre-class first estimate — the sim's "ZWD
formal 0.005 m vs 0.35 m actual" of §6.8.4, on real data).  Standing invariant #1 applies as the peer warned:
a stiff time-linked ZWD with no wired response.  And it closes the loop with probe 4: the ledger's
`carry_conditional_coupling` row records that the Card-1 coupling rows fold NOTHING under carry_invariant unless
`carry_zwd` is on — arming the ZWD carry made those rows LIVE on the shipped vehicle, and the S-basis gate
tripped 118 times on a vehicle that never trips it: the conditional coupling rows are what close the declared
null direction on this chain, on both vehicles.  **`network.carry_zwd` on the shipped field vehicle: KEEP OFF
(12 h: divergence).**  The fourth member is real; the fix for it is not a carry, it is whatever makes the
(I, N, ZWD) direction observable per epoch (F3-class whitening of the station block INCLUDING the ZWD column,
or an honest external ZTD) — a design card, not a knob.

**Arm 2 `fldf3_on` — landed 18:25 (exit 0; 956 solved epochs of 1,440: 223 `gate_withheld` by the same S-basis
null-space gate from the SAME onset epoch 101, plus 261 gate-ok-but-unsolved; withholds persist all day, h4–h11
49–75/h; D0 median 388 ms vs arm 1's 289 = +34 %, overlapped).**  P1 PASSES — F3 fires and stays conditioned
all day: 86,344 blocks whitened, 991,230 rows, 60,458 columns marginalized at inject, 2,813 pairs zeroed
(harvest-stamp mismatch), **0 refused, 0 factorization fallbacks**, block dims 10–24 (max 21), within-epoch
refloat re-whiten 9,924 ok / 0 failed.  But the vehicle does not converge under it either: ZWD per-station
settled span median **55 m** (arm 1 120, control 21), σ 0.043 m (over-confident); prires worst |t| 1.5e8; the live
pool refloats continuously — T2 646/950 (68 %), T3 **80,222/403,728 (19.9 %)**, T3c 12,935/24,832 (52 %) — worse
than arm 1's 9 % (the §7 item 4 population: F3's dense L⁻¹ rows admitted to the pool at unit variance);
T4a 10,601 → 3,618 (better).  **P2 FALSIFIED — at the null on every signal:** L1C as-traced med|f| 0.246
(null 0.247), R 0.458 (0.466), gap +0.009; L2W R 0.442 (0.449); WL12 (24,269 paired chords) R 0.485 (0.476),
near 19.8 %, gap +0.003 — the WL null bounds the residual at ≳ 3 L1 cycles, on a NON-converged arm (the
pre-registered ambiguity).  Arc stability: e1/e2 MAD **6.95 / 6.97 cyc** (control 10.2, arm 1 24.2), WL 0.63,
cross-band slope **1.003** (p10 0.83, p90 1.19, corr 0.998) — cycle-common again; ref-free 2.98 cyc (gap −0.19).
Iono leg: DD-iono MAD **0.77 m** (control 1.25, arm 1 3.7), single-LOS 0.82; A on iono +1.40 (corr 0.83), B +0.20
(corr 0.49), baseline split 0.35 → 0.87 m with A's slope flat +1.3…+1.7 — the same (I, N, ZWD) trade as the
control, at ~60 % of its amplitude.  In-process AR consumer settled 7.9 m (pathology).

**Float-only cold-consumer replays (WTZZ00DEU hold-out, truth position, settled ≥ h6, 3D rms / p50):**
control wire **6.56 / 5.83 m**, arm 1 wire 4.37 / 2.13, arm 2 wire **2.90 / 1.62** — P5 PASSES (arm 2 ≤ arm 1)
and the shipped control's wire is the WORST of the three for a float consumer, i.e. every field wire here is
metre-class (the sim's cold-on-continuous floor is 0.06 m) and the (I, N, ZWD) trade is priced into the products.

**VERDICT of the ladder (18:35).**  (1) The phase-leveling vehicle does NOT converge on the FLD63/EXT63 field
chain: both leveling arms trip the NetworkSbasis null-space gate ("observed 0 (near-)zero eigenvalues" vs
declared 1) from epoch 101 onward and carry a 10²-m ZWD sink — a datum/rank failure of a posture (eq34 chain
datum + SBC + SH-pinned iono + leveling) that was never validated anywhere; the sim golden's leveling vehicle
runs the prioritized pivot phase datum with continuous handover.  (2) F3 itself is sound on the field — fires,
conditioned, 0 fallbacks — and MITIGATES the vehicle's divergence on every scale-bearing instrument (ZWD sink
120 → 55 m, DD-iono 3.7 → 0.77 m, DD-ambiguity wander 24 → 7 cyc, T4a 10.6k → 3.6k, float consumer 4.4 → 2.9 m)
but does not cure it, and its pre-registered headline — lifting the field DD fractions off uniform — is
FALSIFIED on this chain (at the null on L1C, L2W and WL12), with the confound that the arm it was read on
never converged.  (3) The field control's DD "ambiguities" are not estimable-invariant in practice: (I, N₁, N₂,
ZWD) co-move along a weakly-determined direction of the shipped posture (§6.15.3a item 5), and that structure is
present on all three arms.  Ledger: `phase_leveling_local_block` stays RECOMMEND_SCOPED on its sim regime, with
a FIELD entry "KEEP OFF on the field until a leveling posture converges there" — item 21 is re-cut from "does F3
help the field" to "make the leveling vehicle converge on the field (datum declaration), then re-run F3"; the
pre-registered probes `_fldf3_pl_pdp3h` (sim golden's phase-datum family) and `_fldf3_pl_nosbc3h` (SBC off) and
the shipped-vehicle arm `_fldf3_ci_zwd` (pin the fourth member) are queued behind this ladder.

**Two record notes (peer review, 18:45).**  (1) *The tension in the KEEP OFF.*  The F3 arm gave the BEST
consumer wire of the three (float cold consumer 2.9 m vs the shipped control's 6.6 m) and the row still says
KEEP OFF: the verdict is decided on PRODUCER-INTERNAL convergence — 223 withheld epochs, a 55 m ZWD sink,
prires |t| 1.5e8, a 20 % T3 rate — because a producer that withholds a sixth of its epochs is not shippable
whatever its emitted epochs do for a consumer; the consumer replay moves the other way and both facts are
real.  (2) *"Every field wire metre-class" retires the DD-uniformity question as an independent mystery.*
6.6 m of float-consumer rms is ~35 L1 cycles, and the producer's own DD arc wander is ~10 cycles (1.9 m) per
hour; DD errors at that scale wrap to uniform NECESSARILY, so the at-null L1C/L2W/WL12 fractions on every arm
are what a wire of that quality must produce, not a separate fact about integer content.  Three numbers, one
story: wire 6.6 m, DD wander 10 cyc, fractions at the null.  It also validates the on-the-fly WL instrument:
its truth-free lower bound (|e₁| ≳ 3.5 cyc from the WL null) sits below the ~10–35 the other instruments
read — consistent and conservative, as a lower bound should be.  **Scoping consequence for whatever follows
item 21:** the integer question cannot be answered on this bed while the wire is metre-class — no refinement
of a wrapped-fraction instrument reaches it, the errors are two orders above the regime where the statistic
carries information — so getting the DOY-129 field wire to sub-decimetre (the (I, N, ZWD) trade and the
leveling-vehicle datum are the two named levers) is UPSTREAM of every integer claim, and outranks the F3
question that started this section.

**Divergence probes (3 h, same exe; pre-registered; grading: S-basis withhold count, h1–h3 ZWD span, prires
|t|, T2/T3/T4 rates, arc stability at settle 1 h).**  Probe 1 `_fldf3_pl_pdp3h` — the sim golden's phase-datum
FAMILY (prioritized pivot datum + continuous handover + anchor carry re-ref + min_stations 2) instead of the
chain's eq34: **still withholds — 84 of 360 epochs, same onset TOW 262230 ("observed 0 (near-)zero" ×68,
"observed 2" ×16), prires |t| 1.5e8**, h1–h3 ZWD span p50 1.7 m (σ 0.010).  The phase-datum family is
EXONERATED as the cause of the WITHHOLDS — but not of the wander: on the same 3 h window the phase-datum
family swap takes the DD arc wander from arm 1's e1 MAD 24.2 cyc / WL 4.7 (491 arcs) to **0.068 / 0.138 cyc on
the 45 arcs that survive** (a selection effect is possible — far fewer chords reach 30 epochs) and the h1–h3
ZWD span from 14.4 m to 1.7 m, so the eq34 chain datum + leveling pairing owns most of arm 1's wander even
though the withholds are unchanged.  Probe 2 `_fldf3_pl_nosbc3h` (`sbasis_complete` off): **identical to arm
1's first 3 h to the printed digit** (82 withheld, ZWD span 14.425 m, σ 0.0153) — SBC never engaged in that
window (arm 1 logged 15 engagements over 12 h) and is not the cause.  Queued (18:50, pre-registered the same way, one family each): probe 3 `_fldf3_pl_nosh3h` —
`estimate_iono_sh: false`, a FREE per-LOS iono as on the sim (the leveling vehicle was only ever validated
with free iono; the SH pin plus absolute leveling priors may close the declared null direction); probe 4
`_fldf3_pl_nocpl3h` — the Card-1 conditional coupling rows off (leveling rows as plain diagonal priors);
probe 5 `_fldf3_pl_vf3h` — `phase_leveling_var_floor_m2: 0.01` (a GAUGE probe: if over-stiff absolute
ambiguity priors are what pins the declared null direction, loosening them should let the gate see it).
Working hypothesis being tested, not asserted: the leveling carry injects ABSOLUTE per-station ambiguity
priors, which are not datum-invariant, so with the chain's declared null-space dimension of 1 they can pin
the last gauge direction (observed 0 ⇒ withhold) — a declaration defect of the leveling posture on this
chain rather than a numerical one.  The ZWD-carry arm (`_fldf3_ci_zwd`) is re-queued behind these probes.
**Probes 3–5 (read 18:50):** probe 3 (SH iono OFF) — 82 withheld, same gate, ZWD span 12 m: SH exonerated.
Probe 5 (leveling var floor 0.01 m²) — 82 withheld, ZWD 10.5 m, prires |t| 6e5 (from 1e8): stiffness is not
the cause.  **Probe 4 (Card-1 conditional coupling OFF): the "observed 0" withholds VANISH (0 of 360)** — and
96 epochs fail to solve instead (gate ok, unsolved), ZWD 15.9 m, 6 surviving arcs: the CONDITIONAL COUPLING
ROWS are what closes the declared null direction on this chain, and without them the leveling vehicle cannot
solve at all.  The sim golden runs the same rows without tripping the gate — and it arms
`network.carry_coupling_gauge_project: true` (project the coupling regressors out of the gauge directions),
which the field chain does not.  **Probe 6 `_fldf3_pl_gp3h` PRE-REGISTERED (18:55): arm 1 +
`carry_coupling_gauge_project: true` — withholds 82 → ~0 with the solve intact (no unsolved epochs beyond arm
1's one), h1–h3 ZWD span below 14.4 m; persisting withholds ⇒ the rows pin the gauge through a channel the
projection does not cover.**
**Probe 6 read (19:03): the withholds are GONE — 0 of 360 (arm 1: 82), 350 solved + 10 unsolved — so the
Card-1 coupling rows' datum-gauge projection (Card 1f, "each (constellation, kind) coefficient sum of a
truncated row held at its harvest value") is exactly what keeps those rows off the declared null direction,
as on the sim golden; but the vehicle still DIVERGES on this chain: h1–h3 ZWD span p50 21.6 m (arm 1 14.4),
prires |t| 6e7, DD wander e1 MAD 50 cyc (14 arcs).  Probe 1 had the mirror image (wander 24 → 0.07 cyc and ZWD
1.7 m with the withholds unchanged).  **Probe 7 `_fldf3_pl_gppdp3h` PRE-REGISTERED (19:05): both together —
gauge projection + the sim golden's phase-datum family (prioritized pivot datum, continuous handover, anchor
carry re-ref, min_stations 2) = the sim golden's leveling posture transplanted onto the field chain.  PASS =
withholds ≤ 5/360 with ≤ 10 unsolved, h1–h3 ZWD span p50 < 2 m with σ ≥ 5 mm, DD arc wander e1 MAD < 1 cyc
on ≥ 40 arcs ⇒ the converging field leveling posture exists and the F3 12 h arm is re-run on it (with its
own no-F3 control); FAIL on any leg ⇒ the two defects are not additive and the leveling vehicle needs a
design card before F3 can be read on the field.**
**Probe 7 read (19:10): PASS on every gate — 360/360 solved, 0 withholds, 0 unsolved; h1–h3 ZWD span p50 0.49 m
(p90 3.5, max 9.1) with σ 8 mm; DD arc wander e1 MAD 0.255 cyc on 59 arcs (54 % < 0.3, a 30 % tail > 1 cyc;
cross-band slope −0.87, corr −0.91 — band-ANTI-correlated sub-cycle wander, i.e. a GF-shaped residual, noted not
read); prires worst |t| still 2.5e8; DD fractions at the null as expected without F3.  So the converging field
leveling posture is the sim golden's: `carry_coupling_gauge_project` + the prioritized phase-datum family
(`phase_datum_eq34: false`, `phase_datum_prioritized`, `phase_datum_handover_continuous`,
`phase_anchor_carry_reref`, `phase_datum_min_stations: 2`).  **Second round PRE-REGISTERED (configs written
19:08, before probe 7 was read): `_fldf3_pl2` (that posture, F3 off) and `_fldf3_on2` (+ F3), 12 h fresh,
same exe, float replays of both wires; §6.15.3 P1–P7 apply unchanged with on2 ↔ pl2 as the graded delta.**

**SECOND ROUND — landed 19:20 (both arms 1,440/1,440 solved, 0 withholds, 0 unsolved; ~8 min wall each).**

| | pl2 (converging vehicle) | on2 (+ F3) | control ci (shipped) |
|---|---|---|---|
| ZWD per-station span p50 (σ) | 0.80 m (8 mm) | 1.20 m (9 mm) | 21 m (0.57 m) |
| DD-tropo arc MAD | **0.07 m** | 0.08 m | 1.22 m |
| DD-iono arc MAD | 0.56 m | 0.45 m | 1.25 m |
| e₁ on DD-iono slope (corr) | **+0.03 (0.49)**, A −0.000 | +0.09 (0.46), A +0.001 | +1.72 (0.93) |
| DD arc wander e₁ MAD / WL | **0.12 / 0.12 cyc** (ref-free 0.065) | 0.71 / 0.19 (ref-free 0.35) | 10.2 / 0.56 |
| cross-band slope | 1.07 (p10 −1.27, p90 1.37) | 1.17 (0.86–1.34), iono-shaped | 1.004 |
| DD fractions L1C: R (null), gap | 0.454 (0.449), −0.000 | 0.468 (0.449), −0.000 | 0.464 (0.472), +0.012 |
| L2W: R (null) | 0.430 (0.433) | 0.444 (0.433) | — |
| **WL12: R (null), near, ref-free gap** | 0.468 (0.450), 22.8 %, +0.004 | **0.562 (0.450), 24.3 %, −0.028** | 0.487 (0.482), 21.4 %, +0.006 |
| prires worst \|t\| / T3 / T4a | 2.5e8 / 3.4 % / 0.31 % | 2.9e8 / 2.3 % / 0.18 % | 178 / inert / inert |
| block-Baarda block-only | 0.8 % | 5.4 % | 2.4 % |
| float cold consumer, settled 3D rms / p50 | 3.77 / 3.18 m | 3.99 / 3.22 m | 6.56 / 5.83 m |
| D0 median | 265 ms | 268 ms | 1,072 ms |

**Readings.**  (1) *The converging posture closes the weakly-determined direction on the field.*  On pl2 the
DD-tropo wander is 0.07 m (control 1.22), the ambiguity↔iono co-motion is gone (slope +0.03, A −0.000 — the sim
F3 arm's signature), and the per-band DD wander is 0.12 cyc (control 10.2) — the (I, N, ZWD) trade of
§6.15.3a is a property of the SHIPPED posture (eq34 datum, no coupling gauge projection, per-epoch ZWD as a
sink, invariant carry), not of the field data.  Its float consumer is 3.8 m against the shipped wire's 6.6 m
— better, and still metre-class.  (2) *F3 on it — PARTIAL, the pre-registered intermediate branch.*  P1 passes
(87,140 blocks, 0 fallbacks, 0 refused; +1.4 % D0, P7 passes).  **WL12 leaves the uniform null for the first
time on the field** — R 0.562 vs 0.450 (+0.112), near-integer 24.3 %, ref-free gap −0.028, closure 0.143 vs
0.174 — while L1C and L2W stay at it (R − null +0.019 / +0.011, below the pre-registered +0.05): the L1 errors
are multi-cycle-but-bounded and F3's effect is read on the WL, as §6.15.2 said it would be.  The residual
per-band wander under F3 is LARGER than pl2's (0.71 vs 0.12 cyc) but iono-shaped (cross-band 1.17, the sim
F3 arm's 1.26) — the honest per-LOS floor family, not the frozen-level family.  Consumer flat (3.99 vs 3.77,
P5 not met), T3/T4a lower, block-Baarda block-only higher (5.4 %).  (3) *Verdict, revised.*  The F3 knob is
SOUND on real data and gives the first non-uniform WL fractions the field has produced, but it does not
change the wire (metre-class on every arm) and does not lift L1/L2; ledger stays KEEP OFF on field lineages
as a promotion decision (no consumer win, prires |t| ~1e8 on both arms of the converging posture), with the
field regime now DEFINED: the converging leveling posture = `carry_coupling_gauge_project` + the prioritized
phase-datum family.  **The field lever this ladder actually found is that posture, not F3:** it retires the
shipped posture's (I, N, ZWD) trade and halves the float-consumer error (6.6 → 3.8 m) with the invariant carry
replaced by the leveling carry — a vehicle question for the field goldens, owed its own card (the invariant
carry's RAIM pool re-host, the lanes and the E2/E3 machinery all assume the shipped vehicle).  What is still
open on the converging posture: the prires |t| 1e8 rows (over-confident priors on both arms), the 23 % of arcs
with > 1 cyc wander on pl2 (a tail population), the block-Baarda rise under F3, and the metre-class wire itself
(§6.15.4's scoping note stands: sub-decimetre first, integers after).

**Smokes (20 epochs each, 2026-09-09 17:4x, before the 12 h arms):** all three exit 0.  Arm 2's F3 line at 20
epochs: 779 blocks whitened, 9,719 rows emitted, 116 columns marginalized at inject, 28 pairs zeroed
(harvest-stamp mismatch), 0 refused, 0 factorization fallbacks, block-dim histogram <10: 136 / 10–24: 643
(max 18) — the GPS-dual block sizes predicted in P1.  No ladder knob reads INERT.  Loader lines that DO change
with the vehicle and are therefore part of the arm-1↔arm-0 reading: `state_snapshot_carry` disabled (needs
carry_invariant), `emit_pb_filter` INERT without carry_invariant (the v1 PB emit filter is off on the leveling
wires — the consumer sees unfiltered PB records), `mw_wl.bridge_sample_debias` INERT without `ewl_enable`; the
post-fit RAIM pool goes LIVE (T3 5,838 opportunities / 91 flags on arm 1 vs 0 / 0 on arm 0 over the 20 epochs),
and on arm 2 it flags 8× more cells than arm 1 in ignition (733 / 5,838; T3c consensus 125 vs 5) — the
§7 item 4 population (F3's dense L⁻¹ rows sit in the T3/T4 pool at unit variance, admitted by the |A_r| > 0.5
test); to be read settled, not in ignition.  Cost: the leveling arms run at ~300 ms/epoch against the control's
~800–1,100 (no invariant DD carry blob), so P7 is read as arm 2 vs arm 1 only.

---

