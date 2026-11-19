# The grading folder's scripts and their owners here (card GRA, plan step G10)

The bars' scripts of the grading folder `D:\GnssProcessorSupport\runs_nom\_irdu_grades\scripts_s20` (S20), the safe
copy of an ended session's scratchpad `D:\GnssProcessorSupport\gates\irdu_handover\scratchpad_copy` (SPC) and the
managing session's scratch folder `D:\GnssProcessorSupport\gates\manager_2026-11-19` (MGR), with what owns each one in
`scripts/grading`. Nothing is deleted: the old folders stay as the record of the numbers they made. A script listed
here as owned, a copy or named nowhere is retired in the sense that no script of this folder calls it, which
`retire_listing.py` checks (`py -3.14 scripts/grading/retire_listing.py`). A script not listed stays the owner of its
read (card GRA part 2, results).

Status: **owned** (a view of the owner reproduces its recorded outputs), **partly owned** (the part named is
reproduced; the rest stays with the old script), **copy** (of the script named), **named nowhere** (card GRA, defect
D17: in no section of card NRF, no hand-over note and no other script).

| old script | folder | status | owner here, or the original |
| --- | --- | --- | --- |
| single_hourly.py | S20 | owned | network_hourly.py table (legacy) |
| d24_hourly.py | S20 | partly owned | network_hourly.py table --view d24 (the held, RAIM, epoch and withheld rows) |
| c4_compare.py | S20 | owned | network_hourly.py compare (legacy as of section 118.1; the unrounded rewrite = exact) |
| held_by_family.py | S20 | owned | network_hourly.py families |
| slip_hold_check.py | S20 | owned | slip_hold.py flagged (legacy) |
| slip_hold_check2.py | S20 | owned | slip_hold.py flagged --view lli (its counts) |
| slip_hold_check3.py | S20 | owned | slip_hold.py flagged --view check3 |
| grade_slip_bar.py | S20 | owned | slip_hold.py injected |
| injected_slips.py | S20 | owned | slip_hold.py truth |
| user_fix_vs_float_full.py | S20 | owned | users.py inside |
| user_fix_vs_float_full_sites.py | S20 | owned | users.py inside --sites |
| user_fix_vs_float_free.py | S20 | owned | users.py outside |
| user_fix_vs_float_free2.py | S20 | owned | users.py outside --view free2 |
| paired_users.py | S20 | owned | users.py paired |
| bar6_receivers.py | S20 | owned | users.py medians |
| ref_changes_full.py | S20 | owned | users.py refch |
| users_hourly2.py | SPC | owned | users.py hourly |
| users_group_hourly.py | S20 | partly owned | users.py hourly (the hourly medians; its mean of hours 3 to 24 is computed from users.replay_hourly) |
| o1_emulate.py | SPC | owned | users.py (the network's pins) |
| o2_audit.py | SPC | owned | users.py (the network's holds) |
| two_run_loops.py | S20 | partly owned | two_run.py --view bylink (each disagreeing link's line) |
| frame_free_count.py | S20 | owned | truth.py cover --order seed0 |
| grade_guard_sim.py | S20 | partly owned | truth.py runend, guard (the truth and cover lines; the guard's moves and the epochs' dispositions stay) |
| grade_entry_sim.py | S20 | partly owned | truth.py runend, cover (the truth and cover lines; the refusals by reason stay) |
| compare_runs.py | S20 | owned | compare_runs.py compare --view legacy |
| check_member_records.py | S20 | owned | compare_runs.py members --view check |
| grade_l12_parts.py | S20 | partly owned | compare_runs.py compare (its identity mode; its values mode stays) |
| gf_code_signature.py | S20 | owned | code_signature.py links (legacy: every number; its tie-ordered lists by content) |
| arc_signature_spread.py | S20 | owned | code_signature.py arcs |
| mode_code_trace.py | S20 | owned | code_signature.py modes |
| decision_split.py | MGR | owned | held_integrity.py onesigned (legacy) |
| onesigned_scan.py | MGR | owned | held_integrity.py onesigned --view exact (its population and columns) |
| window_monitor_replay.py | MGR | owned | held_integrity.py window (the trip on exact sums: one tie differs, card GRA) |
| user_fix_vs_float.py | SPC | copy | user_fix_vs_float_full.py |
| ref_changes.py | SPC | copy | ref_changes_full.py |
| grade_bar6_s2.sh | S20 | copy | grade_bar6.sh |
| grade_bar6_n41.sh | S20 | copy | grade_bar6.sh |
| derive_m10_users.py | S20 | copy | derive_users_for.py |
| derive_n41_users.py | S20 | copy | derive_users_for.py |
| derive_n46_users.py | S20 | copy | derive_users_for.py |
| derive_s1_users.py | S20 | copy | derive_users_for.py |
| derive_s2_users.py | S20 | copy | derive_users_for.py |
| hksl_trace_code.py | S20 | copy | hksl_trace.py |
| band_split_census2.py | S20 | copy | band_split_census.py |
| chain_cg_validate.sh | S20 | named nowhere | |
| chain_cg_validate2.sh | S20 | named nowhere | |
| chords_modes.py | S20 | named nowhere | |
| cq_trace5.py | S20 | named nowhere | |
| derive_n41_xusers.py | S20 | named nowhere | |
| e14_trace.py | S20 | named nowhere | (also a copy of e03_trace.py) |
| e1e5a_check.py | S20 | named nowhere | |
| grade_after_chains.sh | S20 | named nowhere | |
| grade_d146_full.sh | S20 | named nowhere | |
| grade_kc.sh | S20 | named nowhere | |
| grade_l12.sh | S20 | named nowhere | |
| grade_o1.sh | S20 | named nowhere | |
| grade_o2.sh | S20 | named nowhere | |
| grade_o2r.sh | S20 | named nowhere | |
| grade_o9.sh | S20 | named nowhere | |
| launch_o3o4.sh | S20 | named nowhere | |
| make_mode1_arms.sh | S20 | named nowhere | |
| make_o2_arms.py | S20 | named nowhere | |
| make_o2r_arms.py | S20 | named nowhere | |
| make_o3o4_arms.py | S20 | named nowhere | |
| make_o5_arms.py | S20 | named nowhere | |
| n2c_level.py | S20 | named nowhere | |
| n2c_read.py | S20 | named nowhere | |
| n2c_stream.py | S20 | named nowhere | |
| o1_int_changes.py | S20 | named nowhere | |
| o1_raw_classify.py | S20 | named nowhere | |
| prep_exact_epoch_time_adoption.py | S20 | named nowhere | |
| ring_bands.py | S20 | named nowhere | |
| sat_level_series.py | S20 | named nowhere | |
| sat_resid_by_sys.py | S20 | named nowhere | |
| self_audit.py | S20 | named nowhere | |
| trace_vs_truth.py | S20 | named nowhere | |
| users_chain_n41.sh | S20 | named nowhere | |
| users_chain_s2.sh | S20 | named nowhere | |

Not retired (each stays the owner of its read): `gps_share.py`, `slip_hold_spans.py`, `wl_off_list.py`,
`decision_cycles.py`, `aud_windows.py`, the 49 `grade_*.sh` orchestrators (kept frozen beside the outputs they made;
`grade.py` is their successor for new grades), `scripts/nrf_two_run_audit.py` (its wind-up options) and the other reads
of S20.
