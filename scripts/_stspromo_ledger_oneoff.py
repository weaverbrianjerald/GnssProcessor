#!/usr/bin/env python3
"""ONE-OFF promotion pass for `network.handover_stransform` (queue 1(b), increment 1 of
the pivot-departure card, SPEC_event_harness §19.6).

Mirrors scripts/_scopedpromo_ledger_oneoff.py, which did the same job for increment 0's
`network.scoped_handover_response`.  Kept in-tree for the same reason that one was: the
pass is the promotion STEP this project lacked, and a promotion that cannot be re-read
is a promotion that cannot be audited.

Four edits, all idempotent:
  1. ledger JSON  — verdict KEEP_OFF -> RECOMMEND_ON, an EXECUTABLE regime predicate
                    (the prose one a KEEP_OFF row is allowed to carry would throw the
                    moment the generator started evaluating it), and the decision
                    reasoning including WHY the two failed gates do not block.
  2. ledger .md   — the same, in the prose row.
  3. posture      — a Tier::ON row in GnssConfigNetworkPosture.cpp, predicate
                    `scoped_handover_response && a pivot clock datum`.
  4. manifest     — pin in golden_sim_tri_w6; documented exception in
                    golden_field_gps_dual (its prerequisite is itself a field exception).

Run from the worktree root, then regenerate + verify.
"""
import json
import collections
import sys

LEDGER_JSON = 'docs/CONFIG_promotion_ledger.json'
LEDGER_MD = 'docs/CONFIG_promotion_ledger.md'
POSTURE = 'src/core/config/GnssConfigNetworkPosture.cpp'
MANIFEST = 'config/golden/golden_manifest.json'
KNOB = 'network.handover_stransform'

REGIME = ('on("network.scoped_handover_response") '
          'and not on("network.clock_datum_sum_to_zero") '
          'and not on("network.clock_datum_broadcast_anchored")')

DECISION = (
    "PROMOTED 2026-09-12 on the following record.  Every WIRE gate passes at 250-575x "
    "back to control, OFF is byte-identical across TWO source revisions, section 8.7 gate 6 "
    "never fired, all producer truth channels move within +/-1.6 % of a +/-3 % bar, and the "
    "pooled consumer p50, the window-excluded p50, ZWD and L1C are all better or equal with "
    "the hourly profile identical -- no channel is worse beyond noise.  The two consumer "
    "EDGE gates that failed were MIS-SPECIFIED: they demanded an improvement AT the switch "
    "epoch, which increment 0 had already flattened (the scoped baseline reads 0.129 m the "
    "epoch before the GPS edge and 0.126 m at it -- there was no shock left to remove), and "
    "the unchanged switch-epoch reset COUNT is an emission property, not a carry property: "
    "the per-satellite disc bump marks phase, code and clock together, so a clock-only "
    "re-reference still resets the consumer's ambiguities.  That is pre-registered as "
    "INCREMENT 2 and is explicitly out of scope for section 8, which states that the emitted "
    "wire still steps by the datum delta BY DESIGN.")


def edit_ledger_json():
    raw = open(LEDGER_JSON, encoding='utf-8').read()
    d = json.loads(raw, object_pairs_hook=collections.OrderedDict)
    row = next(k for k in d['knobs'] if k['knob'] == KNOB)
    if row['verdict'] == 'RECOMMEND_ON':
        print('  ledger JSON: already promoted (idempotent no-op)')
        return
    row['verdict'] = 'RECOMMEND_ON'
    row['regime'] = REGIME
    row['regime_why'] = (
        "A handover only exists under the per-constellation satellite-clock PIN datum, so the "
        "two anchor-free datums are excluded exactly as they are for the scoped row.  Unlike "
        "that row there is NO n_sys >= 2 term: the transform acts on the CHANGED "
        "constellation's own carry, so a single constellation is enough for it to do work.  "
        "It does require scoped_handover_response, because releasing the changed "
        "constellation's skip while the global response still invalidates every sibling would "
        "not fix the edge.")
    row['evidence'] = DECISION + '  ' + row['evidence']
    row['posture_why'] = (
        "Sim-validated on two beds (quiet 643-epoch fresh pair, slipped 720-epoch storm arm), "
        "single knob, one exe, OFF byte-identical.  Field arm owed -- and blocked behind its "
        "own prerequisite, since scoped_handover_response is itself a field exception on the "
        "GPS-only field golden.")
    open(LEDGER_JSON, 'w', encoding='utf-8', newline='').write(
        json.dumps(d, indent=1, ensure_ascii=False) + '\n')
    print('  ledger JSON: verdict -> RECOMMEND_ON, regime made executable')


def edit_ledger_md():
    L = open(LEDGER_MD, encoding='utf-8').read().split('\n')
    i = next(k for k, s in enumerate(L) if s.startswith('### `' + KNOB + '`'))
    if 'PROMOTED' in L[i]:
        print('  ledger .md: already promoted (idempotent no-op)')
        return
    L[i] = ('### `network.handover_stransform`  (queue 1(b) — RE-EXPRESS the changed '
            'constellation\'s carry at a pivot handover instead of dropping it; '
            'PROMOTED 2026-09-12, SPEC_event_harness §19.6)')
    j = i + 1
    assert L[j].startswith('- **verdict:**'), L[j]
    L[j] = ('- **verdict:** **RECOMMEND_ON** — ' + DECISION.replace('+/-', '±').replace('250-575x', '250–575×')
            + '  — **default:** false (`src/core/config/GnssConfigLoadNetwork.cpp`, member '
              '`include/config/GnssConfig.hpp`) — **regime:** `' + REGIME + '` (no `n_sys >= 2` '
              'term: it acts on the changed constellation itself, so one suffices).')
    open(LEDGER_MD, 'w', encoding='utf-8', newline='').write('\n'.join(L))
    print('  ledger .md: heading + verdict line rewritten')


POSTURE_ROW = '''            // --- PIVOT DEPARTURE increment 1 (SPEC_event_harness §19.6, promoted
            //     2026-09-12).  Increment 0 above stops a handover invalidating the
            //     constellations whose datum did NOT move; this re-expresses the CHANGED
            //     constellation's own carry along the structural clock null direction and
            //     FOLDS it, instead of dropping it for one epoch.  Needs a pivot datum (no
            //     handover seam without one) and its own prerequisite; no n_sys term —
            //     it acts on the constellation that changed, so one is enough.
            //     Fingerprinted (armed-only token): fresh-convergence only.
            { network_scoped_handover_response_
                  && !network_clock_datum_sum_to_zero_
                  && !network_clock_datum_broadcast_anchored_,
              network_handover_stransform_,
              Tier::ON, true , "network.handover_stransform",
              "re-express the changed constellation's carry at a pivot handover instead of "
              "dropping it for an epoch: its own code-bias and phase-bias steps fall 250-575x "
              "back to control (storm arm G PB L1C 0.19914 -> 0.00079 m, G CB C5Q 0.16678 -> "
              "0.00029) and every follow-on consumer reset goes to zero, with ZWD 0.0066 -> "
              "0.0059, L1C 90.2 -> 90.4 and all truth contrasts within 1.6 % "
              "(runs/golden_sim_campaign_zwdblk_sts, exe B3A7E11B)" },
'''


def edit_posture():
    src = open(POSTURE, encoding='utf-8').read()
    if 'network.handover_stransform' in src:
        print('  posture: already present (idempotent no-op)')
        return
    anchor = '              "(runs/golden_sim_campaign_zwdblk_scoped, exe F4FF8EF0)" },\n'
    assert src.count(anchor) == 1, src.count(anchor)
    src = src.replace(anchor, anchor + POSTURE_ROW)
    open(POSTURE, 'w', encoding='utf-8', newline='').write(src)
    print('  posture: Tier::ON row added after the scoped row')


def edit_manifest():
    raw = open(MANIFEST, encoding='utf-8').read()
    d = json.loads(raw, object_pairs_hook=collections.OrderedDict)
    changed = False
    for g in d['goldens']:
        if g['name'] == 'golden_sim_tri_w6':
            if KNOB not in g['pins']:
                g['pins'][KNOB] = True
                changed = True
        elif g['name'] == 'golden_field_gps_dual':
            if KNOB not in g['exceptions']:
                g['exceptions'][KNOB] = (
                    "PREREQUISITE IS ITSELF A FIELD EXCEPTION: network.handover_stransform "
                    "requires network.scoped_handover_response, which is excepted on this "
                    "GPS-only lineage, so the regime predicate is false here and the knob is "
                    "out of regime rather than suppressed.  This entry is the record.  "
                    "Sim-validated on two beds (SPEC_event_harness §19.6); a FIELD ARM IS "
                    "OWED, and it is owed behind the prerequisite's field arm, not "
                    "independently of it.")
                changed = True
    if not changed:
        print('  manifest: already pinned/excepted (idempotent no-op)')
        return
    open(MANIFEST, 'w', encoding='utf-8', newline='').write(
        json.dumps(d, indent=2, ensure_ascii=False) + '\n')
    print('  manifest: pinned in golden_sim_tri_w6, exception recorded in golden_field_gps_dual')


if __name__ == '__main__':
    print('promotion pass for', KNOB)
    edit_ledger_json()
    edit_ledger_md()
    edit_posture()
    edit_manifest()
    json.load(open(LEDGER_JSON, encoding='utf-8'))
    json.load(open(MANIFEST, encoding='utf-8'))
    print('  both JSON files re-parse OK')
    sys.exit(0)
