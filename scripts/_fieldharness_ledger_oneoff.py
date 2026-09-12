#!/usr/bin/env python3
"""FIELD HARNESS ledger pass (SPEC_event_harness s20) -- one command per outcome.

Records, for each of the three sim-validated slip cards, what the FIELD harness measured
and whether the field golden now PINS the knob or carries an EXCEPTION with the mechanism:

  python scripts/_fieldharness_ledger_oneoff.py \
      --evidence PIN|EXCEPTION "<field sentence>" \
      --scope    PIN|EXCEPTION "<field sentence>" \
      --pairs    PIN|EXCEPTION "<field sentence>"

Edits (in this order): docs/CONFIG_promotion_ledger.json (evidence prefixed with
'FIELD HARNESS (SPEC_event_harness s20): ...', armed_when_why rewritten from the
'field golden ...' clause on), docs/CONFIG_promotion_ledger.md (the evidence and
armed-when bullets of the same three sections), config/golden/golden_manifest.json
(golden_field_gps_dual: PIN = add to pins + drop the exception; EXCEPTION = rewrite the
exception text with the measured mechanism).  Then run, in this order:
  python scripts/make_golden_config.py            (regenerate)
  python scripts/make_golden_config.py --verify
  python scripts/config_posture_check.py --verify-loader
The scope and the pairs are pinned TOGETHER or not at all (the veto is the scope's
coverage) -- the script refuses a split outcome.  Posture rows need no change: the
existing Tier::ON rows' regime (phase_leveling && mw_detect) already covers the field.
"""
import argparse
import io
import json

ROOT = 'C:/GnssProcessor/'
KNOBS = {
    'evidence': 'network.cycle_slip.mw_evidence_refloat',
    'scope':    'network.cycle_slip.common_band_scope',
    'pairs':    'network.cycle_slip.if_phase_td_pairs',
}
PIN_VALUE = {'evidence': True, 'scope': True, 'pairs': 'all'}
SIM_WHY = {
    'evidence': ("phase_leveling lineages running the MW detector (the sim golden).  Arm together with "
                 "network.cycle_slip.anchor_slip_handover where a prioritized anchor election exists: on an anchor arc "
                 "this knob supplies the detection and the handover supplies the cure.  Knob-on legs against a knob-on "
                 "null (the quiet null is byte-identical either way, but a dosed leg is not).  "),
    'scope':    ("phase_leveling lineages running the MW detector, together with mw_evidence_refloat, "
                 "if_phase_td_pairs = all and (where an anchor election exists) anchor_slip_handover (the sim golden).  "),
    'pairs':    ("phase_leveling lineages running the MW and IF-TD detectors together with "
                 "network.cycle_slip.common_band_scope (the sim golden).  "),
}


def main():
    ap = argparse.ArgumentParser()
    for k in KNOBS:
        ap.add_argument('--' + k, nargs=2, required=True, metavar=('PIN|EXCEPTION', 'SENTENCE'))
    a = ap.parse_args()
    out = {k: getattr(a, k) for k in KNOBS}
    for k, (v, _) in out.items():
        assert v in ('PIN', 'EXCEPTION'), k
    assert out['scope'][0] == out['pairs'][0], 'scope and pairs are pinned TOGETHER or not at all'

    # ---------------- ledger JSON ----------------
    p = ROOT + 'docs/CONFIG_promotion_ledger.json'
    L = json.load(io.open(p, encoding='utf-8'))
    rows = {r['knob']: r for r in L['knobs']}
    for k, (v, sentence) in out.items():
        r = rows[KNOBS[k]]
        tag = 'FIELD HARNESS (SPEC_event_harness s20, 2026-09-12): ' + ('FIELD PIN -- ' if v == 'PIN' else 'FIELD EXCEPTION -- ') + sentence
        if 'FIELD HARNESS (SPEC_event_harness s20' not in r['evidence']:
            r['evidence'] = tag + '  ' + r['evidence']
        field_why = (('the field golden (golden_field_gps_dual) PINS it since 2026-09-12: ' if v == 'PIN'
                      else 'the field golden carries a documented EXCEPTION (2026-09-12): ') + sentence)
        r['armed_when_why'] = SIM_WHY[k] + field_why
        print('JSON:', KNOBS[k], v)
    json.dump(L, io.open(p, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)

    # ---------------- ledger md ----------------
    p = ROOT + 'docs/CONFIG_promotion_ledger.md'
    s = io.open(p, encoding='utf-8').read()
    for k, (v, sentence) in out.items():
        r = rows[KNOBS[k]]
        head = '### `' + KNOBS[k] + '`'
        i = s.find(head); assert i > 0, head
        j = s.find('\n### ', i + 10)
        sec = s[i:j]
        lines = sec.split('\n')
        for n, line in enumerate(lines):
            if line.startswith('- **evidence:** ') and 'FIELD HARNESS (SPEC_event_harness s20' not in line:
                lines[n] = '- **evidence:** ' + r['evidence']
            if line.startswith('- **armed when:** '):
                lines[n] = '- **armed when:** `' + r['armed_when'] + '`  ' + r['armed_when_why']
        s = s[:i] + '\n'.join(lines) + s[j:]
        print('md:', KNOBS[k], 'updated')
    io.open(p, 'w', encoding='utf-8').write(s)

    # ---------------- manifest: field pins / exceptions ----------------
    p = ROOT + 'config/golden/golden_manifest.json'
    m = json.load(io.open(p, encoding='utf-8'))
    g = next(e for e in m['goldens'] if e['name'] == 'golden_field_gps_dual')
    ex = g.setdefault('exceptions', {})
    for k, (v, sentence) in out.items():
        knob = KNOBS[k]
        if v == 'PIN':
            g['pins'][knob] = PIN_VALUE[k]
            ex.pop(knob, None)
        else:
            g['pins'].pop(knob, None)
            ex[knob] = 'FIELD HARNESS EXCEPTION (SPEC_event_harness s20, 2026-09-12): ' + sentence
        print('manifest:', knob, v)
    json.dump(m, io.open(p, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print('done -- now regenerate + --verify + config_posture_check.py --verify-loader')


if __name__ == '__main__':
    main()
