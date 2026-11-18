# NRF part 16: the guard's release form, the held re-host by remaining life, the extra-band delay in time (sections 115 onwards of card NRF)

**STATUS**

- **id:** NRF-part16
- **topic:** network-vehicle
- **status:** open
- **summary:** Sections 115 onwards of card NRF (2026-11-18), continued from [part 15](NRF_network_ring_fixing_2026-11-14_part15.md); the managing session's queue items N4, N2 and N6, built default off in one engine (exe nrf45) and pre-registered here. Section 115: the release form of the near-null guard (`network.ring_fix.nn_guard_release`: a flagged chord is released, never moved) on the guard's five simulator beds with section 108's truth bars. Section 116: the held-first walk by remaining life (`network.ring_fix.held_first_by_life`) on section 106.5's bed, where a setting satellite was re-hosted three times onto ending arcs, and two more beds. Section 117: the receiver extra-band delay's own process noise (`network.rx_extra_band_delay_q`) in place of the code down-weight of section 79.4, on the simulator and on DOY 144 from both datum receivers: a read only. Section 112.5: the engine build's stream puts E11 and E14 at the wrong combined-signal wide-lane level for combined-mode users (the cause of ARHT00ATA's wrong E14 holds). Section 112.6: the trace of the managing session's hypothesis: 74 % of the two-run audit's failing checks are pilot links of satellites first held at combined-mode receivers, and E11's combined-mode code column departs 0.2 cycle in the wide-lane direction with a formal sigma of 1 cm. Section 118 pre-registers the codes-shared form of the table (`network.phase_family_codes`). Section 119: the stop at the author's instruction; what ran and what did not.
- **source:** [part 15](NRF_network_ring_fixing_2026-11-14_part15.md); the managing session's queue of 2026-11-17 21:58 (items N2, N4 and N6); sections 79.4, 106.5, 108 and 108.1.

**Outcome.** The night ended at the author's instruction before these builds were graded. The trace of section 112.6 shows why the single Galileo network with the codes kept apart fails where run A, with them shared, did not: the failures sit on pilot links of satellites whose level the combined-mode receivers set first, and one satellite's combined-mode code column carries a wrong value for hours; the same stream puts two satellites at the wrong wide-lane level for combined-mode users. The codes-shared form is built and committed unrun; the guard's release form, the re-host by remaining life and the extra-band delay noise are built, smoked and mostly unrun.

### 115. 2026-11-18: the release form of the near-null guard (the managing session's queue item N4; pre-registered)

**Why.** Section 108.1: the guard's move made 12 right chords wrong, because a constant code offset of a link and a one-step error share the signature in the link's code residuals. A release never moves an integer: a flagged chord's hold is dropped and the ring fix may decide it again from its float. Its cost is right chords released; its gain, real steps released.

**Build.** `network.ring_fix.nn_guard_release` (default off, byte-identical; read only with `nn_guard`): a chord the guard flags is released at its first flag (the release path of a second flag: `monitor_trip`, a RINGFIX `nn_release` event, the `nn_releases` counter), in place of the move by −k (3, 4). The arming line names the form.

**Arms** (exe nrf45, 6 h from 00:30:15 on `mode1_r1`'s configuration): `m1_nr`, `d1_nr`, `d31_nr`, `h1_nr` and `h40_nr`, the guard's arms of section 108.1 with the release form; `h40_off45` (guard off) for S1. The controls are the guard-off runs of section 108.1 (`m1_off37`, `d1_off`, `d31_off`, `h1_off`, `h40_off`, exe nrf37), which S1 ties to this exe.

**Bars** (section 108's truth bars for a release):
- S1. Off: `h40_off45` reproduces `h40_off` in `products.txt` and the ring-fix, chain-edge and event traces.
- S2. The clean bed: no release by the guard; 0 wrong held link-epochs.
- S3. The code-error beds (d1, d31, h1, h40): the truth audit's wrong held link-epochs not above the guard-off run's; the frame-free count (each family, summed over the sampled epochs, fixed hash seed) not above the guard-off run's; every epoch emitted.
- S4. The cost: on every bed the Galileo held link-epochs at least 98 % of the guard-off run's.

Every released chord is listed against the truth at its release. Observed arms (24 h, as section 108's O1 to O4) only if S1 to S4 pass.

### 116. 2026-11-18: the held-first walk by remaining life (the managing session's queue item N2; pre-registered)

**Why.** Section 106.5: on `simb_mode1z` the single network's E02, the Galileo clock datum's satellite and setting, lost its host when the joint root's link ended in hour 5 and was re-hosted three times in six minutes onto held links whose arcs were ending (S56000SIM, S40000SIM, S29000SIM). Each re-host re-pinned its level (phase-bias steps over 2 cm: 39 against 15), and the settled rule dropped the satellite during the churn (flagged at 67 of hour 5's 120 epochs against 110). The held-first walk takes the first held arc it meets, in the order of attachment, whatever that arc's remaining life.

**Build.** `network.ring_fix.held_first_by_life` (default off, byte-identical; read only with `held_first`): when the walk takes a held arc to attach a vertex, it takes, over every attached vertex, the held arc of the most remaining life: the satellite's elevation at the station plus 90 degrees while rising, a slipped arc last (the score of the longevity election, `chainArcLife`, without that election, which stays KEEP_OFF because it reorders every new edge); the walk's order on a tie. Unit cases `HeldFirstByLife` (4).

**Arms** (exe nrf45, 6 h from 00:30:15): `u2_zl` (`u2_z`'s configuration, section 106.5's bed) against `u2_z45` (the same exe, off); `m1_l` and `h40_l` (the clean and the 40 % code-error beds) against `m1_off45` and `h40_off45`.

**Bars.**
- H1. Off: `u2_z45` reproduces `u2_z` (exe nrf37) and `m1_off45` reproduces `m1_off37` in `products.txt` and the ring-fix and chain-edge traces.
- H2. Section 106.5's event, counted by `sat_host_hour.py` (the station of the tree edge whose child is E02, per band; the epochs at which every P record of E02 carries signal_integer 1): over hours 5 and 6 of `u2_zl`, E02's host changes on each band fewer than on `u2_z45` (`u2_z`: 3 on each, at 4.12, 5.36 and 5.65 h); E02's settled epochs in hours 5 and 6 and the held-out user's resets reported. Amended before any result: the ring-fix trace of `u2_z` shows the first re-host (4.12 h, onto S11000SIM) on a link not held (refused the epoch before), which no held-first rule reaches, and the second (5.36 h) onto S29000SIM while S37000SIM, its host from 5.65 h, was held as well; so the rule can remove the third change, not the first, and the bar on hour 5's settled flags written first (100 or more of 120) is withdrawn as out of its reach. A rule for new edges at a re-host is the next form if H2 passes and the first re-host's churn remains.
- H3. No harm: on each bed the truth audit's wrong held link-epochs not above the control's, the Galileo and GPS held link-epochs within 1 % of the control's, every epoch emitted.

One-change arms on observed data (the chain configuration of DOY 146) only if H1 to H3 pass.

### 117. 2026-11-18: the receiver extra-band delay free to vary in time (the managing session's queue item N6; pre-registered, a read)

**Why.** Section 79.4: with the third bands, the runs of DOY 144 from the two datum receivers disagree in two clusters; deweighting the third bands' code rows 100 times makes the pair's integers equal `f13`'s at every check, and the receivers' extra-band delays drift smoothly by decimetres over hours (DJIG −80 cm, SEYG −76 cm on E5b), 60 to 180 times their process noise (`network.rx_cb_carry_q` 1e-9 m²/s, 4.4 mm over 5.5 h). The question: with the delay free to vary in time, at full code weight, do the two anchors agree on every integer?

**Build.** `network.rx_extra_band_delay_q` (m²/s; default 0, byte-identical; fingerprinted when set): the random-walk noise of the receiver IFB columns of the receiver code-bias carry in place of `network.rx_cb_carry_q`, the pair's receiver code bias keeping its own; one rule (`rxCbCarryQ`) for every carry site (the inject gates, the harvest's step test, the prior-residual census). Unlike the falsified blanket `rx_cb_carry_q` 1e-6, it frees the extra-band columns only. Unit cases `RxExtraBandDelayQ` (2).

**Arms** (exe nrf45; q = 1e-5 m²/s: 19 cm in an hour and 44 cm in 5.5 h at one sigma, the scale of the measured drifts): on the simulator `t3q_sim` and `t3q_sim_r` (`t3t_sim`'s configuration from its two datum receivers, 3 h, the cascades held, truth) against `t3t45_sim` and `t3t45_sim_r` (same exe, off); on DOY 144 `k1q_n144` and `k1qr_n144` (`k1t_n144` and `k1r_n144`, cascades in the log mode, 6 h) against `k1t45_n144` and `k1r45_n144`.

**Bars** (a read: no third-frequency integer held on observed data, no adoption).
- Q1. Off: `t3t45_sim` reproduces `t3t_sim`'s integers and `k1t45_n144` reproduces `k1t_n144`'s two-run disagreement with `k1r45_n144` (else the controls are the same-exe runs).
- Q2. The simulator: no held integer wrong by the truth audit beyond the controls' (pairs and cascades), the pairs' held link-epochs within 1 % of the controls'.
- Q3. The question, DOY 144: the two-run audit of `k1q_n144` against `k1qr_n144` and of each against `f13_n144`, as section 79.4's run (a) (0 failing checks there); reported by cluster, with the extra-band delays' drift and the slant ionosphere on the clusters' lines of sight beside it.

### 112.5 2026-11-18: ARHT00ATA's wrong E14 wide lanes and the combined signals' wide-lane levels on the two streams (a read)

**The users** (`arht_e14_read.py`, DOY 146, from the resolver lines). On the phase-family stream the fixing user at ARHT holds E14's wide lane one cycle from its float user's against every reference in hours 10 to 12 (E26, E33, E07, E08, E12, E16, E25) and 23 to 24 (E32), the float 0.03 to 0.32 cycle from the other integer (0.43 in hour 23); on today's stream the two agree within half a cycle in every hour.

**The streams** (`stream_wl_bias.py`, `stream_wl_split.py`): a combined-mode user's satellite wide-lane level from the records, (P_L1X / λ1 − P_L5X / λ5) − (f1 B_C1X + f5 B_C5X) / (f1 + f5) / λWL, sampled once an hour, the phase-family stream minus today's, centred over the satellites at each epoch. Most satellites agree within ±0.05 cycle in every hour. E11 differs by 0.21 to 0.5 cycle in hours 3 to 15, its phase part +0.49 in hour 4 decaying to +0.08 by hour 20 and its code part −0.12 to −0.22; E14 by 0.29 to 0.40 cycle in hours 16, 21, 23 and 24 (its records are absent from the hourly sample in hours 10 to 15), the phase part; E31 by 0.13 to 0.29 in hours 3 to 8 and 0.36 in hours 23 and 24; E10 and E25 by 0.16 to 0.48 in hours 21 to 24; E18 0.48 in hour 6.

**Reading.** The phase-family stream gives a combined-mode user the pilot family's phase levels with the combined signals' own code biases, and for a few satellites the result sits fractions of a cycle from today's level, mostly in the phase part and drifting over hours, not a constant of the tracking kind (section 107.8's term is 0.026 cycle rms). That is enough to put a marginal wide-lane decision on the wrong side, as at ARHT on E14. E11 is also the satellite of DJIG00DJI's audit failures (section 112.2). Which level is right for the combined-mode users is not settled here; the next read is their own Melbourne-Wübbena residuals by satellite on both streams.

**The users' check** (`xuser_wl_frac.py`, `xuser_wl_frac_146.txt`): the eleven combined-mode float users' single-difference wide lanes (L1X − L5X against their references), the median distance from the nearest integer by satellite, hours 3 to 8, 9 to 15 and 16 to 24 of DOY 146. On today's stream every satellite sits at 0.04 to 0.12 cycle except E14 in hours 16 to 24 (0.19); on the phase-family stream E11 sits at 0.32, 0.39 and 0.25 cycle and E14 at 0.36 in hours 16 to 24, the other satellites as on today's stream. Today's stream's levels are the right ones for the combined-mode users; the phase-family stream's E11 and E14 levels are off for them by a fraction of a cycle, which is where ARHT's wrong E14 holds come from. E12 (0.03 to 0.21 in hours 9 to 15) and E36 move less. On DOY 147 (`xuser_wl_frac_147.txt`) the effect is smaller: E12 0.07 to 0.16 and E18 0.08 to 0.18 in hours 9 to 15, every other satellite within 0.08 cycle of today's stream. A defect of the build of section 112 for its combined-mode users, before any adoption: the member records join the pilot family's phases to the combined signals' separately estimated code biases, and for these satellites the two do not agree.

### 112.6 2026-11-18: the trace of the managing session's hypothesis (step 1): the failing links are pilot links of satellites first held at combined-mode receivers

**The hypothesis** (the managing session, 2026-11-18 04:50, after the pattern of sections 107, 110.3 and 112.2: the near-null steps and the audit's failures go with the codes kept apart, not with the slip layer). With the phase columns shared, the combined mode's own satellite code-bias columns (C1X, C5X) leave a weakly determined direction per satellite, the code level between the modes in the wide-lane direction; early in a run the satellite's shared wide-lane phase level is set through whichever mode's links are held first, and the other mode's links meet it a step away.

**The reads** (`mode_code_trace.py` on `n41_n146`, output `mode_code_n41_n146.txt`; the stations' tracking from `n43_smoke`'s stations trace; the audit's full failure list `audit_n41_n146.json`).
- The columns: per satellite and epoch, W = (f1 (C1X − C1C) + f5 (C5X − C5Q)) / (f1 + f5) / λWL from the satellite bias states, centred over the satellites (section 107.8's true term: at most 0.07 cycle, 0.026 rms). Over every satellite the median |W| is 0.022 to 0.040 cycle in every hour, the term's scale. E11 departs: −0.07 in hour 1, −0.18 in hour 3, −0.26 in hour 5, back to −0.17 by hour 9 (0.20 cycle, about 15 cm, of change); E29 moves 0.06 cycle, E14 0.04, E18 0.02. Their formal sigmas stay at 0.00 to 0.02 m (0.04 to 0.05 m in hour 1): the column that moves is not loose by its sigma, it is stiff and wrong for a while.
- First held: 19 of the 30 Galileo satellites are first held on a link of a combined-mode receiver (E11 at KRGG00ATF, E29 at CAS100ATA, E14 at MBAR00UGA), 11 at a pilot receiver (E18 at GUAM00GUM).
- The audit's failing checks (E1, 2 163): 1 608 (74 %) on pilot links of satellites first held at combined-mode receivers (DJIG00DJI–E11 335; GUAM00GUM–E29 322 and its E04, E28, E19, E26 links about 120 each; PARK00AUS–E29, E19, E28, E23, E04), 512 (24 %) on pilot links of pilot-first satellites (MAJU00MHL and SOLO00SLB–E18, GUAM00GUM–E33 and E31, PARK00AUS–E27 and E21), 43 (2 %) on combined-mode links. Were the failures independent of the first hold, about 31 % would fall in the first class (pilot links about half of the links, combined-first satellites 63 %).

**Reading.** The trace agrees with the hypothesis where it matters for the next build: the failures concentrate on the pilot links of satellites whose level the combined mode set first, and one satellite's combined-mode code column moves by decimetres (E11, also the satellite whose combined-signal wide-lane level section 112.5 found off for the combined-mode users). It disagrees in one detail: the columns are not formally loose (sigmas of a centimetre), so the cost is a stiff column carrying a wrong value, not a visibly free one. Sharing one code bias per band removes the column either way; the build of section 118 follows, as the managing session's rule provides.

### 118. 2026-11-18: the codes-shared form of the phase-family table (pre-registered)

**Build** (exe nrf46). `network.phase_family_codes` (default off, byte-identical; read only with `network.phase_families`): at the intake a member's codes are read under the identity's names too (C1X as C1C, C5X as C5Q), as its phase, Doppler and signal strength already are, so the two tracking kinds share one satellite code bias per band; the stream's member code-bias records (C1X, C5X) repeat the identity's value and, after every rule of the emission, its discontinuity counter, as the member phase-bias records do; the stations trace's tracking names the code renames (`E:L1X>L1C C1X>C1C L5X>L5Q C5X>C5Q`); the run-start intake line says CODES SHARED; fingerprinted. Unit cases `PhaseFamilyCodes` (2).

**Arms** (exe nrf46; `entry_revalidate` off in every arm, as in run A and today's runs, so that each comparison has one change).
- `n46c_n146`, `n46cr_n146`, `n46c_n147`: today's configurations (`d24_n146`, `d24r_n146`, `d24_n147`) with the table and the option, 24 h; their users at the ten pilot-type receivers and the eleven combined-mode receivers outside the network, fixing and float, engine rscb6.
- The simulator: `m1_c` (the clean two-mode bed, `mode1_r1`'s configuration with the table and the option, truth alias `simb_mode1_ta`) and `m1s_c` (section 112.1's slip bed with the option).
- Off: `off46_smoke` against `off45_smoke`.

**Bars.**
- C1. Off identical: `off46_smoke` reproduces `off45_smoke`'s products, stations trace and ring-fix trace.
- C2. Run A reproduced: `n46c_n146` against `s2_n146` (run A, exe nrf37, the files renamed) at equal length: every network trace and the stream's records identical except the member records (P on L1X and L5X, B on C1X and C5X), which run A's stream lacks; any other difference explained before any other bar is read.
- C3. The beds: on `m1_c` no held integer wrong by the truth audit, with what the injected mode code offsets do to the held-out user and the products reported as section 106.1 did; on `m1s_c` no link held through any injected slip.
- C4. Observed data, the managing session's bars of 01:30: GPS held share not below today's by more than the neutral two-root spread (3 points) in any hour from 3, its mean over hours 3 to 24 not lower by more than 1 point; RAIM rows within today's pair's own difference; the two-run audit of DOY 146 (`n46c_n146` against `n46cr_n146`) with 0 failing Galileo checks, as today's pair; bar 7 (no Galileo link held through a slip today's run flags); the users of both signal types: no hold wrong by the float check (the combined-mode users by the check without the network's integers), and the paired per-receiver difference not worse by more than 1 mm in any hour from 3, the difference of medians beside it.

**Reading prepared.** C1 to C4 passing: the codes-shared form is adopted scoped in the chain configuration on this branch with its ledger row. Only the small user items missing (a few reference changes, millimetres at two or three receivers): not adopted tonight; the author decides, with the managing session's recommendation to adopt.

### 119. 2026-11-18: the stop at the author's instruction (relayed by the managing session)

**The instruction.** The author asked to stop this session at a good stopping point and to continue the branch in a new session managed directly by the managing session, which starts with the audit of the chain code (the audit section 112.3 (d) set aside). The managing session's message superseded the build of section 118; the build had started, so it is brought to a compiling state behind its key (default off) and committed as work in progress.

**What ran and what did not.**
- Section 114 (the users at a 10 degree mask): 18 of the 196 users ended and ten more (DOY 146, fixing, `kokv` to `pots`) were finishing on their own at the stop (DOY 146, fixing, `arht` to `pots` in all); the dispatcher was stopped, and the ten DOY 147 users it had started were stopped within a minute (partial logs, no `exitcode.txt`; a rerun of `users_chain_m10.sh` restarts them and skips the finished ones). Not graded.
- Section 115 (the guard's release form): built (exe nrf45), smoke `h40_nr_smoke` exit 0; its five arms and control made, not run.
- Section 116 (held first by life): built (nrf45), unit cases pass, smoke `u2_zl_smoke` exit 0; `u2_z45` and `u2_zl` were running at the stop (about two thirds through) and finish on their own, ungraded (H1: `compare_runs.py u2_z u2_z45`; H2: `sat_host_hour.py` on both; H3: `held_by_family.py`); `m1_l`, `h40_l` and their controls made, not run.
- Section 117 (extra-band delay noise): built (nrf45), unit cases pass, smoke `k1q_smoke` exit 0; its eight arms made, not run.
- Section 118 (codes shared): built behind its key (default off) and compiling: exe nrf46 (`b80af531ae1dfecf`, the tip's tree) passes the two unit cases `PhaseFamilyCodes` with 181 ring-fix, phase-family, arc-graph and epoch-contract cases; no smoke was run; no arm run; its arms, smokes and 84 users made.
- nrf45's off path reproduces nrf44's (`off45_smoke` against `off44_smoke`: products, stations trace and ring-fix trace identical); its 244 unit cases and `nom_epoch_lint.py` pass.

The hand-over note for the next session (engines, folders, scripts, open findings, traps, where to start the chain audit) is `D:\GnssProcessorSupport\gates\irdu_handover\IRDU_handover_2026-11-18.md`.
