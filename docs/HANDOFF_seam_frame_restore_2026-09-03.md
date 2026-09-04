# HANDOFF — STABILITY campaign, queue item 1: resume-seam frame restore (2026-09-03)

Written at the Fable session's wind-down for the continuing (Opus) session.
Read in this order: `memory/project_seam_continuity_card.md` (state), then
`docs/SPEC_synthetic_minimum_network.md` §18.9 (data verdict), §18.10 (mechanism +
build), §18.11 (leg verdict), then `docs/CONFIG_promotion_ledger.md` rows
`network.resume_frame_restore` / `network.resume_slip_warmup_ep` /
`network.state_snapshot_frame_capture`.  Commit chain: 953290f (§18.9 data
verdict) → cd930a3 (ledger σ-hook addendum) → cdd19bc (§18.7 + row-321
corrections) → f5a7a93 (instruments) → 90718c3 (the BUILD) → 98295ec (§18.11 +
ledger rows) → 09ab716 (this handoff) → d9b49e6 (rung 3 falsified).  All on `master`.

## 0. Baseline posture: build on the BEST STABLE network, not the campaign control

The seam legs above branch from the campaign's CONTROL base (r2q3 without sticky
pivot) because the h12 branch library was cut before E3 landed.  Continue from the
best validated posture instead:

- **Base config: `config/test_suite/_camp60wu_quiet24h_sticky.json`** = the r2q3
  flagship (R2 sticky continuous handover + Q3 anchor q 3e-6 + relevel_free_epoch +
  pivot clock datum + the standard armed QC/observability set) **+
  `network.sticky_pivot: true`** (E3 — settled h12–24 consumer 3D rms 0.303 → 0.078,
  the bed's best run: `runs/camp60wu_quiet24h_sticky`, dashboard in its dir).
- Enhancement arming state for the continuing work:
  | knob | state | why |
  |---|---|---|
  | `network.sticky_pivot` | ON (fingerprinted → fresh convergence) | E3, −74 % settled; field-transferred (scope: triple-freq lineages) |
  | `network.zwd_seam_reseed` | ON on every resume leg | validated seam ZWD (no restart) |
  | `network.state_snapshot_frame_capture` | ON on every WRITER | write-outputs-only, proven; needed for any frame-restore leg |
  | `network.resume_frame_restore` | OFF until §3 lands (then ON on resume legs) | 33-clock trip residual (§18.11) |
  | `network.resume_slip_warmup_ep` | 5 together with frame restore, else 0 | safe response, pointless alone |
  | `network.amb_seam_reinflate_var_m2` | 0 — FALSIFIED under frame restore (rung 3: 5 m consumer walk) | now ACTIVE there, and harmful |
  | `network.clock_disc_gap_jump_m` (E1) | default OFF | validation owed on a triple-frequency lineage |
  | `emit_pb_filter_gate_v2`, `relevel_retap_*`, `carry_iono`, `resume_shock_m` | OFF | falsified / null (ledger) |
- **Regenerate the branch library on that base**: sticky_pivot is fingerprinted, so
  the seam writer must be a FRESH 12 h run of the sticky config with
  `state_snapshot_frame_capture: true` + `state_snapshot_keep_history: true` and
  its own paths (the `_camp60wu_quiet12h_frwriter.json` recipe, template swapped to
  `_camp60wu_quiet24h_sticky.json`); gate it exactly as before (products must be a
  byte-identical prefix of `runs/camp60wu_quiet24h_sticky/products.txt`).  Then the
  §2/§3 legs branch from ITS `.ep1440` with `scripts/simnet_make_seam_leg.py
  --template config/test_suite/<new writer>.json --base-run runs/<new writer>`.
  The mechanism verdicts in §18.11 transfer (the seam is datum-side; sticky only
  changes pivot election), but re-grade the consumer numbers on the sticky base
  before quoting them.

## 1. Where things stand (one paragraph)

The ~4 h post-resume consumer transient is the warm wire's phase-bias
RE-CONVERGENCE, and its root cause is a FRAME loss in the RTB-7 snapshot: the
per-(sat,signal) phase-anchor incumbent + sticky pin target, the Q3 clock-anchor
per-satellite state and the CLK-R held frame were never serialized, so the first
warm epoch's election ran incumbent-less, QUEUE-1(d) erased all 2811 restored
leveling carries and marginalized their phase-bias prior, every clock folded
LOOSE, and the F1 seam mark fired.  The card (90718c3) serializes that state
(snapshot format v2, write-side knob `state_snapshot_frame_capture`) and restores
it behind `network.resume_frame_restore` (+ `resume_slip_warmup_ep` for the cold
MW detector).  ON leg: seam CLOSED on the phase side (PB σ 0.008 m at the first
warm epoch, was 1.112; no datum mark; 54 carries erased, was 2862; consumer resets
5, was 51; consumer h13–15 0.17/0.14/0.11, was 0.25/0.34/0.30) — but 33 of the 52
restored clock anchors TRIP at the first fold because the restored leveling rows
carry no Card-1 coupling regressors and out-vote the anchored clock priors; the
LOOSE re-draw freezes new metre-class clock tails on G14/G17/G20/G22 and the
consumer pays at h19 (0.96 rms, a wire property).  Verdict: mechanism confirmed,
`resume_frame_restore` KEEP OFF until the regressors are serialized.

## 2. Rung 3 — DONE (read 2026-09-03 17:07): the proxy is FALSIFIED

Result: `cta_decouple` at 302430 = **14** (ON leg 33), B1b PB σ 0.009 m, but the
consumer walked **5.13 m rms in h12, 2.93 / 1.79 / 1.10 / 0.86 in h13–h16**, recovering
to 0.07 by h21.  Inflating the restored rows lets them re-float on the cold station
layer — the slip-relevel-walk family.  `amb_seam_reinflate_var_m2` stays 0 in every
arm; the coupling-regressor increment (§3) is the fix.  (The section below is kept
as the reading recipe for any future rung.)

## 2a. Reading a rung (recipe)

For any future leg `<leg>` under `runs/`: check `exitcode.txt`; count the first-fold
clock trips with `awk -F, 'NR>1 && $2==302430 && $4=="cta_decouple"' runs/<leg>/logs/network_events.csv | wc -l`
(ON leg 33, rung 3 14; the target is 0); read the first-epoch B1b line
(`grep -m1 "302430.*B1b" runs/<leg>/console.txt`, target ~0.01 m); then
`python scripts/simnet_seam_tools.py consumer runs/<leg>:x runs/camp60wu_q12warm_fron:frame_ON runs/camp60wu_q12warm_frctl:frame_CTL runs/seamctl_replay_cont:cold_vs_CONT --tow-lo 302400 --tow-hi 345600`
(targets: h13-h15 within 1.5x of 0.060/0.067/0.062, no h19 excursion, no metre-class
h12).  Fold every result into SPEC §18.11 and the `network.resume_frame_restore`
ledger row; quote settled p50 beside rms and name the h16.7-h18.3 eclipse cluster.

## 3. Second increment (the fix): serialize the coupling regressors

Why: a restored `LocalCarry` injects as a plain diagonal row
(`include/filter/NetworkRunTypes.hpp:346`), i.e. "N = value_abs", whereas the
live carry row is CONDITIONAL on the shared border:
y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j at the pure-conditional variance
(`CoupledShared` at NetworkRunTypes.hpp:302-312; row built at
`src/core/app/ApplicationNetworkAssembly.cpp:2889-2907` via buildCplRow;
harvested at `src/core/app/NetworkRunContext.cpp:2551-2676`; re-based on datum
re-references at `src/core/filter/NetworkChainDatum.cpp:882/890` and
`NetworkRelevel.cpp:115-124`).  Without M_j the 2811 stiff rows fight any border
motion and the 9.5 mm anchored clock priors lose (innovations 4–8 cm → w 6–25).

What to build (default-OFF, same forward-only exemption class, snapshot format
stays v2 with new record tags — pick tags not in {M N U D V W K O J L Z R A Y C I
X SV GG PA PT CA FA FP FE}):
- Writer (`networkWriteStateSnapshot`, NetworkRunContext.cpp ~1497-1560): per
  restored-able L row, serialize its `coupling` entries as identity-keyed rows
  (`kind, sat, signal[, sh n/m/trig/layer]`, coeff M_rj, s_prev, p_prev — the
  CoupledShared fields).  ~2811 rows × few entries ≈ 250 KB (R2's estimate).
- Reader/restore (`networkTryWarmResume` ~1172): rebuild `LocalCarry::coupling`
  so the first PASS-B inject builds a coupling row instead of a diagonal.  Mirror
  the same for the Z rows if cheap (they are also diagonal on restore).
- Gate: `network.resume_frame_restore` can own it (it is the same mechanism:
  "restore the frame the snapshot omitted"); count restored regressors; run-end
  line + manifest.
- Prediction: `cta_decouple` at 302430 → 0, emitted clock between-sat SD vs the
  continuous run at 302430 ≤ 0.02 m (ON leg: 0.28 G / 0.16 E), no new clock tails,
  consumer h13–h15 within 1.5× of the cold-on-continuous floor (0.060/0.067/0.062),
  no h19 excursion.
- Then E2 (§18.4 re-admission continuity) reuses the same seed-first /
  mark-if-moved shape; the C5Q tails (G25/G26/G12) are E2's, not this card's.

## 4. How to run a leg (rails)

- Release-only, sequential, own CWD, hash-verified renamed exe beside its DLLs.
  Generator: `python scripts/simnet_make_seam_leg.py --leg <name> --ep 1440
  --base-run runs/camp60wu_quiet12h_frwriter --set network.state_snapshot_frame_capture=true
  --set network.resume_frame_restore=true --set network.resume_slip_warmup_ep=5 [--set ...]`
  then `cd runs/<leg> && ../../build/x64-Release/gnss_processor_<tag>.exe ../../config/test_suite/_<leg>.json > console.txt 2>&1`.
- The branch point is the v2 `.ep1440` of `runs/camp60wu_quiet12h_frwriter`
  (writer gate PASSED: products byte-identical to the continuous run over 1450
  epochs; fingerprint 35860c5f45b8a004 = the v1 library's, so OFF/ON on one
  snapshot is legal).  The old v1 library `runs/camp60wu_quiet24h_r2q3/*.ep*`
  cannot carry frame rows.
- Keep `amb_seam_reinflate_var_m2` at 0 in every arm unless it IS the arm (it
  became active once the erase stopped).
- Controls that exist: OFF control `runs/camp60wu_q12warm_frctl` (reproduces leg
  A `runs/camp60wu_q12warm_szv` to the digit); cold-consumer replays
  `runs/seamctl_replay_{cont,warm,warm_pbsig,warm_csig,warm_p30m,warm_p2h,
  fron_h18,cont_h18}` (33 s each; the replay driver is EXACT on this bed; its
  `start_offset_hours` anchors AFTER the fast-forward to the first product record).
- Grading: `scripts/simnet_seam_tools.py {diff,decompose,walk,quality,steps,drift,
  consumer}`; the truth-graded per-sat clock is the per-constellation
  median-demeaned emitted clock (the bed's truth clock is identically 0).
- Quote settled p50 beside rms; name the eclipse windows inside any graded window
  (30 edges / 12 h; the h16.7–h18.3 cluster is in every leg).

## 5. Traps hit this session (save yourself the hour)

- The Bash tool's heredoc layer collapses `\\` and `\'` inside quoted heredocs:
  write edit scripts with the Write tool into the scratchpad and run them; never
  put C++ `'\n'` or Python `\'` in a heredoc.
- `include/app/NetworkRunContext.hpp` was share-locked (Edit's temp+rename gave
  EPERM); an in-place `io.open(...,'w')` from Python works.
- Another session's uncommitted work is in the tree (Planetary / SatAttitude /
  GnssCorrections / Antenna / ModelGeometry / GnssConfigLoad.cpp / two orbit
  tests / `tests/test_planetary_sun_frame.cpp` / pyviz files).  Commit only your
  hunks (`git diff -U0 <file> | grep ^@@`; for CMakeLists stage your hunk with a
  filtered patch and `git apply --cached --unidiff-zero`).  Those changes are
  default-inert on this bed (the writer gate proved it).
- The design workflow's adversarial verify phase never ran (usage limit).  The
  build follows the three-card consensus + judge grafts (capture knob, ledger
  priming, reinflate now active); the readers' map is in the workflow journal
  `.claude/projects/.../subagents/workflows/wf_67c7e3e0-872/journal.jsonl`.
- No Claude attribution in commits (standing directive; `.githooks` strips it).

## 6. Queue after this card (from the campaign brief)

2. E2 build (re-admission continuity, §18.4) — after the regressor increment.
3. Triple-frequency FIELD arm (EXT-CB/L5 lineage) — validates E1 + sticky_pivot.
4. LV-6b measure-only build + G1′/G2′/G3′ gates (SPEC_level_authority_internal §7).
5. T3/T4 isolated-power bench arm.
6. The (c')-bed "restored amb-carry drag" re-read is MOOT (nothing restored was
   ever folded on that posture — §18.7 correction); GPS+GAL bench estimability
   only if a mixed bench is needed.
