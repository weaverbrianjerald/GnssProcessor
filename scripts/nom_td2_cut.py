#!/usr/bin/env python3
"""Card NOM2 step TD2: one run-end report per subsystem (the cut, run once on nom/wave3).

Step TD1b left the run end as 106 rows in print order, one row per contiguous run of one
owner, so an owner that printed at several places held several rows.  This cut gives each
owner (the file a report lives in) one row:

  * MERGES: an owner's rows become file-local sections (`static`, bodies verbatim, their
    Doxygen moved from the header to the definition) of one subsystem report, declared in
    the owner's header, whose body is the list of its sections in their old print order
    (networkRunEndSections, filter/NetworkRunEnd.hpp).  The report takes the row of its
    anchor section; the other sections' lines move up (or down) to it.
  * MOVES: two reports move whole beside their harvests (card NOM3 HV1 design section 6):
    the phase-colour state to filter/NetworkPhaseColourCarry.cpp, the seam-scoped ZWD
    re-seed to filter/NetworkZwdCarry.cpp.  Their rows keep their place.
  * SPLIT: the receiver-clock jump-gate line leaves the leveling-carry retirement report for
    a new report in filter/NetworkRxClockCarry.cpp, on its own row after the leveling carry.
  * The table (filter/NetworkRunEnd.cpp) is rewritten from the old one by the same lists.

The printed text of every line is unchanged; the order changes by subsystem.  The run-end
walk prints one heading line per report that prints (filter/NetworkRunEnd.cpp, written by
hand).  scripts/nom_td2_verify.py proves the cut against the tree at BASE.

Usage: python scripts/nom_td2_cut.py      (edits the tree in place; run once)
"""
import os
import re
import sys
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'core')
INC = os.path.join(ROOT, 'include')

# (report, home under src/core, row name, anchor section, [(section, new name or None)])
MERGES = [
    ('networkEmitStoreRunEnd', 'filter/NetworkEmitStore.cpp',
     'the emit store: epochs emitted, skipped and withheld; heartbeats of a writer that never '
     'opened; the product contract round trip (B6)',
     'networkEmitOutcomeRunEnd',
     [('networkEmitOutcomeRunEnd', None), ('networkDeferredHeartbeatsRunEnd', None),
      ('networkProductContractRunEnd', None)]),
    ('networkRaimRunEnd', 'filter/NetworkRaim.cpp',
     'RAIM: the carried-phase pools and the re-host, the withhold era, the R2 station factor, '
     'the R4 rate hygiene',
     'networkRaimPoolRunEnd',
     [('networkRaimPoolRunEnd', None), ('networkRaimWithholdRunEnd', None)]),
    ('networkGrossLosRunEnd', 'filter/NetworkGrossLos.cpp',
     'the gross line-of-sight screen and its cold-start bound',
     'networkGrossLosRunEnd',
     [('networkGrossLosRunEnd', 'networkGrossLosScreenRunEnd'),
      ('networkGrossLosColdBoundRunEnd', None)]),
    ('networkInvariantCarryDriverRunEnd', 'filter/NetworkInvariantCarryDriver.cpp',
     'the invariant carry driver: the PD-safe DD fold, the card 4/M1 carry, the D5 gate and '
     'its map misses',
     'networkCarryDdFoldRunEnd',
     [('networkCarryDdFoldRunEnd', None), ('networkInvariantCarryRunEnd', None),
      ('networkD5GateRunEnd', None), ('networkD5MapMissRunEnd', None)]),
    ('networkSlipQcRunEnd', 'slip/NetworkSlipQcRunEnd.cpp',
     'slip QC: MW and LLI detections, re-level, wind-up, anchor-slip handovers, MW admission, '
     'IF-TD pairs and re-seed, datum stations, gaps, re-entry and the IF-TD reference gate',
     'networkSlipMwRelevelRunEnd',
     [('networkSlipMwRelevelRunEnd', None), ('networkSlipWindupMwRunEnd', None),
      ('networkSlipBandResponsesRunEnd', None), ('networkIfTdReseedRunEnd', None),
      ('networkSlipDatumStationRunEnd', None), ('networkSlipGapRefGateRunEnd', None),
      ('networkSlipGapCoresumedShareRunEnd', None)]),
    ('networkLevelingCarryRunEnd', 'filter/NetworkLevelingCarryRunEnd.cpp',
     'the phase-leveling carry: retirement families [A] to [D], the F3 local block, the response',
     'networkLevelingRetirementRunEnd',
     [('networkLevelingRetirementRunEnd', None), ('networkLevelingResponseRunEnd', None)]),
    ('networkResumeRunEnd', 'app/NetworkResumeRunEnd.cpp',
     'warm resume: restores (frame, coupling regressors, dense border), then resume and '
     'snapshot (RTB-7, RS-2 to RS-9, the coupling capture)',
     'networkResumeRestoreRunEnd',
     [('networkResumeRestoreRunEnd', None), ('networkResumeSnapshotRunEnd', None)]),
    ('networkSbasisRunEnd', 'filter/NetworkSbasis.cpp',
     "the S-basis datum arms and card GFC's code-datum arms",
     'networkSbasisDatumRunEnd',
     [('networkSbasisDatumRunEnd', None), ('networkCodeDatumFamilyRunEnd', None)]),
    ('networkFrameDiagRunEnd', 'filter/NetworkFrameDiag.cpp',
     'frame diagnostics: the post-fit sigma0 and rows, the shared-normal spectrum trace',
     'networkPostfitRunEnd',
     [('networkPostfitRunEnd', None), ('networkShSpectrumRunEnd', None)]),
    ('networkPhaseDatumRunEnd', 'filter/NetworkPhaseDatum.cpp',
     'the phase datum: the held S-basis (RF-2), the prioritized drop and heal, the '
     'anchored-union gate and its drop edge',
     'networkHeldSbasisRunEnd',
     [('networkHeldSbasisRunEnd', None), ('networkDropHealRunEnd', None),
      ('networkPhaseDatumGatesRunEnd', None)]),
    ('networkHandoverStransformRunEnd', 'filter/NetworkHandoverStransform.cpp',
     'satellite health (HLT) and clock-pivot handovers with their S-transform',
     'networkSatHealthRunEnd',
     [('networkSatHealthRunEnd', None), ('networkPivotHandoverRunEnd', None)]),
    ('networkSolPrepRunEnd', 'filter/NetworkSolPrep.cpp',
     'solution prep: the clock-runaway quarantine (RT0-5), the per-constellation sigma0 '
     'carve-out (RTA-10)',
     'networkClockRunawayRunEnd',
     [('networkClockRunawayRunEnd', None), ('networkPerConstellationRunEnd', None)]),
    ('networkCouplingRunEnd', 'filter/NetworkCoupling.cpp',
     'the coupling carry (cards 1, 1d), reinit sources (1g) and gauge projection (1f)',
     'networkCouplingRunEnd',
     [('networkCouplingRunEnd', 'networkCouplingCarryRunEnd'),
      ('networkCouplingProjectionRunEnd', None)]),
    # Anchored at the later section: its manifest record keeps its place among the records.
    ('networkCarryDispositionRunEnd', 'filter/NetworkCarryDisposition.cpp',
     "the pivot-frame clock noise (PFQ) and the fold's dispositions of the carried border "
     "(PRM 1b)",
     'networkCarryDispositionRunEnd',
     [('networkPivotFrameQRunEnd', None),
      ('networkCarryDispositionRunEnd', 'networkCarryDispositionTableRunEnd')]),
    ('networkParamInventoryRunEnd', 'filter/NetworkParamInventory.cpp',
     'the receiver extra-band code delay (card L5B) and parameters as armed (NOM N1)',
     'networkParamInventoryRunEnd',
     [('networkRxExtraBandDelayRunEnd', None),
      ('networkParamInventoryRunEnd', 'networkParamInventoryTableRunEnd')]),
    # Anchored at the plan: its manifest record precedes the complete record.
    ('networkSignalRosterRunEnd', 'app/SignalRoster.cpp',
     'the signal roster: signals as planned (NOM N3a), the roster and the arc ledger '
     '(SID phase 1)',
     'networkSignalRosterPlanRunEnd',
     [('networkSignalRosterPlanRunEnd', None),
      ('networkSignalRosterRunEnd', 'networkSignalRosterLedgerRunEnd')]),
]

# (report, from .cpp, to .cpp, new row name or None); headers are the include/ twins.
MOVES = [
    ('networkPhaseColourRunEnd', 'filter/NetworkLevelingCarryRunEnd.cpp',
     'filter/NetworkPhaseColourCarry.cpp', None),
    ('networkZwdSeamRunEnd', 'app/NetworkResumeRunEnd.cpp', 'filter/NetworkZwdCarry.cpp', None),
]

# The split: the jump-gate block of the leveling-carry retirement section.
SPLIT_FROM = ('networkLevelingRetirementRunEnd', 'filter/NetworkLevelingCarryRunEnd.cpp')
SPLIT_FIRST = "    // The jump gate's scale in metres"
SPLIT_REPORT = ('networkRxClockCarryRunEnd', 'filter/NetworkRxClockCarry.cpp',
                "the receiver-clock carry's jump gate (F3 block)")

# Rows placed right after another report's row.
AFTER = {
    'networkLevelingCarryRunEnd': 'networkRxClockCarryRunEnd',
    'networkSignalRosterRunEnd': 'networkSignalRosterSitesRunEnd',   # beside the roster (N3b)
}


EOL = {}   # path -> the file's line ending, restored on write


def rd(p):
    with open(p, encoding='utf-8', newline='') as f:
        s = f.read()
    EOL.setdefault(p, '\r\n' if '\r\n' in s else '\n')
    return s.replace('\r\n', '\n')


def wr(p, s):
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(s.replace('\n', EOL.get(p, '\n')))


def hpp_of(cpp):
    return os.path.join(INC, cpp[:-4] + '.hpp')


def block_end(s, i):
    """Index just past the brace that closes the block opened at s[i] == '{' (skips string
    and character literals and comments)."""
    d, k, n = 0, i, len(s)
    while k < n:
        c = s[k]
        if c == '/' and s.startswith('//', k):
            k = s.index('\n', k)
            continue
        if c == '/' and s.startswith('/*', k):
            k = s.index('*/', k) + 2
            continue
        if c in '"\'':
            k += 1
            while s[k] != c:
                k += 2 if s[k] == '\\' else 1
            k += 1
            continue
        if c == '{':
            d += 1
        elif c == '}':
            d -= 1
            if d == 0:
                return k + 1
        k += 1
    raise ValueError('unbalanced')


def find_def(s, name):
    """(start of the definition's line, end of its closing-brace line) of the view-form report."""
    m = re.search(r'^[ \t]*(?:static )?int ' + name + r'\(const NetworkRunEndView& v\) \{',
                  s, re.M)
    if not m:
        raise KeyError(name)
    e = block_end(s, m.end() - 1)
    e = s.index('\n', e) + 1
    return m.start(), e


def comment_above(s, start):
    """Start of the contiguous // comment block ending just above `start` (or `start`)."""
    lines = s[:start].split('\n')[:-1]
    k = len(lines)
    while k > 0 and lines[k - 1].lstrip().startswith('//'):
        k -= 1
    return len('\n'.join(lines[:k])) + (1 if k else 0)


def find_decl(h, name):
    """(start, end) of the Doxygen block and declaration of the view-form report in a header."""
    m = re.search(r'^[ \t]*int ' + name + r'\(const NetworkRunEndView& v\);\n', h, re.M)
    if not m:
        raise KeyError(name)
    b = h.rfind('/**', 0, m.start())
    b = h.rfind('\n', 0, b) + 1
    assert '*/' in h[b:m.start()] and h[b:m.start()].count('/**') == 1, name
    return b, m.end()


def brief(block):
    m = re.search(r'@brief Run-end report: (.*?)\n[ \t]*\*\s*\n', block, re.S)
    return ' '.join(x.strip(' *') for x in m.group(1).split('\n')).strip()


def ensure_include(s, inc, after):
    line = f'#include "{inc}"\n'
    if line in s:
        return s
    k = s.index(f'#include "{after}"\n') + len(f'#include "{after}"\n')
    return s[:k] + line + s[k:]


def merge(report, home, name, anchor, sections):
    cpp, hpp = os.path.join(SRC, home), hpp_of(home)
    s, h = rd(cpp), rd(hpp)
    # Header: every section's block out; the report's block in at the first one's place.
    spans = [find_decl(h, sec) for sec, _ in sections]
    blocks = [h[b:e] for b, e in spans]
    first = min(b for b, _ in spans)
    ind = re.match(r'[ \t]*', h[first:]).group(0)
    for b, e in sorted(spans, reverse=True):
        h = h[:b] + (h[e + 1:] if h[e:e + 1] == '\n' else h[e:])
    lines = [f'{ind}/**']
    lines += textwrap.wrap(f'@brief Run-end report: {name}.', width=100,
                           initial_indent=f'{ind} * ', subsequent_indent=f'{ind} *        ')
    lines += [f'{ind} *']
    lines += textwrap.wrap(
        'The subsystem\'s one row of kNetworkRunEndReports (card NOM2 step TD2).  Its sections, '
        f'defined beside it in {os.path.basename(home)}, print in this order, each gated on its '
        'own knob:', width=100, initial_indent=f'{ind} * ', subsequent_indent=f'{ind} * ')
    for (sec, new), blk in zip(sections, blocks):
        lines += textwrap.wrap(f'{new or sec}: {brief(blk)}', width=100,
                               initial_indent=f'{ind} *   - ', subsequent_indent=f'{ind} *     ')
    lines += [f'{ind} *',
              f'{ind} * @param v The run at its end (filter/NetworkRunEnd.hpp).',
              f'{ind} * @return 0, or the last non-zero verdict of its sections.',
              f'{ind} */',
              f'{ind}int {report}(const NetworkRunEndView& v);', '']
    h = h[:first] + '\n'.join(lines) + '\n' + h[first:]
    # Source: each section static (renamed where its name is the report's), its Doxygen above it.
    for (sec, new), blk in zip(sections, blocks):
        st, en = find_def(s, sec)
        cb = comment_above(s, st)
        doc = blk[:blk.rindex('\n', 0, len(blk) - 1) + 1]          # the block without the decl
        doc = doc.replace('@brief Run-end report:', '@brief Run-end section:')
        # The one-line "// Run-end report: ..." comment (and any lines under it) gives way to
        # the Doxygen block; a marker comment above it stays.
        k = s.rfind('// Run-end report:', cb, st)
        cut = s.rfind('\n', 0, k) + 1 if k >= 0 else st
        sig = s[st:s.index('\n', st)]
        sig2 = sig.replace(f'int {sec}(', f'static int {new or sec}(', 1)
        s = s[:cut] + doc + s[st:en].replace(sig, sig2, 1) + s[en:]
    last_end = max(find_def(s, new or sec)[1] for sec, new in sections)
    ind = '    ' if home == 'app/SignalRoster.cpp' else ''
    names = [new or sec for sec, new in sections]
    fn = [f'',
          f'{ind}// Run-end report of this subsystem: its sections in print order (card NOM2 step '
          f'TD2;',
          f'{ind}// declared in {os.path.relpath(hpp, INC).replace(os.sep, "/")}).',
          f'{ind}int {report}(const NetworkRunEndView& v) {{',
          f'{ind}    return networkRunEndSections(v, {{']
    fn += [f'{ind}        {n},' for n in names]
    fn[-1] = fn[-1].rstrip(',')
    fn += [f'{ind}    }});', f'{ind}}}', '']
    s = s[:last_end] + '\n'.join(fn) + s[last_end:]
    s = ensure_include(s, 'filter/NetworkRunEnd.hpp', os.path.relpath(hpp, INC).replace(os.sep, '/'))
    wr(cpp, s)
    wr(hpp, h)


def insert_before_namespace_end(s, text):
    k = s.rindex('} // namespace GnssProcessor')
    return s[:k] + text + s[k:]


def add_forward_decl(h):
    if 'struct NetworkRunEndView;' in h:
        return h
    k = h.index('namespace GnssProcessor {\n') + len('namespace GnssProcessor {\n')
    return h[:k] + '\nstruct NetworkRunEndView;\n' + h[k:]


def move(report, frm, to):
    s, d = rd(os.path.join(SRC, frm)), rd(os.path.join(SRC, to))
    hs, hd = rd(hpp_of(frm)), rd(hpp_of(to))
    st, en = find_def(s, report)
    cb = comment_above(s, st)
    fn = s[cb:en]
    s = s[:cb] + s[en:].lstrip('\n') if s[en:].startswith('\n') else s[:cb] + s[en:]
    b, e = find_decl(hs, report)
    decl = hs[b:e]
    hs = hs[:b] + hs[e:].lstrip('\n') if hs[e:].startswith('\n') else hs[:b] + hs[e:]
    d = insert_before_namespace_end(
        d, f'// ---- Run-end report (card NOM2 step TD2: moved here from {frm}, beside the '
           f'harvest) ----\n\n' + fn + '\n')
    d = ensure_include(d, 'filter/NetworkRunEnd.hpp', to[:-4] + '.hpp')
    hd = add_forward_decl(hd)
    hd = insert_before_namespace_end(hd, decl + '\n')
    wr(os.path.join(SRC, frm), s)
    wr(os.path.join(SRC, to), d)
    wr(hpp_of(frm), hs)
    wr(hpp_of(to), hd)


def split():
    sec, frm = SPLIT_FROM
    report, to, _ = SPLIT_REPORT
    s = rd(os.path.join(SRC, frm))
    st, en = find_def(s, sec)
    body = s[st:en]
    a = body.index(SPLIT_FIRST)
    z = body.rindex('    return 0;\n')
    moved = body[a:z]
    s = s[:st] + body[:a] + body[z:] + s[en:]
    wr(os.path.join(SRC, frm), s)
    fn = ('// ---- Run-end report (card NOM2 step TD2: the jump-gate line, moved here from\n'
          f'//      {frm}, beside the harvest) ----\n\n'
          "// Run-end report: the receiver-clock carry's jump gate (network.carry_rx_clock_in_block).\n"
          f'int {report}(const NetworkRunEndView& v) {{\n'
          '    const GnssConfig& config_ = v.config;\n'
          '    const std::shared_ptr<spdlog::logger>& log = v.log;\n'
          '    const auto& C = v.ctx.counters;\n\n'
          + moved + '    return 0;\n}\n\n')
    d = rd(os.path.join(SRC, to))
    d = insert_before_namespace_end(d, fn)
    d = ensure_include(d, 'filter/NetworkRunEnd.hpp', to[:-4] + '.hpp')
    wr(os.path.join(SRC, to), d)
    hd = add_forward_decl(rd(hpp_of(to)))
    hd = insert_before_namespace_end(hd, (
        '/**\n'
        " * @brief Run-end report: the receiver-clock carry's jump gate, in metres (card NOM2 step\n"
        ' *        TD2: moved from the leveling-carry retirement report).\n'
        ' *\n'
        ' * The gate k sigma over the station-epoch tests, its mean, minimum and maximum in metres,\n'
        ' * the largest |innovation| seen and the refusals, so a given step reads as above or\n'
        ' * below the gate.\n'
        ' *\n'
        ' * Gate: `config_.networkCarryRxClockInBlock()`.\n'
        ' *\n'
        ' * @param v The run at its end (filter/NetworkRunEnd.hpp).\n'
        ' * @return 0 (this report does not set the exit code).\n'
        ' */\n'
        f'int {report}(const NetworkRunEndView& v);\n\n'))
    wr(hpp_of(to), hd)


def table():
    p = os.path.join(SRC, 'filter', 'NetworkRunEnd.cpp')
    s = rd(p)
    row_re = re.compile(r'    \{"((?:[^"\\]|\\.)*)",\n     "([^"]+)", (\w+)\},\n')
    rows = row_re.findall(s)
    if any(f == 'networkSlipQcRunEnd' for _, _, f in rows):
        return rows                                    # already regrouped
    to_report = {sec: (rep, home, name, anchor)
                 for rep, home, name, anchor, secs in MERGES for sec, _ in secs}
    moved = {r: to for r, _, to, _ in MOVES}
    old = {f: (n, h) for n, h, f in rows}
    extra = {'networkRxClockCarryRunEnd': (SPLIT_REPORT[2], SPLIT_REPORT[1])}
    out = []
    for n, h, f in rows:
        if f in AFTER.values() and f not in to_report:
            continue                                   # placed by AFTER
        if f in to_report:
            rep, home, name, anchor = to_report[f]
            if f != anchor:
                continue
            out.append((name, home, rep))
        elif f in moved:
            out.append((n, moved[f], f))
        else:
            out.append((n, h, f))
        last = out[-1][2]
        if last in AFTER:
            g = AFTER[last]
            nn, hh = extra.get(g) or old[g]
            out.append((nn, hh, g))
    body = ''.join(f'    {{"{n}",\n     "{h}", {f}}},\n' for n, h, f in out)
    a = row_re.search(s).start()
    z = list(row_re.finditer(s))[-1].end()
    s = s[:a] + body + s[z:]
    own = s.index('#include "filter/NetworkRunEnd.hpp"\n') + 1   # the file's own header: skip
    for inc in ('filter/NetworkPhaseColourCarry.hpp', 'filter/NetworkRxClockCarry.hpp',
                'filter/NetworkZwdCarry.hpp'):
        if f'#include "{inc}"\n' not in s:
            incs = [m for m in re.finditer(r'#include "(filter/[^"]+)"\n', s) if m.start() > own]
            pos = next((m.start() for m in incs if m.group(1) > inc), incs[-1].end())
            s = s[:pos] + f'#include "{inc}"\n' + s[pos:]
    wr(p, s)
    hp = os.path.join(INC, 'filter', 'NetworkRunEnd.hpp')
    h = rd(hp)
    h = re.sub(r'kNetworkRunEndReportCount = \d+;', f'kNetworkRunEndReportCount = {len(out)};', h)
    wr(hp, h)
    return out


def repair_slip_header():
    """A declared doc repair.  In include/slip/NetworkSlipQcRunEnd.hpp the IF-TD re-seed's
    Doxygen block had no close, so it ran on through the co-resumed share's block and both
    declarations followed the one comment.  Each block is closed and put above its own
    declaration, so the cut can move each to its section."""
    p = os.path.join(INC, 'slip', 'NetworkSlipQcRunEnd.hpp')
    h = rd(p)
    a = (" * nothing otherwise, so an unarmed run's log is unchanged.\n\n/**\n"
         " * @brief Run-end report: the co-resumed share")
    b = ("*/\nint networkIfTdReseedRunEnd(const NetworkRunEndView& v);\n\n"
         "int networkSlipGapCoresumedShareRunEnd")
    if h.count(a) != 1 or h.count(b) != 1:
        return
    h = h.replace(a, " * nothing otherwise, so an unarmed run's log is unchanged.\n *\n"
                     " * @param v The run at its end (filter/NetworkRunEnd.hpp).\n"
                     " * @return 0 (this report does not set the exit code).\n */\n"
                     "int networkIfTdReseedRunEnd(const NetworkRunEndView& v);\n\n/**\n"
                     " * @brief Run-end report: the co-resumed share")
    h = h.replace(b, "*/\nint networkSlipGapCoresumedShareRunEnd")
    wr(p, h)


def main():
    repair_slip_header()
    for rep, home, name, anchor, secs in MERGES:
        merge(rep, home, name, anchor, secs)
    for rep, frm, to, _ in MOVES:
        move(rep, frm, to)
    split()
    out = table()
    print(f'{len(MERGES)} subsystems merged, {len(MOVES)} reports moved, 1 split; '
          f'the table has {len(out)} rows')


if __name__ == '__main__':
    sys.exit(main())
