# NRF: integer fixing in the network, ring by ring from the chain's root, on the simulator

**STATUS**

- **id:** NRF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-14 (night), before any graded run, on the author's instruction to build integer fixing in the network by the ring method of card IRDU section 12: a chord's integers are decided ring by ring outward from the chain's root, held in the network's own solve, released at every event that can invalidate them, and every held integer is audited against truth. Simulator only, default off. Stages: N1 the bed without re-seats, N2 r3c with re-seats and event beds, N3 the 6 h reference bed; observed data only after the three pass and are re-graded. Serves card IRD's steps IRD-5 and IRD-8 and card ARL's rungs, with their frozen bars. Results are appended below. 2026-11-15, N1 (section 9): every bar holds (off identity, 0 wrong holds, the negative control seen by the audit, both edges, every epoch emitted); 11 of 186 links held in 2 h, all right, from 40 min; the stream and the users read the plain chain arm's level; the limit is the floats' precision against the gate and the ring order, not wrong integers. 2026-11-15, N1b (sections 10 and 11): with the order free and an odds gate (ln 1e6 on N1 and on the wide lane) 170 of 186 links are held by 2 h, 0 wrong, and the stream's narrow lane reaches Q1's level at depths 1 and 3 in the second hour (D_NL 0.036 over hour 2 against 0.081); the users fix worse during that move (HELD 8 % against 25 %, wrong pair integers at one user). 2026-11-15, N2 (sections 12 and 13, [part 2](NRF_network_ring_fixing_2026-11-14_part2.md)): from a cold start 0 wrong held integers on r3c and seven event beds through re-seats, returns, a root handover, splits and slips; the steady-state harness (truth for 30 min, then the rule) takes 68 wrong decisions, not traced. 2026-11-15, N2b (part 2, sections 14 and 15): the cause was a released link's carried entry that kept the hold's variance; with it re-opened, the ceiling and the steady-state harness take 0 wrong; held-first re-attachment keeps held integers through re-seats (held share of the last 30 min 62 to 84 % against 30 to 61 %); true slips on held links released at their epoch; 0 wrong on every arm. N3 pre-registered (part 2, section 16). 2026-11-15, N3 ([part 3](NRF_network_ring_fixing_2026-11-14_part3.md), section 18): on the 6 h golden sim bed, both constellations, 0 wrong held integers (342 217 held link-epochs with held first); held first holds 77 to 88 % of the chords from hour 3, the stream's narrow lane at most 0.024 cycle at every depth, its users hold integers about half the time with 0 wrong (plain chain 8 to 17 %); a constellation-matching defect of the cut algebra found and fixed before the re-runs (part 2, section 16.1). N3b, N2c and O1 pre-registered (part 3, section 19). 2026-11-15, N3b, N2c and O1 (part 3, section 20), every bar passing: the 6 h bed started 1 h later takes 0 wrong, and two starts disagree only by the accumulated wind-up branch of arcs older than the later start (0 failing on the arcs both start together); an unflagged slip under a hold is released by the residual monitor after its 4 epochs and held again right 16.5 min after the slip; on observed data, log only, Galileo L1X/L5X, 288 links would be decided in 6 h with products unchanged, and two starts agree on every arc they start together (0 failing of 6 642 checks). Stage O2 pre-registered, the first runs that hold integers on observed data ([part 4](NRF_network_ring_fixing_2026-11-14_part4.md), section 21). 2026-11-15, O2 (part 4, section 22): with the odds at twice the formal sigma, Galileo L1X/L5X held on observed data from two starts agree on every held integer (0 failing of 78 588 checks over every common arc, the wind-up branches removed with the new trace), every epoch emitted, RAIM unchanged; with the formal sigma the audit fails on 7 held links. 2026-11-15, O2 amended (part 4, sections 21.1 and 23): with twice the formal sigma, two reference receivers at one start agree on every held integer (0 of 82 972), and so do GPS L1C/L2W and Galileo L1X/L5X held together (0 of 126 238); the formal-sigma failures are the younger network's early decisions. 2026-11-15, G0, O3 and O4 (part 4, sections 24 to 26): deep GPS links are held back by window resets at cut releases and by fractions that carry the frame's tree-arc errors; on DOY 145 two receivers agree on every held integer (0 of 122 609); holding all four pair families on DOY 144 fails (an uncertified re-host on Galileo C/Q, the GPS frame of one run, two GPS families on one L2W root). 2026-11-15: stage O2's formal-sigma pairs re-run with the bad-epoch response of card [BER](BER_bad_epoch_response_2026-11-15.md) on, pre-registered ([part 5](NRF_network_ring_fixing_2026-11-14_part5.md), section 27). 2026-11-15, its results (part 5, section 28): with the response on, the formal-sigma pairs agree on every held integer (0 of 155 633 checks between two receivers, 0 of 131 369 between two starts), every epoch emitted, RAIM 7 against 7, the holding share unchanged (82 to 87 % from hour 2). 2026-11-15 (part 5, section 31): on DOY 145 the same setting fails the same-start audit on one Galileo wide-lane decision (271 failing checks, closures +3 / +4); nothing adopted, twice the formal sigma stays. 2026-11-15: remedy (a) and the formal sigma adopted in the chain posture, with the adopted set and the step declaration pre-registered: [part 6](NRF_network_ring_fixing_2026-11-14_part6.md). 2026-11-15 (part 6, sections 42.1 to 47): the remedy as first built carried a fractional step into the moved side and failed DOY 144; kept only at a step of 0, the re-runs pass every bar on both days and the adoption stands; the decision cap as a key and the settled flags pre-registered; the step declaration leaves the estimates unchanged and fails its cause-side check. 2026-11-15 (part 6, sections 48 and 49): the decision cap at 12 passes on the simulator and on DOY 144 and is adopted in the chain posture; the settled flags, after two fixes, equal the rule at every epoch on DOY 144 (`f11_n144`). 2026-11-16 (part 6, sections 50 and 51): the release increments built and passing (`f12_n144`, the stream for user replays); the cap of 12 verified on DOY 145 with two starts; the GPS held share read (the wide lane refuses through the fraction and the odds; the block split's cause open, section 51.1). 2026-11-16 (part 7, section 52): the bed for remedy (b) built (GPS code biases per receiver and satellite); on it the ring fix takes no wrong integer, but GPS holds far more than observed GPS: [part 7](NRF_network_ring_fixing_2026-11-14_part7.md). 2026-11-16 (part 7, section 53): E05's flagged walk on DOY 144 was KOKV00USA's arc; a flag that asks a satellite's links to agree passes the segment bar on Galileo and fails by one GPS run a day. 2026-11-16 (part 7, sections 54 and 55): the unsettled steps built and passing (streams `u12_n144`, `u12_n145`); Galileo L1C/L5Q held beside L1X/L5X with the audit zero; the slow wide-lane error the same on GPS and Galileo. 2026-11-16 (part 7, sections 56 to 60): the step declaration adopted in the chain posture; GPS L1W/L2W beside L1C/L2W holds almost nothing (the shared L2W); a root-free flag rule proposed; a third frequency planned, simulator first; the third band beside the two held families passes (60.1). 2026-11-16 (part 8, sections 61 to 63): the flags by the largest tied set built (streams `l12_n144`, `l12_n145`); the chord table forms every configured family; GPS's held share is short on the wide lane: [part 8](NRF_network_ring_fixing_2026-11-14_part8.md). 2026-11-16 (part 8, sections 64 to 74): GPS on L1C holds as Galileo does; the cap of 12; the final candidate passes and is adopted in the chain posture; the cascade right on the simulator. 2026-11-16 (part 9, sections 75 to 77): Galileo C/Q's moving levels on DOY 144 are not the wet delay, the borrowed antenna pattern or weak ties; the cascade's refusals sat where the third band's tree left the pair's holds, and with that tree grown on the pair's forest they vanish on the simulator: [part 9](NRF_network_ring_fixing_2026-11-14_part9.md). 2026-11-16 (part 10, section 83): the adopted posture over 24 h on DOY 146 and 147: [part 10](NRF_network_ring_fixing_2026-11-14_part10.md).
- **source:** the author's two messages of 2026-11-14 (section 1); [IRDU part 3](../record/refound/IRDU_user_integers_on_the_chain_2026-11-14_part3.md) section 12 (the rings, Q1 to Q4, the gated rounding and the log-only search); [IRD](IRD_integer_recovering_datum_2026-10-13.md) sections 9 to 11 (bars, rules, increments); card ARL section 2026-10-24 and card FCFC section 9 (both in the record); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 6; [IRDD](IRDD_integer_recovering_datum_rows_2026-11-03.md) sections 2 to 4

**Outcome.** Question: do chord integers decided ring by ring from the chain's root, held in the network's solve and released at every event, take only right integers and bring the stream to the truth-fed chain's level? On the simulator they take 0 wrong integers through re-seats, slips, seven event beds and the 6 h reference bed on GPS and Galileo, from either of two starts, holding 77 to 88 % of the chords from hour 3 with the stream's narrow lane at the truth-fed level; users hold integers half the time, 0 wrong (N1 to N3b, N2c). On observed data, Galileo L1X/L5X held with the odds at twice the formal sigma agree between two starts on every common arc; with the formal sigma they do not (O1, O2). Standing: default off; one observed-data bed.

## 1. The author's words and the assignment

The author's two messages, verbatim, as relayed by the requesting session:

- "Build the safe ring-by-ring fixing on the simulator. Integer fixing in the network is no longer "do not build", in fact, it seems like the current bottleneck we need immediately!"
- "Continue to manage Opus to work on the integer network and our ring method, sim, then observed data, expand to multi-system, multi-frequency if everything works."

The assignment as relayed (a reading of the requesting session, not the author's words): top priority for the night, ahead of the users' rule of card IRDU section 15 (its runs wait) and in place of the queued all-integers run on r3c, which becomes the truth-fed control of N2. Stages N1 to N3 in order; a report at the end of N1. Limits in section 8.

## 2. Why the do-not-build verdict no longer stands for this work

The verdict of 2026-10-07, "producer network AR: do not build", is lifted by the author for this program. Card FCFC section 9 gave the reasons a held producer solve added little: the zero-q float already integrates the whole arc, so conditioning on integers moves the shared parameters only by cross terms; the emitted phase biases were not integer-recovering on the datum then armed (several anchors, float pin targets, no receiver phase-bias states); and the releases cost too much (collateral releases, coverage, a slip no detector saw held for 63 epochs).

Card IRDU section 12 measured the eq34 chain, which differs on each count:

1. **The chain is integer-recovering.** Its tree links are pinned at integer targets, the root receiver's phase bias is pinned and every receiver carries phase-bias states. With every link at its true integer the stream reaches the truth stream's level at every depth: D_NL p50 0.016 to 0.030 cycle at 15 to 30 min against 0.080 to 0.184 cycle without (12.1). On this datum integers move the stream, and here they act through the solve itself, not through a detached second solve that only the emission reads (card FCF's design).
2. **A ring reaches the root's level once its own ring is held**, in 15 to 45 min (12.3).
3. **Re-seats onto links at their integers are free.** The frame is kept and the moved pairs cost nothing after 1 to 5.5 min, against 25 to 45 min of wide-lane cost without (12.5, Q4). The release cost card FCF measured belongs to the release of floats, which held links remove.
4. **Two decision rules made 0 wrong fixes, log-only**, at every stage of the bed without re-seats: the network's search (30 978 kept row-epochs per band, 0 wrong) and a gated rounding (formal sigma at most 0.10 cycle, fraction at most 0.20; 0 wrong at every time and stage) (12.5).

The constraints of card ARL's section of 2026-10-24 stand and are kept here. The fix is the network's, never per station. No tree (pivot) ambiguity is searched or held: the tree links are the frame. An event that changes the frame invalidates the integers it moves: here every frame-changing chain event releases the links it touches (section 4.3). The 14.48 % wrong fixes at a reported success rate of 1.0 came from a covariance blind to the carry; card IRDC section 6 found wrong certifications only on damaged floats. The gate below therefore reads damage and persistence, not sigma alone, and every held integer is audited against truth at every epoch.

## 3. Steps of card IRD served, and the bars reused

- **IRD-5** (certification from the network's own integers): here the chords are certified ring by ring, and IRD-5's "0 wrong lattice-sourced targets" becomes 0 wrong held integers against truth.
- **IRD-8** (the consumer proof): K-a (0 wrong among at least 1 000 settled determined integers per held-out station on each 2 h bed; on the 6 h bed a 95 % Wilson upper bound below 3e-4), K-b, K-c, P-a to P-c, applied at N3. At N1 and N2 the user readings are K-a's zero and the float position by bin.
- **Card ARL's rungs:** 0 wrong on every rung; N2's event beds include the slip rungs for which a fixture exists.
- **The stream bars** W-WL and W-NL (card IRD section 9), per depth class, as read in card IRDU section 12.1.

## 4. The build (default off; byte-identical when off)

**Keys** (three; ledger rows UNVALIDATED; the numbers of the rule are constants of the code, frozen here, not keys):

- `network.ring_fix.enable` (default false): the mechanism.
- `network.ring_fix.rule` (default `gated`): `gated` is the rule of 4.1; `plain` is the negative control only (every candidate rounded: no sigma, fraction, damage or persistence test).
- `network.ring_fix.audit_frame_file` (default empty; simulation only): a frame-target file (`scripts/irdu_frame_targets.py --all`: station, satellite, signal, frame integer, role) against which every held integer is audited each epoch.

The loader refuses `enable` without the eq34 chain and `network.trace_chain_datum` (the chain record the rings are read from), and refuses an audit file that does not exist. Without `enable` no object is built and no branch is reachable.

### 4.1 Decide (frozen)

- **Links.** A link is a (station, satellite) arc of a pair family (band 1 and band 2 of a constellation, as in the chord table of `network_chain_frame.csv`) that is a chord of both bands' trees this epoch. Tree links are never decided or held: they are the frame.
- **Rings.** On the band-1 tree, the root receiver has depth 0. A link's ring is the larger depth of its two vertices; its inner vertex is the one of smaller depth.
- **Candidates, ring by ring.** A ring-2 link is always a candidate (its inner vertex, a satellite at depth 1, reaches the root through one tree link). A link of ring 3 or more is a candidate when every link of its inner vertex to a vertex of smaller depth than the inner vertex is held: tree links count as held, chords count when this mechanism holds them.
- **The gate** (card IRDU 12.4 (b), as measured there): from the link's two-band float f1, f2 (cycles) and its 2x2 conditional covariance (cycles squared, unscaled; the chord table's quantities), the wide lane is rounded first, z = round(f1 - f2); the ionosphere-free row with the wide lane rounded gives n = (F1 f1 - F2 f2 - F2 z) / (F1 - F2), N1 = round(n), N2 = N1 - z. The link passes when the formal sigma of n is at most **0.10 cycle** and |n - N1| is at most **0.20 cycle**.
- **The damage gate.**
  - Event window, **600 s**: both bands' carried entries exist and their last re-level (the carry's own stamp: birth, a slip re-level, a drop, an erase and rebirth) is at least 600 s old; neither band is in this epoch's free-epoch latch; the link has been a chord with a float at every epoch of the last 600 s (a gap above 45 s restarts the count); it has not been a tree link in either band within the last 600 s.
  - Consistency: the producer's lattice of fixed cycles, where it implies the link's integer on a band, agrees with the gate's integer (a disagreement refuses the link); and the link passed the gate with the same (N1, N2) at each of the last **10** epochs at which it was a candidate (any refusal restarts the count).
- **The decision** is taken after the solve of epoch t and held from epoch t + 1.

### 4.2 Hold

One diagonal prior row per band on the link's ambiguity column, at lambda_j N_j in metres, with **sigma 1e-4 m**, folded in PASS B by a station step of its own (the oracle pin's row and path: Λ = 1/sigma², η = Λ (lambda_j N_j - x_lin)). A tight prior, not an exact constraint: at this sigma Q1 put the truth-fed chain at the truth stream's level at every depth (12.1), and the posterior sits within about 1e-4 m of the held value; it stays two decades below the 1e10 datum pins, so the gauge remains the chain's; and a prior row leaves the solve's structure unchanged, so a release is the end of one row plus the vehicle's own drop. A link that the chain pins (a tree link) never takes the row.

### 4.3 Release (frozen)

At the epoch a cause is seen and before PASS B, the link's row ends and its carried entries are re-opened: under the float vehicle by its drop (restored variance the largest of the arc's birth variance, its first variance and the floors; `ambFloatDropFloored`), on the leveling vehicle by an erase. A released link is never held at a stale level, and it re-enters the decision only after the event window (the drop stamps the re-level). Causes:

1. **slip:** either band in this epoch's free-epoch latch (any detector: Melbourne-Wubbena, loss of lock, IF time difference, geometry-free jump, gap re-entry, wind-up restart).
2. **refloat:** either band's carried entry dropped or erased since the previous epoch by another mechanism (RAIM attribution T4, T3 or prior channel, the gap cliff, the gross line-of-sight screen), seen as a new re-level stamp or a missing entry.
3. **gap:** the link absent (no ambiguity column in a band) or no chord of its pair family for longer than `phase_leveling_max_gap_s` (900 s on these beds). Within that horizon the row pauses and the time-linked carry spans the gap, with the slip layer's re-entry test at the return.
4. **chain:** the link becomes a tree link in either band; a cut release (the returning, re-host, slip and merge rows) whose moved side holds either vertex of the link; a root handover, split or merge of its component (stage N1's conservative rule; N2 replaces it by section 4.4).
5. **monitor:** the link's post-fit ionosphere-free phase residual (its two bands' observation phase rows, metres) above **0.03 m** in absolute value at **4** consecutive epochs.

### 4.4 Re-seats (stage N2, not built for N1)

A re-host onto a held link keeps the frame (card IRDU Q4), so nothing is released. Elsewhere a cut moves its side by a step known exactly when the path is held (an integer), and the links crossing the cut are re-expressed by that step; where the path is not held, they are released. Built and pre-registered with N2.

### 4.5 Audit and records

- **Audit** (simulation only): every held link at every epoch against the frame-target file: wrong when either band differs; a held link the file does not list is counted unaudited. N1's bed keeps one tree for the whole run, so the file's integers hold for the whole run. N2 needs the frame integers of links that rise during the run, from truth and the tree of each epoch.
- **Run-end line:** decided, held (links at the end, link-epochs), released by cause, refused by cause (link-epochs), wrong against truth (decisions, held link-epochs), unaudited.
- **Events** (standing invariant 7, both edges with identities): `RINGFIX hold` at each decision and `RINGFIX release` with its cause, in `network_events.csv`.
- **Trace** `network_ring_fix.csv`: per epoch and link, ring, candidate, floats, n, sigma, fraction, the lattice integers, the state (refused and why, decided, held, released and why) and the audit.
- Doxygen, CMake, unit tests of the gate, the candidate rule, the hold bookkeeping and each release cause.

## 5. Stage N1: the bed without re-seats (pre-registered)

**Bed and base.** `irdu_nr` (the third cut of card IRDU section 11.1: one tree for the whole run, root S30000SIM, depth 5), 2 h, the adopted arm A1krhRsmh with the wet delay carried: the configuration of `_rap_irdu_zw_nr3`.

**Arms** (exe: this card's build, copied under its own name, sha256 stated with the results; Release only):

- **N1-0, off identity:** the new build with the key off reproduces `_rap_irdu_zw_nr3` (the plain chain arm) and `_rap_irdu_q1_nr3` (Q1, every link held at its true integer by the oracle pin) whole (`nom_td_gate.py --whole --mask-time`). These two runs are the controls.
- **N1-G:** the plain chain arm with `network.ring_fix.enable` and the audit file `config/sim/irdu/fpin_nr3_all.csv`. Smoke first (10 epochs).
- **N1-P, negative control:** N1-G with `network.ring_fix.rule` `plain`.

**Readings.**

- Links held by ring against time: first hold per ring, the ring complete (every chord of the ring held), held at 30, 60, 90 and 120 min.
- Releases by cause and refusals by cause.
- Feedback: links held on N1-G against the log-only counts on the plain arm (the search's kept rows and the gated rounding's acceptances of card IRDU 12.5 (a) and (b)), and the would-be acceptances on N1-G's own chord table.
- The stream against truth by depth class in 15 min bins (D_WL, D_NL per-pair offsets), N1-G against the plain arm and Q1; card IRD's W-WL and W-NL over hour 2.
- Users (the five of card IRDU, the users' build of this branch): started at 30 min, plus one row started with the network; float 3D by 15 min bins; fixing users HELD and WRONG; on N1-G's stream against the plain arm's and Q1's.

**Bars.**

- **N1-B0:** N1-0 IDENTICAL on both controls.
- **N1-B1:** N1-G holds 0 wrong integers against truth, at every epoch of every held link, with the decided links and held link-epochs stated.
- **N1-B2:** the negative control's audit counts wrong held integers (more than 0). If plain rounding takes none, the control is void and is reported so.
- **N1-B3:** every release names its cause and link; holds and releases are written at both edges; decided minus released equals held at the end.
- **N1-B4:** N1-G emits 240 of 240 epochs.

**Prediction** (written before the runs). On this quiet bed nothing slips and nothing re-seats, so releases come only from the monitor or RAIM, and few or none are expected. The gate accepts few ring-2 links before 60 min (card IRDU 12.5 (b): 3 of 27 at 60 min on the plain arm), so N1-G holds a few links by 60 min and more in hour 2 as held rings tighten the next ring's floats (12.5 (b): 10, 5 and 28 accepted at 60 min once the previous rings were held). The stream moves towards Q1's level only at the depths whose rings are held, so most of the gain, if any, is in hour 2; a 2 h run may hold too few links to move the stream much, which is a reading, not a failure. N1-P takes wrong integers in its first minutes (plain rounding at 30 min took 12 wrong of 186 in 12.3).

**Decision after N1.** N2 is built and pre-registered only if N1-B0 to N1-B4 hold. The report goes to the requesting session at the end of N1.

## 6. Stages N2 and N3 (outline; each is pre-registered in detail before it runs)

- **N2:** r3c with its re-seats (section 4.4 built), and the event beds of card IRDU (a true slip, a returning satellite, a root handover, a split and merge). Control: the truth-fed run on r3c (the former queued all-integers run). Bars: 0 wrong holds; every slipped or cut link released at its epoch.
- **N3:** the 6 h simulator reference bed, with card IRD-8's K-a to K-e and P-a to P-c.
- **Observed data** only after N1 to N3 pass and are re-graded, and only when asked. Multi-system and multi-frequency after that, as the author's second message orders.

## 7. Decisions made in the author's absence, for review

1. A link's ring is the larger depth of its vertices (card IRDU section 12's definition); a link of ring 3 or more waits until its inner vertex's links towards the root are held.
2. The decision rule is the gated rounding of card IRDU 12.4 (b) at its 0.10 cycle gate, not the search's acceptance: it reads one link at a time, which is the ring order, and it was measured on these chord tables. The search's lattice enters only as a veto (a disagreement refuses).
3. The damage gate's numbers: a 600 s event window, a 45 s gap, 10 epochs of the same integers.
4. The hold is the oracle pin's prior row at sigma 1e-4 m, a tight prior rather than an exact constraint (reasons in 4.2).
5. The monitor reads the held link's post-fit ionosphere-free phase residual at 0.03 m for 4 epochs.
6. At N1 every frame-changing chain event releases the links it touches in its component; the exact re-expression waits for N2.
7. The audit at N1 reads a frame-target file (one tree on the bed); N2 audits from truth and each epoch's tree.
8. Three keys; the rule's numbers are frozen constants of the code.
9. The users replay on this branch's build.

## 8. Limits

Simulator only until the author says otherwise. Nothing merged or pushed. No reference configuration and no ledger verdict changed; the new keys are default off with UNVALIDATED rows. Every decision above is listed for the author's review. The runs, commits and numbers are reported so each can be re-graded.

<!-- Results are appended below this line under dated headings. -->

### 9. 2026-11-15: stage N1, the bed without re-seats: every bar holds; 11 links held in 2 h, all right; the stream and the users unchanged

**What ran.** The ON arms on the build of commit `f2a5e00` (exe `gnss_processor_irdu_nrf1.exe`, sha256[:16] `40764cd6b753acb2`): `_rap_nrf_g_nr3` (N1-G, the frozen rule) and `_rap_nrf_p_nr3` (N1-P, plain rounding), with the controls `_rap_nrf_off_zw_nr3` and `_rap_nrf_off_q1_nr3` (the key off). The identity gate on the build of the next commit (`gnss_processor_irdu_nrf2.exe`, `aebd34edebfe7234`: one run-end text line made conditional, below) against the previous build of this branch (`gnss_processor_irdu_pv1.exe`, `cee9504ce1d1424e`, commit `a024ae3`): `_rap_nrf2off_{zw,q1}_nr3` and `_rap_pv1off_{zw,q1}_nr3`. Smokes on `gnss_processor_irdu_nrf0.exe` (`a3c395afcbf2cde4`): `_rap_nrf_smk_g_nr3` (0.09 h) and `_rap_nrf_smk_p_nr3` (0.2 h). Every run exit 0 with 240 of 240 epochs (runs under `D:/GnssProcessorSupport/runs_nom`). Users: 20 replays on N1-G's stream with the users' build of card IRDU (the fixing control's `gnss_processor_rscb3.exe`, `5ecf8e1c765fafe3`, copied as `gnss_processor_irdu_users_rscb3.exe`), read beside the controls' replays of sections 12.1 and 12.5 of card IRDU on the same build, regraded here with the same tools; and 60 float-only replays on this branch's build (suffix `_bnrf1`), which reproduce the float readings below and carry no HELD reading (this branch's resolver does not write the held lines the grader reads, so section 7 item 9 is corrected to the users' build). Grades: `runs_nom/_irdu_grades/nrf_n1_*`; the independent re-grade is `scripts/nrf_audit.py`.

**N1-B0, off identity: PASSES.** The build with the key off against the previous build: IDENTICAL whole on both controls (`nom_td_gate.py --whole --mask-time`). Against the controls as run in card IRDU section 12 (build `fp2`), the first build of this card differs in two text lines and nothing else: the products' legend line (the `[,discpv=<n>]` token of card IRDU section 14, commit `a024ae3`) and one run-end log line that named the new contribution kind; that line now names it only on a run that folded it (committed with this section). Every product record, every trace and the run manifest are identical.

**N1-B1, 0 wrong holds: PASSES.** N1-G decided 11 links (ring 2: 10; ring 3: 1), each right against the frame file at its decision; 777 held link-epochs, 0 wrong. The code's audit and the independent re-grade agree to the count. First hold at 40.5 min (ring 2, S38000SIM-G22) and 97.5 min (ring 3); 0, 3, 5 and 11 links held at 30, 60, 90 and 119.5 min; no ring complete; 0 releases (a quiet bed: no slip, no re-seat, no monitor trip).

**N1-B2, the negative control: PASSES.** N1-P: 1 285 decisions, 1 254 wrong; 41 903 held link-epochs, 40 138 wrong. The audit sees the wrong holds. The monitor released 1 028 of them (RAIM refloats 68, slips 3), and the plain rule decided them again at once.

**N1-B3, both edges: PASSES.** Every release is an event row with its cause and link (N1-P: 1 099 rows). Decided equals released plus held at the end: N1-G 11 = 0 + 11, N1-P 1 285 = 1 099 + 186. Hold rows folded: two per held link-epoch (1 554 and 83 806), none skipped on a tree edge, none without a column; carried entries re-opened, two per monitor release (2 056).

**N1-B4: PASSES.** N1-G emits 240 of 240 epochs.

**Refusals** (N1-G, link-epochs): not a candidate 27 050, sigma 14 216, window 1 362, persistence 108, fraction 0, lattice 0. The ring order and the gate's sigma bound hold back nearly every link; no link was refused by its fraction or by the lattice.

**Feedback.** Chords whose gate passes (sigma at most 0.10, fraction at most 0.20) and that are not held, at 30, 60, 90 and 119.5 min, graded against the frame file:

| arm | held + passing (not held) | wrong among them | sigma_if p50, rings 2 / 3 / 4 / 5, at 90 min | at 119.5 min |
| --- | --- | --- | --- | --- |
| plain chain arm | 0, 3, 7, 19 | 0 | 0.166 / 0.209 / 0.262 / 0.239 | 0.123 / 0.158 / 0.202 / 0.197 |
| N1-G | 0 + 0, 3 + 1, 5 + 2, 11 + 8 | 0 | 0.168 / 0.195 / 0.251 / 0.235 | 0.137 / 0.157 / 0.198 / 0.191 |

The totals are the same on both arms: holding 11 of 186 links does not make more links decidable within 2 h, and the formal sigmas by ring move by at most 0.016 cycle. The plain arm's count at 60 min reproduces card IRDU 12.5 (b) (3).

**Stream** (per-pair offset p50 against truth by depth class, cycles; N1-G / plain chain arm / Q1):

| channel, depth | 15 to 30 min | 60 to 75 min | 75 to 90 min | 90 to 105 min | 105 to 120 min |
| --- | --- | --- | --- | --- | --- |
| D_NL, 1 | 0.080 / 0.080 / 0.021 | 0.047 / 0.048 / 0.020 | 0.060 / 0.055 / 0.010 | 0.036 / 0.048 / 0.015 | 0.040 / 0.023 / 0.025 |
| D_NL, 3 | 0.155 / 0.155 / 0.016 | 0.094 / 0.094 / 0.015 | 0.130 / 0.097 / 0.016 | 0.062 / 0.065 / 0.017 | 0.059 / 0.048 / 0.013 |
| D_NL, 5 | 0.184 / 0.184 / 0.030 | 0.092 / 0.091 / 0.015 | 0.162 / 0.142 / 0.017 | 0.097 / 0.101 / 0.019 | 0.077 / 0.075 / 0.015 |
| D_WL, 1 | 0.131 / 0.131 / 0.079 | 0.058 / 0.035 / 0.031 | 0.065 / 0.058 / 0.027 | 0.067 / 0.041 / 0.043 | 0.048 / 0.037 / 0.028 |
| D_WL, 3 | 0.131 / 0.131 / 0.085 | 0.065 / 0.064 / 0.051 | 0.065 / 0.067 / 0.056 | 0.066 / 0.068 / 0.052 | 0.051 / 0.052 / 0.049 |
| D_WL, 5 | 0.123 / 0.123 / 0.069 | 0.080 / 0.081 / 0.051 | 0.091 / 0.095 / 0.050 | 0.086 / 0.088 / 0.052 | 0.064 / 0.073 / 0.048 |

Card IRD's stream bars over hour 2 (48 720 pair-epochs): N1-G D_WL p50 0.066 with 96.0 % within 0.20 and D_NL p50 0.084 with 91.3 % within 0.25; the plain arm 0.067 (96.2 %) and 0.081 (92.9 %); Q1 0.051 (100 %) and 0.020 (100 %) (card IRDU 12.1). N1-G and the plain arm both pass W-WL and W-NL. N1-G reads the plain arm's level within 0.04 cycle in every cell above: a little worse on the narrow lane in some cells of the second hour (depth 3 at 75 to 90 min, 0.130 against 0.097; depth 1 at 105 to 120 min, 0.040 against 0.023) and a little better in others; not traced.

**Users** (the five of card IRDU; float 3D median over the users by 15 min from each user's start, mm; HELD, WRONG and UNRESOLVED of the fixing users; the users' build):

| stream, start | 0-15 | 15-30 | 30-45 | 45-60 | 60-75 | 75-90 | 90-105 | 105-120 | HELD; WRONG, UNRESOLVED (compared) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| N1-G, 0 | 1 613 | 1 259 | 795 | 287 | 320 | 236 | 82 | 84 | 23.2 %; 0, 102 (102) |
| plain chain arm, 0 | 1 613 | 1 259 | 794 | 286 | 317 | 236 | 84 | 86 | 24.5 %; 0, 106 (106) |
| Q1, 0 | 2 188 | 939 | 587 | 209 | 165 | 115 | 109 | 106 | 50.2 %; 0, 0 (1 357) |
| N1-G, 30 | 1 157 | 277 | 338 | 222 | 88 | 98 | | | 31.4 %; 0, 104 (104) |
| plain chain arm, 30 | 1 157 | 274 | 335 | 221 | 86 | 96 | | | 26.4 %; 0, 104 (104) |
| Q1, 30 | 781 | 180 | 192 | 101 | 119 | 119 | | | 53.0 %; 0, 0 (1 178) |

The users on N1-G's stream read the plain arm's within 3 mm in every bin and hold about as much, with the same unresolved comparisons; no user reaches a sustained 5 cm on either stream. The controls reproduce card IRDU 12.1 and 12.5 to the digit.

**Verdict.** Every bar of N1 holds: the ring fix took only right integers on this bed (11 links, 777 held link-epochs, 0 wrong), its audit sees the negative control's wrong holds, and it is byte-identical when off. It also holds far too few links in 2 h (11 of 186, from 40 min) to move the stream or the users: both read the plain arm's level, where Q1, with every link held, reads the truth level at every depth from the first half hour. On this bed the limit is the floats' precision against the gate (formal sigma p50 0.12 to 0.20 cycle by ring at 2 h, against the bound 0.10) and the ring order (27 050 link-epochs not yet candidates), not wrong integers. The prediction holds for the count (a few by 60 min, more in hour 2) and fails for any gain in hour 2.

**What it points to, for the author's decision (the frozen rule is unchanged).** (a) Stage N3, the 6 h bed, where the floats keep converging and the count keeps rising (19 links pass the gate at 119.5 min against 7 at 90 min on the plain arm); (b) the gate at 0.15 cycle, which took 0 wrong integers at every stage in card IRDU 12.5 (b) and accepts several times more; (c) the ring order relaxed to the best connected links first (card IRDU 12.3, Q3); (d) stage N2 (r3c with its re-seats), next by this card's plan. N2's pre-registration and build wait for the requesting session's or the author's word, since N1's gain is nil and (a) to (c) change what N2 would measure.

**Not verified.** One bed, 2 h. The release path on true events (a slip, a cut release or a root change on a held link) is exercised only by the negative control (monitor, RAIM refloats, three slips) and the unit tests. The narrow-lane differences of N1-G in the second hour are not traced. The HELD readings use the fixing control's resolver, not this branch's.

### 10. 2026-11-15: stage N1b, two changes on the same bed: the order and the gate (pre-registered before the runs)

**Assigned** by the requesting session after its re-grade of N1, which reproduced section 9 from `network_ring_fix.csv` and the frame file (777 held link-epochs, 0 wrong; the frozen gate log-only 0, 4, 7 and 19 links at 30, 60, 90 and 119.5 min, 0 wrong; plain rounding of the floats right on 173, 183 and 186 of 186 links at 60, 90 and 119.5 min). The author is asleep; the decisions below are the requesting session's and are listed for the author's review.

**The two findings behind the changes, as the requesting session states them.**
1. The ring order holds back 27 050 link-epochs. The author's idea is that the integer frame grows outward from the reference receiver; Q2 (card IRDU 12.3) showed what a held ring buys, not that a decision must wait for a whole inner ring, and one slow inner link blocks everything behind it.
2. A gate on the odds accepts about five times more with the same safety on this bed. Log-only on N1-G's trace (the requesting session's `nrf_regrade.py`): with the likelihood ratio between the nearest and the next integer of N_1 at least 1e6, (1 - 2 d) / (2 sigma^2) >= ln(1e6) with d the fraction, and d at most 0.20, the links passing are 4, 17, 50 and 92 at 30, 60, 90 and 119.5 min, 6 827 passing row-epochs, 0 wrong; at 1e4, 9, 27, 89 and 126 links and 10 661 row-epochs, 0 wrong; at 1e9, 2, 8, 28 and 51, 0 wrong.

**Change O, the order** (key `network.ring_fix.order`, default `ring`: section 4.1 unchanged; `certain`: change O).
- A link is a candidate whenever it is a chord of its pair family with a float. Rings are computed and reported (trace, run-end, the audit's ring completion) and not enforced.
- Within an epoch, the links that pass every test (the gate, the window, the lattice veto, the persistence count) are decided in the order of their certainty, the log-odds of N_1, (1 - 2 d) / (2 sigma^2), highest first, and at most **3** per epoch. A link passed over by this cap keeps its persistence count and is refused this epoch with the cause `cap`; it is decided at a later epoch if it still passes. The next solve, which the earlier holds already enter, decides on the later links.

**Change L, the gate** (key `network.ring_fix.gate`, default `sigma`: section 4.1 unchanged; `odds`: change L). In place of the sigma bound:
- N_1 (the ionosphere-free row with the wide lane rounded, as in 4.1): (1 - 2 d) / (2 sigma^2) >= ln(1e6) = 13.8155, with d = |n - round(n)| and sigma its formal sigma; and d at most 0.20 (the fraction bound kept).
- The wide lane's own float and sigma, before it is rounded: (1 - 2 d_WL) / (2 sigma_WL^2) >= ln(1e6), with d_WL = |(f1 - f2) - round(f1 - f2)| and sigma_WL^2 = q11 + q22 - 2 q12.
- A link that fails either is refused with the cause `odds`, one whose N_1 fraction exceeds 0.20 with the cause `fraction`. The window, the persistence count, the lattice veto, the hold, every release and the audit are unchanged.

**Arms** (the bed and base of section 5; Release, a new build of this branch):
- **N1b-O:** change O with the frozen gate.
- **N1b-OL:** changes O and L.
- The comparison is the frozen rule as built (N1-G, section 9); the controls are the plain chain arm and Q1.
- Gates before the graded arms: the new build with the key off reproduces the previous build whole on both controls, and with the key on under the defaults (order `ring`, gate `sigma`) reproduces N1-G whole; each new arm is smoked first (10 epochs).

**Reads.** Links held against time and by ring, and the minute each ring is complete; wrong holds; releases by cause; refusals by cause; the formal sigmas of the links not yet held, by ring, against the plain arm (the feedback); the stream against truth by depth in 15 min bins and card IRD's W-WL and W-NL over hour 2; the users (the five of card IRDU, the users' build) from 30 min and one row from the start, float by 15 min bins and fixing (HELD, WRONG, UNRESOLVED).

**Bars.**
- **N1b-B1:** 0 wrong held integers against truth at every epoch, per arm. An arm with any wrong hold is rejected, not adjusted: no number of the rule is tuned after the runs.
- **N1b-B2:** every release named and decided = released + held, per arm; 240 of 240 epochs emitted.
- **N1b-B0:** the two identity gates above IDENTICAL.

**Prediction** (the requesting session's, to be tested): with O and L about half of the links (about 93 of 186) are held by 2 h, the stream's narrow lane at depths 3 and 5 moves toward Q1's, and the users hold more than on the plain arm.

**Then N2**, as soon as N1b's runs are graded and reported, on the arm or arms of N1b that took 0 wrong: section 4.4 built (held integers through re-seats, root changes, returning satellites, splits and merges, re-expressed exactly where the path is held and released where it is not), r3c and the event beds with the truth-fed run on r3c as the ceiling, and a steady-state harness (the oracle holds every link until a cut-over at 30 min, after which every link born, re-seated or released is decided by the ring fix alone). N2 is pre-registered in its own section before its runs.

**Decisions in the author's absence, for review.** The requesting session's: both changes and their numbers (the odds at 1e6, deliberately not 1e4; the same rule on the wide lane; the fraction bound kept; the frozen gate kept for N1b-O); the arms; no tuning after the runs; N2 without waiting for the re-grade. This card's: (1) the cap of 3 decisions per epoch; (2) the certainty that orders them is the log-odds of N_1 in both arms; (3) a link passed over by the cap keeps its count; (4) under change O the trace's candidate column reads 1 for every link with a float; (5) the run-end line prints the counts of the causes `odds` and `cap` only on a run that arms the change that owns them, so a run under the defaults prints what it printed before.

### 11. 2026-11-15: stage N1b results: both arms take 0 wrong; the order with the odds gate holds 170 of 186 links by 2 h and brings the stream's narrow lane to Q1's level; its users fix worse

**What ran.** Build of commit `efcf888` (exe `gnss_processor_irdu_nrf3.exe`, sha256[:16] `c7e594ca105d3d53`): `_rap_nrf_o_nr3` (N1b-O, change O with the frozen gate) and `_rap_nrf_ol_nr3` (N1b-OL, changes O and L); the identity runs `_rap_nrf3_g_nr3` (the defaults, key on), `_rap_nrf3off_{zw,q1}_nr3` (key off) and `_rap_nrf2_g_nr3` (the defaults on the previous build, `gnss_processor_irdu_nrf2.exe`); the smoke `_rap_nrf_smk_ol_nr3`. Every run exit 0, 240 of 240 epochs. Users: 40 replays on the two new streams (five users, float and fixing, from 0 and 30 min) on the users' build (`gnss_processor_irdu_users_rscb3.exe`), read beside section 9's. Grades `runs_nom/_irdu_grades/nrf_n1b_*`.

**N1b-B0: PASSES.** Key off, against the previous build (pv1): IDENTICAL whole on both controls. Key on under the defaults, against the same arm on the previous build of this card (nrf2): IDENTICAL whole. Against N1-G as run (nrf1) the defaults differ in one log line only: the first-epoch "contributions as armed" line, which since commit `335235e` names the hold kind once it has folded a row, and at the first emitted epoch no row had folded.

**N1b-B1, 0 wrong holds: PASSES on both arms.** N1b-O: 19 links decided (ring 2: 10, ring 3: 6, ring 5: 3), 1 036 held link-epochs, 0 wrong. N1b-OL: 170 links decided (ring 2: 27, ring 3: 64, ring 4: 37, ring 5: 42), 12 194 held link-epochs, 0 wrong. The code's audit, the independent re-grade against the frame file and the re-grade against truth per epoch (`nrf_audit.py --fixture`) agree to the count.

**N1b-B2: PASSES.** No release on either arm (a quiet bed); decided = released + held (19 = 0 + 19; 170 = 0 + 170); 240 of 240 epochs emitted.

**Links held against time** (minutes from the run's first epoch):

| arm | first hold, rings 2 / 3 / 4 / 5 | held at 30 | 60 | 90 | 119.5 min | ring complete |
| --- | --- | --- | --- | --- | --- | --- |
| N1-G (the frozen rule) | 40.5 / 97.5 / - / - | 0 | 3 | 5 | 11 | none |
| N1b-O | 40.5 / 85.0 / - / 92.0 | 0 | 3 | 6 | 19 | none |
| N1b-OL | 20.5 / 54.5 / 39.0 / 66.0 | 3 | 16 | 105 | 170 | ring 2 at 85.0 min |

Refusals, link-epochs: N1b-O window 3 749, sigma 38 522, persistence 193, fraction 5; N1b-OL window 3 749, odds 13 834, fraction 11 800, persistence 1 667, cap 110. Change O alone adds 8 links in 2 h: with the order free, the frozen gate's sigma bound is what holds the links back. With change L the odds rule accepts links whose formal sigma is above 0.10 when their fraction is small (the 16 links not held at 119.5 min have sigma p50 0.215 to 0.279 by ring).

**Feedback** (the links not yet held: formal sigma of N_1, p50 by ring, and how many round right):

| arm, time | held | not held, rounding right | sigma p50, rings 2 / 3 / 4 / 5 |
| --- | --- | --- | --- |
| plain chain arm, 90 min | 0 | 186, 184 | 0.166 / 0.209 / 0.262 / 0.239 |
| N1b-OL, 90 min | 105 | 81, 81 | - / 0.198 / 0.215 / 0.192 |
| plain chain arm, 119.5 min | 0 | 186, 186 | 0.123 / 0.158 / 0.202 / 0.197 |
| N1b-OL, 119.5 min | 170 | 16, 16 | - / 0.279 / 0.215 / 0.278 |

**Stream** (per-pair offset p50 against truth by depth class, cycles; N1b-OL / N1b-O / plain chain arm / Q1):

| channel, depth | 15 to 30 min | 45 to 60 min | 60 to 75 min | 75 to 90 min | 90 to 105 min | 105 to 120 min |
| --- | --- | --- | --- | --- | --- | --- |
| D_NL, 1 | 0.079 / 0.080 / 0.080 / 0.021 | 0.063 / 0.058 / 0.045 / 0.016 | 0.052 / 0.047 / 0.048 / 0.020 | 0.015 / 0.060 / 0.055 / 0.010 | 0.016 / 0.036 / 0.048 / 0.015 | 0.018 / 0.038 / 0.023 / 0.025 |
| D_NL, 3 | 0.152 / 0.155 / 0.155 / 0.016 | 0.119 / 0.107 / 0.105 / 0.019 | 0.094 / 0.094 / 0.094 / 0.015 | 0.037 / 0.130 / 0.097 / 0.016 | 0.027 / 0.063 / 0.065 / 0.017 | 0.013 / 0.055 / 0.048 / 0.013 |
| D_NL, 5 | 0.174 / 0.184 / 0.184 / 0.030 | 0.110 / 0.103 / 0.102 / 0.028 | 0.115 / 0.092 / 0.091 / 0.015 | 0.114 / 0.163 / 0.142 / 0.017 | 0.032 / 0.100 / 0.101 / 0.019 | 0.022 / 0.070 / 0.075 / 0.015 |
| D_WL, 1 | 0.142 / 0.131 / 0.131 / 0.079 | 0.084 / 0.071 / 0.071 / 0.056 | 0.043 / 0.058 / 0.035 / 0.031 | 0.035 / 0.065 / 0.058 / 0.027 | 0.046 / 0.067 / 0.041 / 0.043 | 0.032 / 0.048 / 0.037 / 0.028 |
| D_WL, 3 | 0.128 / 0.131 / 0.131 / 0.085 | 0.083 / 0.087 / 0.087 / 0.062 | 0.054 / 0.065 / 0.064 / 0.051 | 0.048 / 0.065 / 0.067 / 0.056 | 0.052 / 0.063 / 0.068 / 0.052 | 0.048 / 0.049 / 0.052 / 0.049 |
| D_WL, 5 | 0.124 / 0.123 / 0.123 / 0.069 | 0.090 / 0.091 / 0.087 / 0.052 | 0.079 / 0.080 / 0.081 / 0.051 | 0.050 / 0.091 / 0.095 / 0.050 | 0.054 / 0.081 / 0.088 / 0.052 | 0.049 / 0.061 / 0.073 / 0.048 |

Card IRD's stream bars over hour 2: N1b-OL D_WL p50 0.052 with 98.9 % within 0.20 and D_NL p50 0.036 with 95.5 % within 0.25; N1b-O 0.065 (96.6 %) and 0.084 (91.3 %); the plain arm 0.067 (96.2 %) and 0.081 (92.9 %); Q1 0.051 (100 %) and 0.020 (100 %). From 75 min, as the held links pass half of the network, N1b-OL's narrow lane comes to Q1's level at depths 1 and 3 (0.013 against 0.013 at depth 3 in the last bin) and close to it at depth 5 (0.022 against 0.015), where the plain arm reads 0.048 and 0.075; its wide lane comes to Q1's at depths 3 and 5. In the first hour it reads the plain arm's level, slightly worse in some cells (all depths at 45 to 60 min: 0.111 against 0.100). The stream's declared steps are unchanged (1 789 against 1 798 on the plain arm), and its step p95 per bin is no larger than the plain arm's.

**Users** (the users' build; float 3D median over the users by 15 min from each user's start, mm; HELD; WRONG, UNRESOLVED of the fixing users):

| stream, start | 45-60 | 60-75 | 75-90 | 90-105 | 105-120 | sustained 5 cm (min) | HELD; WRONG, UNRESOLVED (compared) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| N1b-OL, 0 | 289 | 276 | 225 | 76 | 82 | SZZ 74.0, S32 93.0 | 7.9 %; 2, 54 (126) |
| N1b-O, 0 | 287 | 320 | 236 | 81 | 84 | none | 24.9 %; 0, 108 (108) |
| plain chain arm, 0 | 286 | 317 | 236 | 84 | 86 | none | 24.5 %; 0, 106 (106) |
| Q1, 0 | 209 | 165 | 115 | 109 | 106 | SZZ 58.0 | 50.2 %; 0, 0 (1 357) |
| N1b-OL, 30 | 211 | 74 | 81 | | | SZZ 43.0, S32 65.5 | 8.0 %; 6, 78 (110) |
| N1b-O, 30 | 222 | 89 | 98 | | | none | 31.4 %; 0, 104 (104) |
| plain chain arm, 30 | 221 | 86 | 96 | | | none | 26.4 %; 0, 104 (104) |
| Q1, 30 | 101 | 119 | 119 | | | none | 53.0 %; 0, 0 (1 178) |

On N1b-OL's stream the float users are a little better in the second hour (76 to 82 mm against 84 to 86 from the start; 74 to 81 against 86 to 96 from 30 min) and two of the five reach a sustained 5 cm, where none does on the plain arm. The fixing users hold three times less (8 % against 25 %), and S40000SIM, started at 30 min, holds three wrong pair integers (G05-G15, G05-G23, G15-G23, off by 1 to 2 cycles) in its first half hour, 6 wrong pair-epochs; from the start, 2. On the plain arm and on N1b-O no user integer is wrong. Not traced. The candidate: in the second hour the stream moves from the float's level to the integer level as links are held (the narrow lane by up to 0.1 cycle per satellite pair between 60 and 105 min), in small undeclared steps, so a user that fixes during that move fixes on a stream that keeps moving; Q1, held from its start, does not move.

**Verdict.** Every bar of N1b holds on both arms: 0 wrong network holds (1 036 and 12 194 held link-epochs), both edges closed, byte-identical when off and under the defaults. Change O alone barely moves the count (19 links). Changes O and L together hold 170 of 186 links by 2 h (more than the predicted half), complete ring 2 at 85 min, and bring the stream's narrow lane to Q1's level at depths 1 and 3 and near it at depth 5 in the second hour, passing the stream bars with margin (D_NL 0.036 over hour 2 against 0.081). The prediction holds for the count and the stream and FAILS for the users: they hold less, with wrong fixes at one user, while the stream moves to the integer level.

**For N2.** N1b-OL took 0 wrong network holds; N2 is built on it (and N1b-O, also 0 wrong, is kept as its control). The users' loss during the move to the integer level is the question N2's steady-state harness asks from the other side: whether a stream that is already integer stays integer, and still, without moving under the users.

**Not verified.** One bed, 2 h; no release occurred on either arm (the release paths are covered by N1-P and the unit tests); the users' wrong fixes and their lower HELD share are not traced.

### 12. 2026-11-15: stage N2, held integers through the chain's events, the event beds and a steady state (pre-registered before the runs)

**Assigned** by the requesting session with N1b: start N2 as soon as N1b is graded, on the arm or arms of N1b that took 0 wrong, without waiting for its re-grade. Both N1b arms took 0 wrong; N2 is built on N1b-OL (changes O and L), the arm that holds links, with N1b-O beside it where a bed is cheap.

**What is built** (default off; byte-identical when off and under the keys of N1b; ledger rows UNVALIDATED).

1. **Held integers through a cut release** (section 4.4 built; it replaces N1's component-wide chain release). A cut release of the chain (the returning, re-host, slip and merge rows) re-attaches a moved side M through a released edge e' with the target k' of this epoch. Inside M the tree keeps its edges and targets, so every vertex of M moves by one integer frame step delta per band. When e' was a held link with integer N_h before the event (not latched this epoch, its monitor not tripped), the step is known: delta = N_h - k' when e''s satellite is the moved vertex, k' - N_h when its receiver is. A held link with exactly one end in M is re-expressed exactly: its integer moves by -delta when its satellite is in M and by +delta when its receiver is. When delta is not known, such a link is released (cause chain). A held link with both ends in M, or none, keeps its integer. A held link that becomes a tree edge leaves the held set without a release (the chain pins it); its carried entries are not re-opened.
2. **The old host is inherited.** A tree edge of the previous epoch that is a chord of its pair family this epoch is held at once at its previous target, moved by the steps of the cuts it crosses, when every such step is known and the link is not latched; otherwise it waits for the rule like any other link.
3. **The cut release's erase is not a refloat.** A cut release erases the carried entries of the links that touch M, and the next harvest re-creates them with a new re-level stamp; a held link that touches M takes the new stamps as its baseline at the next epoch instead of being released for a refloat.
4. **A root handover** that the root row pinned at the carried receiver phase bias keeps the frame, so the held links keep their integers; one left as built (no carry, not a vertex, not the keeper) releases the held links of its component.
5. **Truth in the code** (simulation only, key `network.ring_fix.truth_root`, default empty): each link's integer in the frame of every epoch, from the chord's fundamental cycle of that epoch and the fixture's integers (`truth_ambiguities.csv`, `truth_slips.csv`) with the producer's wind-up branch of each arc (`truth_windup.csv` at the arc's first epoch, a new arc after a gap above 300 s: the rule of `scripts/simnet_producer_ar_grade.py`). It drives an audit of every held integer at every epoch (run-end counts, the trace's audit column), and is checked against the frame file of the bed without re-seats (every chord, every epoch).
6. **An oracle phase** (simulation only, key `network.ring_fix.oracle_until_s`, default 0): until that many seconds after the run's first epoch every chord with a true integer is held at it (decided at once, its integer set again at every epoch); afterwards the run's rule alone decides, and what is held carries on through items 1 to 4.
7. Unit tests of the cut algebra (known step, unknown step, both ends inside), the inheritance, the stamp rebase, the root rule, the branch rule and the truth integer of a cycle.

**Beds and arms** (2 h each; the exe of this build; each new arm smoked first).
- **N2-r3c:** N1b-OL on r3c (re-seats every 100 s at the median, returns, the fixture's one true slip), the truth audit on.
- **N2-ceiling:** r3c with the oracle phase for the whole run (every link at its true integer all run, the former queued all-integers run).
- **N2-steady:** r3c with the oracle phase for the first 30 min, then N1b-OL alone.
- **Event beds** (the IRD fixtures; N1b-OL with the truth audit; the plain chain arm of each bed beside it): a true slip on a tree edge with a large subtree (`ird_t2`), on the root's own edge (`ird_tr`) and on a chord (`ird_t3`); a returning interior vertex (`ird_rv16`); a root handover (`ird_rg`); a split and merge (`ird_ms`, `ird_msx`).
- **Gates:** the key off reproduces the previous build whole on both controls of the bed without re-seats; N1b-OL on that bed reproduces section 11's run whole (no cut release occurs there), and the truth model matches its frame file at every epoch.

**Bars.**
- **N2-B1:** 0 wrong held integers against truth at every epoch, on every arm and bed (the code's truth audit and the independent re-grade `nrf_audit.py --fixture` agree).
- **N2-B2:** every held link on a slipped arc is released at the slip's epoch; every held link that crosses a cut with an unknown step is released at the cut's epoch; every re-expression and inheritance is an event row with its link, its step and the cut's row.
- **N2-B3:** card IRD's I-b at the chain events of the event beds (the truth step of every undeclared pair at most 0.02 cycle per band, 0.05 on the wide lane), read beside the bed's plain chain arm.
- **N2-B4:** every epoch emitted on every arm.

**Reads.** Held links against time and by ring; releases and re-expressions by cause and row; on N2-steady, for every link born, re-seated or released after the cut-over: the minutes from its birth to its hold (median and p90, by ring), the share of link-epochs held, wrong holds; the stream against truth by depth against N2-ceiling and the plain chain arm; the users (the users' build, from 30 min and one start-up row) against N2-ceiling.

**Prediction.** Re-seats onto held links keep the frame (card IRDU Q4: the target of a re-host onto a link at its integer is that integer), so on N2-steady most re-seats re-express with delta 0 and the old host is inherited; the links born after the cut-over are held within tens of minutes as their floats converge against the held network; the held share stays high and the stream near N2-ceiling's level. A wrong hold, if one appears, comes from a re-seat onto a link whose integer was not held (a step not known), which the rule releases rather than re-expresses.

**Decisions in the author's absence, for review** (this card's): (1) N2 on N1b-OL; (2) the step of a cut is known only through a held released edge, never through a float; (3) the old host inherited only when every crossing step is known; (4) the stamp rebase after a cut release; (5) the root rule (pinned keeps, as built releases); (6) truth in the code from the fixture with the producer's wind-up branch, sim only; (7) the oracle phase as the harness's warm start and as the ceiling; (8) the beds chosen from the IRD fixtures.

The results of stage N2 (section 13) are in [part 2](NRF_network_ring_fixing_2026-11-14_part2.md).
