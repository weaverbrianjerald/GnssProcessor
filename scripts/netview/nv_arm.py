"""The arm command: a copy of a run config with the trace keys the page needs switched on.

Only trace keys are touched (files written for reading, nothing the estimate reads).  The tree traces are written
only on a run with the chain datum (network.phase_datum_eq34); the held whole numbers only on a run with the
fixing (network.ring_fix.enable).  Neither is switched on here: they change the estimate, so a config without them
gets a note instead.
"""
from __future__ import annotations

import json
from pathlib import Path

#: The trace keys of the page, in the network section, with the part of the page each feeds.
TRACE_KEYS = [
    ('trace_chain_datum', 'tree and links (network_chain_edges.csv, network_chain_frame.csv)'),
    ('trace_events', 'event timeline (network_events.csv)'),
    ('trace_slip_events', 'alarms (network_raim.csv)'),
    ('trace_stations', 'receiver positions (network_stations.csv)'),
    ('trace_zwd', 'wet delay (network_zwd.csv)'),
    ('trace_rx_states', 'receiver clock (network_rx_states.csv)'),
    ('trace_rpb_states', 'receiver phase bias (network_rpb_states.csv)'),
    ('trace_live_record', "the live monitor's record, one line per epoch (network_live.jsonl)"),
]
MAP_KEY = ('trace_ionosphere', 'satellite positions for the map (network_ionosphere.csv)')


def arm(src: Path, dst: Path, with_map: bool = True):
    """Write dst = src with the page's trace keys on.  Returns (changes [(key, old, new)], notes)."""
    cfg = json.loads(src.read_text(encoding='utf-8'))
    net = cfg.setdefault('network', {})
    keys = TRACE_KEYS + ([MAP_KEY] if with_map else [])
    changed = []
    for k, _what in keys:
        if net.get(k) is not True:
            changed.append((f'network.{k}', net.get(k), True))
            net[k] = True
    notes = []
    if not net.get('phase_datum_eq34'):
        notes.append('network.phase_datum_eq34 is off: the tree and link traces are written only on a chain run '
                     '(the page then shows events, receivers and corrections only)')
    rf = net.get('ring_fix') or {}
    if not rf.get('enable'):
        notes.append('network.ring_fix.enable is off: links show floats only (no held whole numbers)')
    if src.resolve() == dst.resolve():
        raise SystemExit('arm: the output must be a new file (the source config is left as it is)')
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(json.dumps(cfg, indent=2) + '\n', encoding='utf-8')
    return changed, notes
