"""Network dashboard: an interactive page of one network run, in layers a reader clicks through.

    py -3.14 scripts/netview/netview.py build <run folder> [-o page.html] [--no-compress]
    py -3.14 scripts/netview/netview.py arm <config.json> -o <armed config.json> [--no-map]
    py -3.14 scripts/netview/netview.py compare <page A.html> <page B.html> -o <compare.html>
    py -3.14 scripts/netview/netview.py live <run folder, live record or built page> [--speed N] [--port 8770] [--start HH:MM]

build   reads the run folder's traces (logs/*.csv, logs/network_run_manifest.json and products.txt) and writes one
        HTML page that opens by double-click from file:// (no server, no network access).  Default output:
        <run folder>/netview.html.  The page has layers (overview, network links, receivers, satellites, cycle
        slips, stochastic model, atmosphere, products and user check, events) and pages of one receiver, one
        satellite and one line of sight.  Parts whose trace is absent are left out and named on the page with the
        key that writes them.
arm     writes a copy of a run config with the trace keys the page needs switched on (traces only: nothing that
        the estimate reads).  Normal runs leave them off and pay nothing; a run made from the armed copy can be
        viewed.
compare writes a page that shows two built pages side by side (for example the same day from two reference
        receivers); time, signals, layout and the selected link or node follow each other.
live    serves the live monitor on 127.0.0.1: a rolling window of the network.  A run folder whose engine wrote the
        live record (network.trace_live_record: logs/network_live.jsonl), or that file itself, is followed as the
        engine writes it, a running engine included; a built page, or a run folder without the record (or with
        --replay), is replayed as a live engine would send it.

The page, the quantities and the cost of the traces: docs/onboarding/network_view.md.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import nv_arm  # noqa: E402
import nv_build  # noqa: E402
import nv_page  # noqa: E402


def cmd_build(a) -> int:
    run = Path(a.run)
    if not run.is_dir():
        print(f'{run}: not a folder', file=sys.stderr)
        return 2
    out = Path(a.output) if a.output else run / 'netview.html'
    t = time.time()
    print(f'reading {run}')
    blocks = nv_build.build_blocks(run)
    header = blocks['core'][0]
    sizes = nv_page.write_page(out, blocks, compress=not a.no_compress)
    raw = sum(v['raw'] for k, v in sizes.items() if k != 'page')
    print(f'wrote {out}: page {sizes["page"] / 1e6:.1f} MB (data {raw / 1e6:.1f} MB before compression), '
          f'{header["n_ep"]} epochs, {len(header["stations"])} receivers, {len(header["sats"])} satellites, '
          f'{len(header["families"])} signal families, {time.time() - t:.1f} s')
    print('  blocks (MB in the page): ' + ', '.join(f'{k} {v["encoded"] / 1e6:.1f}' for k, v in sizes.items() if k != 'page'))
    for m in header['missing']:
        print(f'  not in this run: {m["part"]} ({m["file"]}; key {m["key"]})')
    return 0


def cmd_live(a) -> int:
    from live import follow, server, telemetry
    src = Path(a.source)
    # A .jsonl argument is the record itself; a run folder is followed when its engine wrote the record, or when
    # it has no products yet (an engine that is starting); a finished run without the record is replayed.
    record = src if src.suffix.lower() == '.jsonl' else src / 'logs' / 'network_live.jsonl'
    starting = src.is_dir() and not (src / 'products.txt').exists()
    if src.suffix.lower() == '.jsonl' or (src.is_dir() and (record.is_file() or starting) and not a.replay):
        return _live_follow(a, follow, server, record)
    if src.is_file() and src.suffix.lower() in ('.html', '.htm'):
        print(f'reading the blocks of {src}', flush=True)
        blocks, archive = nv_page.read_page(src), src
    elif src.is_dir():
        print(f'reading {src} (a built page opens faster: netview.py live <page.html>)')
        blocks, archive = nv_build.build_blocks(src), None
    else:
        print(f'{src}: neither a run folder nor a built page', file=sys.stderr)
        return 2
    source = telemetry.ReplaySource(blocks, archive=archive is not None)
    start = 0
    if a.start:
        if ':' in a.start:                                     # a GPS time of day, HH:MM
            hh, mm = (int(x) for x in a.start.split(':')[:2])
            tod = ((source.abs_ms // 1000) % 86400)
            start = int(min(range(source.n_ep), key=lambda e: abs(int(tod[e]) - (hh * 3600 + mm * 60))))
        else:
            start = int(a.start)
    html = nv_page.live_page_html(blocks['core'][0]['run'])
    speed = a.speed if a.speed is not None else 1.0
    srv, replay, _clock = server.serve(source, html, port=a.port, window=a.window, speed=speed, start=start, archive=archive)
    print(f'live monitor of {blocks["core"][0]["run"]}: http://127.0.0.1:{a.port}/   '
          f'(replaying {source.n_ep} epochs of {source.step_s:g} s from epoch {start}, {speed:g} epochs/s; Ctrl+C stops)', flush=True)
    return _serve_until_stopped(srv, replay)


def _live_follow(a, follow, server, record: Path) -> int:
    """The live monitor on an engine's live record: the newest window first, then each epoch as it is written."""
    source = follow.FollowSource(record)
    source.poll()
    start = source.n_ep                                        # the newest window
    if a.start:
        if ':' in a.start:                                     # a GPS time of day, HH:MM
            hh, mm = (int(x) for x in a.start.split(':')[:2])
            want = hh * 3600 + mm * 60
            tods = [(r['ms'] // 1000) % 86400 for r in source.records]
            start = min(range(len(tods)), key=lambda e: abs(tods[e] - want)) if tods else 0
        else:
            start = int(a.start)
    html = nv_page.live_page_html(source.run)
    speed = a.speed if a.speed is not None else 50.0
    srv, replay, _clock = server.serve(source, html, port=a.port, window=a.window, speed=speed, start=start)
    state = f'{source.n_ep} epochs so far' if source.header else 'waiting for the engine to write its header'
    print(f'live monitor of {source.run}: http://127.0.0.1:{a.port}/   (following {record}; {state}; '
          f'catching up at {speed:g} epochs/s; Ctrl+C stops)', flush=True)
    return _serve_until_stopped(srv, replay)


def _serve_until_stopped(srv, replay) -> int:
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        replay.stopped = True
        srv.server_close()
    return 0


def cmd_arm(a) -> int:
    changed, notes = nv_arm.arm(Path(a.config), Path(a.output), with_map=not a.no_map)
    print(f'wrote {a.output}')
    for k, old, new in changed:
        print(f'  {k}: {old} -> {new}')
    if not changed:
        print('  every key the page needs was already on')
    for n in notes:
        print(f'  note: {n}')
    return 0


def cmd_compare(a) -> int:
    pa, pb = Path(a.page_a), Path(a.page_b)
    for p in (pa, pb):
        if not p.is_file():
            print(f'{p}: no such page (build it first)', file=sys.stderr)
            return 2
    nv_page.write_compare(Path(a.output), pa, pb)
    print(f'wrote {a.output}: {pa.name} beside {pb.name} (keep the three files in their folders)')
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='write the page of one run folder')
    b.add_argument('run')
    b.add_argument('-o', '--output', help='page path (default <run>/netview.html)')
    b.add_argument('--no-compress', action='store_true', help='store the data block uncompressed')
    b.set_defaults(fn=cmd_build)
    r = sub.add_parser('arm', help='copy a run config with the traces the page needs switched on')
    r.add_argument('config')
    r.add_argument('-o', '--output', required=True)
    r.add_argument('--no-map', action='store_true', help='leave the ionosphere trace (satellite positions) as it is')
    r.set_defaults(fn=cmd_arm)
    lv = sub.add_parser('live', help='the live monitor: follow an engine\'s live record, or replay a finished run, in a local page')
    lv.add_argument('source', help='a run folder, its logs/network_live.jsonl, or a built page')
    lv.add_argument('--port', type=int, default=8770)
    lv.add_argument('--speed', type=float, default=None,
                    help='epochs released per second (default: 1 for a replay; 50 while a followed record catches up)')
    lv.add_argument('--window', type=int, default=240, help='epochs the page keeps (default 240)')
    lv.add_argument('--start', help='first epoch: an index or a GPS time of day HH:MM (default: a replay from its '
                                    'start, a followed record at its newest epoch)')
    lv.add_argument('--replay', action='store_true', help='replay a run folder from its traces even when it has a live record')
    lv.set_defaults(fn=cmd_live)
    c = sub.add_parser('compare', help='a page showing two built pages side by side, time and selection linked')
    c.add_argument('page_a')
    c.add_argument('page_b')
    c.add_argument('-o', '--output', required=True)
    c.set_defaults(fn=cmd_compare)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == '__main__':
    sys.exit(main())
