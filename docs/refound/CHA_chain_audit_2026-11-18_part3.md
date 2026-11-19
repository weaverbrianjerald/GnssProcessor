# CHA part 3: the refactor from step S8 on (card CHA, continued)

**STATUS**

- **id:** CHA-part3
- **topic:** network-vehicle
- **status:** open
- **summary:** The results of card CHA's refactor from step S8 on (2026-11-18), continued from [part 2](CHA_chain_audit_2026-11-18_part2.md), which reached its size. The order is the manager's of 2026-11-18 (the author's word: the saved state moved up): S8 (a), S8 (b), S9, S7 (a), S7 (b), the native build of the phase family (released by the author's adoption of the single Galileo network, card NRF section 123), S10, S11, milestone D, S12. The gate of an identical-output step is three 2 h controls (n147, f40sc, h40) with the control with the entry check on (n147e) from S8 on, the chain unit cases and the two lints; the full control set, the Debug suite and a whole-day run at milestones C (after S8 (b)) and D. Sections: steps S8 (a) and S8 (b); milestone C; rule change 11 (a causal group-delay latch).
- **source:** [part 2](CHA_chain_audit_2026-11-18_part2.md) (the plan, sections 5.1 and 5.2, and the defects P1 to P18 after S1 to S6); [NRF part 17](NRF_network_ring_fixing_2026-11-14_part17.md) (sections 121 and 122, pre-registered); the manager's messages of 2026-11-18 (the order of record, the run-time amendment of 14:05, the order change of 14:15).

**Outcome.** Steps S8 (a) and S8 (b) are built. S8 (a) gives a held entry's step one owner, `chainEntryStep`, with the rules of the ring fix's cut, the chain's returning row and keep-carry side by side; it moves nothing. S8 (b) makes keep-carry ask the cut's entry check when `network.ring_fix.entry_revalidate` is on, on evidence gathered before the election; off it moves nothing, and on the observed control with the check on it checked 172 kept re-hosts in 2 h and refused none. S8 (b)'s graded runs (card NRF section 122) belong to the second session. Rule change 11, a causal group-delay latch so that a short run is a prefix of a long one, joins part 2's list; it waits for the author's word.

## Results

### 2026-11-18: steps S8 (a) and S8 (b)

| step | commit | lines | gate | what moved |
| --- | --- | --- | --- | --- |
| S8 (a) | `8d71321` | +81 / -14, 5 files | pass: n147, f40sc and h40 identical in full to the runs of the key's engine (`_rap_fp1_*`, logs included); n147e (the entry check on) identical to the base engine's reference `_rap_chaR_n147e` in products and every trace, the logs apart from the lines S1 and S2 announced | nothing |
| S8 (b) | `12ecc70` | +181 / -42, 6 files | pass: n147, f40sc and h40 identical in full to S8 (a)'s runs; n147e identical to S8 (a)'s run in products and every trace | with the check on only: the run-start line of the entry re-validation (it names the keep-carry path) and a run-end line; on n147e, 172 kept re-hosts checked, 172 kept, 0 refused |

Engines: `gnss_processor_chas8a.exe` (sha256[:16] `635ccaa00bec9a66`) and `gnss_processor_chas8b.exe` (`0a0399f07765017e`), frozen for S8 (b)'s graded runs as `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_cha_s8a.exe` and `gnss_processor_cha_s8b.exe`. The chain and loader unit cases pass at each step (314 at S8 (a), 315 at S8 (b)), and so do `nom_epoch_lint.py` and `nom_n4_verify.py --whole`. The control with the entry check on, n147e, is the 2 h observed control of DOY 147 with `network.ring_fix.entry_revalidate` on; its reference ran on the base engine cha2 (`72b7b3b`), so S8 (a) ties it to the start of the refactor.

**What each step did.**
- S8 (a). `chainEntryStep(kind, evidence, revalidate, max_if)` (`include/network/ar/RingFix.hpp`) owns whether a held link that enters the tree carries its integer as a known step, for the three mechanisms that ask it (`ChainEntryKind`): the ring fix's cut (`ringFixEntryVerdict`), the chain's returning row (never known: a new verdict `RowUnknown`, which the cut never returns) and keep-carry (known whenever the link is usable). The cut, `applyReturningRow` and `chainHeldInteger` read it. The evidence of each mechanism stays where it was gathered, so every verdict is the same.
- S8 (b). With `entry_revalidate` on, keep-carry takes the cut's rule. Its evidence is gathered before the election (`ringFixGatherKeepEvidence`, called in the phase anchor seam after PASS A): for every held link, this epoch's gap resumptions of the slip layer (its arc came back across a gap; its own pair measured, shifted) and its near-null signature from the ring fix's state at the epoch's start, where the cut reads this epoch's after the solve. A refused keep is released as without keep-carry. Counters `keep_checked`, `keep_known` and `keep_refused_*`, a run-end line, RINGFIX `keep_refused` events from the decision seam. The ledger row of `entry_revalidate` (RECOMMEND_SCOPED) records the widened reach; card NRF section 122 pre-registers its bars (B1 off identical; B2 the truth audit and the frame-free count on the beds m1, d1, d31, h1 and h40, at 2 h, then once at 6 h; B3 the observed control with the check on).

**Milestone C** (after S8 (b) is built): the rest of the full control set on the S8 (b) engine (n146, m1, d1, f40mc, u2z, gf, gs), the full Debug suite at S8 (b) and a whole-day run of the adopted configuration on DOY 147 against `_rap_d24_n147` were started at 16:50; their results follow below when they end.

### 2026-11-18: rule change 11, a causal group-delay latch (added to part 2's list "Rule changes the audit suggests")

11. A key, default off, that makes the RF-0c broadcast group-delay latch causal: each satellite's value from the navigation records in force at its first use, held from then on, with no look-ahead, so that a short run is a prefix of a long one. Not built without the author's word (the manager asked on 2026-11-18); if approved, it goes before S7 (b).

**Evidence** (the second session's read, card NRF section 124; the manager checked the code and the first lines of the runs). With `network.sat_gf_datum` broadcast_tgd, the setup latches each satellite's broadcast group delay once per run: the low median of its healthy navigation records whose clock epoch lies in [t0, t0 + `max_duration_s`) (`NetworkSbasis::latchBroadcastGroupDelays` in `src/core/network/solve/Sbasis.cpp`, called from `src/core/network/driver/ApplicationNetworkSetup.cpp` near line 710). Over 24 h against 5 h on DOY 146, 22 Galileo satellites latch differently by 0.07 to 0.49 m, and the first epoch's code-bias records differ by that amount (E04: −0.4188 m on the E1 codes, −0.7510 m on E5a). A 24 h configuration stopped at 45 epochs equals `d24_n146` over the first 20 epochs, data line for data line, and a 5 h configuration stopped likewise equals the 5 h run. This is card NRF section 102.1's untraced note (the first 2 h of a 24 h run differ from a 2 h run of the same configuration). A day run's latch reads navigation records up to a day ahead, which a streamed network cannot.

**Its bearing on S7 (b).** An uninterrupted run, a run that saves its state and a resumed run have three latch windows unless the latched values travel in the snapshot or the three runs share one window; S7 (b)'s pre-registration states how the latch is handled.
