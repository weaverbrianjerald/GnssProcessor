#!/usr/bin/env python3
"""Move the network-producer sources, headers and tests into their own directory tree.

The tree is include/network/<concern>/ and src/core/network/<concern>/ (mirroring each
other), with the network unit tests under tests/network/.  Every basename is kept, so
log lines (which print basenames) are unchanged.  The only content edits are the
#include strings of moved headers and the path in a Doxygen @file line.

Subcommands (all read the table scripts/netdir_map.tsv except --emit):
  --emit      derive the table from the concern rules below and write it
  --apply     git mv every row, then rewrite the includes and the CMakeLists.txt paths
  --check     prove the result: each moved file equals its base blob except include and
              @file lines, no stale include or path reference is left
  --survey    list files that stay in place but are included only by moved files
              (candidates for a later move) and Network* files left out of the table
  (the path edits of PATH_EDITS cover tests that open headers by path)
  --rewrite-refs FILE...
              update path references (full path, include string, or the same without
              the extension) in the given docs and live scripts, exact match only
  --emit and --survey read the tree as it was before the move; --apply and --check
  read only the table.

The base revision for --check is the merge base of HEAD and main (--base overrides).
"""
import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "scripts" / "netdir_map.tsv"
CODE_EXT = (".hpp", ".cpp", ".h", ".c", ".cc", ".inl")
SCOPE_DIRS = ("filter", "app", "slip", "logging")

# Concern -> stem patterns (fnmatch, case sensitive).  A file must match exactly one
# concern; the table build fails on zero or on several matches.
CONCERNS = {
    # Driver: the epoch loop, its setup, and the types the loop and its seams share.
    "driver": ["ApplicationNetwork*", "NetworkRun", "NetworkRunGroups", "NetworkRunTypes",
               "NetworkWlRunTypes", "NetworkEpoch", "NetworkEpochAdmission",
               "NetworkEpochObservers", "NetworkKnobs", "NetworkSetupResult",
               "NetworkStreamFrontEnd", "NetworkStreamAligner"],
    # State: run-lifetime state, snapshots, warm resume and the border stash.
    "state": ["NetworkRunContext", "NetworkEntities", "NetworkRunFields",
              "NetworkStateSnapshot*", "NetworkWarmResume", "NetworkSnapshotFingerprint",
              "NetworkBorderStash", "NetworkBorderPrior", "NetworkCarrySites",
              "NetworkCarryDisposition", "ArcLedger"],
    # Assembly: per-station rows, parameter tables, admissions and band scopes.
    "assembly": ["NetworkStationBuild", "NetworkStationSplit", "NetworkStationAnchors",
                 "NetworkStationCarry", "NetworkContributions", "NetworkContributionKind",
                 "NetworkParamInventory", "NetworkParamRegistry", "NetworkResponses",
                 "NetworkIonoConstraintRows", "NetworkMeasurementAdmission",
                 "NetworkBiasAdmission", "NetworkCanonicalPairAdmission",
                 "NetworkSignalIntegrity", "NetworkRowClassify", "NetworkSecondCodeScope",
                 "NetworkSecondPhaseScope", "NetworkExtraBandScope", "NetworkChannelAll",
                 "NetworkMultiGnss", "SignalRoster", "SignalRosterSites"],
    # Carry: what is carried across epochs and how it is coupled and injected.
    "carry": ["NetworkLevelingCarry*", "NetworkPhaseColourCarry", "NetworkSlantIonoCarry",
              "NetworkRxClockCarry", "NetworkRxCodeBiasCarry*", "NetworkZwdCarry",
              "NetworkLocalCarryHarvest", "NetworkInstallCarry",
              "NetworkInvariantCarryDriver", "NetworkDdPriorBuild", "NetworkInject",
              "NetworkCoupling*", "NetworkSharedHistory", "NetworkCarryVce*",
              "NetworkClockClone", "NetworkLevelAuthority", "NetworkPivotFrameQ"],
    # Solve: datum, normal equations, estimator and the solve glue.
    "solve": ["NetworkSolveGlue", "NetworkSolveStations", "NetworkEstimator*",
              "NetworkNormalEquations*", "NetworkRankGate", "NetworkCodeBiasDatum",
              "NetworkClockDatum", "NetworkClockAnchor", "NetworkClockLevel",
              "NetworkSbasis*", "NetworkChainDatum", "NetworkPhaseDatum",
              "NetworkPhaseAnchorEpoch", "NetworkAnchorHygiene", "NetworkAnchorReentry",
              "NetworkHandoverStransform", "NetworkDatumGraph", "NetworkPivotHandover",
              "NetworkPhaseGateDrop"],
    # QC: post-fit and pre-fit quality control.
    "qc": ["NetworkRaim*", "NetworkWithinEpochCulprits", "NetworkClockQc",
           "NetworkGrossLos", "NetworkPrefitCensus", "NetworkRedundancyCensus",
           "NetworkBlockBaarda", "NetworkCongruence*", "NetworkMdb",
           "NetworkPriorResidual", "NetworkRhsAttrib", "NetworkRelevel",
           "NetworkRobustStats"],
    # Slip: cycle-slip detection and response (every Network* file under slip/ too).
    "slip": ["NetworkSlipFunnel", "NetworkGapResumeCensus", "NetworkSlipOracle",
             "NetworkOraclePin*"],
    # AR: arc graph, integer lattice, fixed cycles and the producer ambiguity layer.
    "ar": ["NetworkAr*", "NetworkFixedCycles", "NetworkProducerAr", "NetworkSdEwlAr",
           "NetworkAmbFloat", "NetworkZStore"],
    # Emit: product preparation, emission and the product I/O.
    "emit": ["NetworkSolPrep", "NetworkEmitStore", "NetworkLedgerEmit", "NetworkBrdcWire",
             "NetworkPbHold*", "NetworkEmitRebase", "NetworkClockEmitRebase"],
    # Diag: trace sweeps and diagnostics.
    "diag": ["NetworkDdFrac", "NetworkIonoDiag", "NetworkShDevVtec", "NetworkShHealth",
             "NetworkLeveling", "NetworkStashStatus", "NetworkFrameDiag",
             "NetworkPhaseVisibleDiag", "NetworkAmbStateTrace", "NetworkRunTraces",
             "NetworkStationBlock"],
    # Run end: the run-end report table, its walker and the driver-level reports.
    "runend": ["NetworkRunEnd", "NetworkTeardown", "NetworkClosedLoopRunEnd",
               "NetworkReadmitRunEnd", "NetworkResumeRunEnd", "NetworkRunArtifactsRunEnd",
               "NetworkRunTimingRunEnd", "NetworkSolveStationsRunEnd"],
}

# Directories outside the four scope directories whose Network* files are moved too: the
# streaming front end in realtime/ has no includer outside the network producer.  The
# Network* files of products/ stay when the consumers, the real-time engine or the config
# loader include them (wire I/O, product emitter, state snapshot; see --survey).  The two
# emit-rebase files are included by the network producer alone and move.
EXTRA_DIRS = ("realtime",)
PRODUCT_STEMS = ("NetworkEmitRebase", "NetworkClockEmitRebase")

# Network-only helpers without a Network prefix (every production includer is a network
# file); moved with the concern that owns them.
HELPER_STEMS = ("SignalRoster", "SignalRosterSites", "ArcLedger")

# Edits other than includes that a moved test needs because it opens headers by path
# (GNSS_PROJECT_ROOT / "include" / ...): (old text, new text) per moved file.
PATH_EDITS = {
    "tests/network/test_carry_sites.cpp": [
        ('"include" / "app" / "NetworkRunContext.hpp"',
         '"include" / "network" / "state" / "NetworkRunContext.hpp"'),
        ('"include" / "filter" / "NetworkRunTypes.hpp"',
         '"include" / "network" / "driver" / "NetworkRunTypes.hpp"'),
        ('"include" / "app" / "NetworkEntities.hpp"',
         '"include" / "network" / "state" / "NetworkEntities.hpp"'),
    ],
}

# Tests that target only network code but are not named test_network_*: the test includes
# at least one moved header and every other project header it includes is neutral
# (common types, math, config, the log manager), or it is listed here.
EXTRA_TESTS = ["test_codebias_network_config", "test_galileo_network",
               "test_glonass_network_config"]
NEUTRAL_INCLUDES = ("common/", "math/", "config/", "logging/LogManager.hpp",
                    "logging/SafeLogger.hpp")


def git(*args, text=True):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True,
                          capture_output=True, text=text).stdout


def tracked():
    return git("ls-files").splitlines()


def concern_of(stem, directory):
    hits = [c for c, pats in CONCERNS.items()
            if any(fnmatch.fnmatchcase(stem, p) for p in pats)]
    if directory == "slip" and "slip" not in hits:
        hits.append("slip")
    return hits


def candidates(files):
    """Old path of every file that the table moves (sources and headers first, then tests)."""
    out = []
    for f in files:
        m = re.match(r"(include|src/core)/([^/]+)/([^/]+)$", f)
        if m:
            root, d, name = m.groups()
            stem = name.rsplit(".", 1)[0]
            if (name.startswith(("Network", "ApplicationNetwork")) or stem in HELPER_STEMS)                     and (d in SCOPE_DIRS or d in EXTRA_DIRS
                         or (d == "products" and stem in PRODUCT_STEMS)):
                out.append(f)
    inc_to_path = {f.split("/", 1)[1]: f for f in files
                   if f.startswith("include/") and f.endswith((".hpp", ".h", ".inl"))}
    moved = set(out)
    for f in files:
        m = re.match(r"tests/(test_[^/]+)\.cpp$", f)
        if not m:
            continue
        if m.group(1).startswith("test_network_") or m.group(1) in EXTRA_TESTS:
            out.append(f)
            continue
        hits = [x.group(2).decode("utf-8", "replace")
                for x in INC_RE.finditer((ROOT / f).read_bytes())]
        project = [h for h in hits if h in inc_to_path]
        if any(inc_to_path[h] in moved for h in project) and all(
                inc_to_path[h] in moved or h.startswith(NEUTRAL_INCLUDES) for h in project):
            out.append(f)
    return out


def new_path(old):
    m = re.match(r"(include|src/core)/([^/]+)/([^/]+)$", old)
    if m:
        root, d, name = m.groups()
        stem = name.rsplit(".", 1)[0]
        hits = concern_of(stem, d)
        if len(hits) != 1:
            raise SystemExit(f"{old}: concern matches {hits} (need exactly one)")
        return f"{root}/network/{hits[0]}/{name}"
    return "tests/network/" + old.split("/", 1)[1]


def build_table():
    return [(o, new_path(o)) for o in sorted(candidates(tracked()))]


def read_table():
    return [tuple(l.split("\t")) for l in TABLE.read_text().splitlines() if l.strip()]


def inc_string(path):
    """The #include string for a header path, or None for a non-header."""
    return path.split("/", 1)[1] if path.startswith("include/") and \
        path.endswith((".hpp", ".h", ".inl")) else None


def cmd_emit():
    rows = build_table()
    TABLE.write_text("".join(f"{o}\t{n}\n" for o, n in rows), newline="\n")
    per = {}
    for _, n in rows:
        key = "tests" if n.startswith("tests/") else n.split("/network/")[1].split("/")[0]
        per[key] = per.get(key, 0) + 1
    print(len(rows), "rows;", ", ".join(f"{k} {v}" for k, v in sorted(per.items())))


INC_RE = re.compile(rb'^(\s*#\s*include\s*)"([^"]+)"', re.M)


def rewrite_includes(rows):
    mapping = {inc_string(o): inc_string(n) for o, n in rows if inc_string(o)}
    bmap = {}
    for k in mapping:
        bmap.setdefault(k.rsplit("/", 1)[1], []).append(k)
    changed = 0
    bare = []
    for f in tracked():
        if not f.endswith(CODE_EXT) or f.startswith("third_party/"):
            continue
        p = ROOT / f
        if not p.exists():
            continue
        data = p.read_bytes()

        def sub(m):
            s = m.group(2).decode("utf-8", "replace")
            if s in mapping:
                return m.group(1) + b'"' + mapping[s].encode() + b'"'
            if s.rsplit("/", 1)[-1] in bmap and s not in mapping.values():
                bare.append((f, s))
            return m.group(0)

        out = INC_RE.sub(sub, data)
        if out != data:
            p.write_bytes(out)
            changed += 1
    if bare:
        for f, s in bare:
            print("UNMAPPED INCLUDE FORM:", f, s)
        raise SystemExit("include forms the exact-match rewrite does not cover")
    return changed


def retarget_test_support(rows):
    """A moved test one level deeper reaches the shared harness (tests/support) as
    "../support/X": its include of "support/X" was relative to tests/."""
    n = 0
    for _, new in rows:
        if not new.startswith("tests/network/"):
            continue
        p = ROOT / new
        data = p.read_bytes()
        out = re.sub(rb'(?m)^([ \t]*#[ \t]*include[ \t]*)"support/', rb'\1"../support/', data)
        if out != data:
            p.write_bytes(out)
            n += 1
    return n


def rewrite_atfile(rows):
    """Update a path stated in an @file line of a moved file (exact old path or old
    include string only)."""
    n = 0
    for old, new in rows:
        p = ROOT / new
        data = p.read_bytes()
        cands = {old} if old.startswith("tests/") else {old, old.split("/", 1)[1]}
        if old.startswith("src/core/"):
            cands.add(old[len("src/core/"):])

        def sub(m):
            s = m.group(2).decode("utf-8", "replace")
            if s in cands:
                alt = new if s == old else (new.split("/", 1)[1] if s == old.split("/", 1)[1]
                                           else new[len("src/core/"):])
                return m.group(1) + alt.encode()
            return m.group(0)

        out = re.sub(rb"(?m)^(.*@file[ \t]+)(\S+)", sub, data)
        if out != data:
            p.write_bytes(out)
            n += 1
    return n


def rewrite_cmake(rows):
    p = ROOT / "CMakeLists.txt"
    text = p.read_bytes()
    n = 0
    for old, new in rows:
        o, nn = old.encode(), new.encode()
        pat = re.compile(rb"(?<![\w/.-])" + re.escape(o) + rb"(?![\w])")
        text, k = pat.subn(nn, text)
        n += k
    p.write_bytes(text)
    return n


def ref_forms(rows):
    """Every spelling of a moved file that a doc or a script may use -> its new spelling."""
    out = {}
    for o, n in rows:
        forms = [(o, n)]
        if inc_string(o):
            forms.append((inc_string(o), inc_string(n)))
        if o.startswith("src/core/"):
            forms.append((o[len("src/core/"):], n[len("src/core/"):]))
        for a, b in list(forms):
            if "." in a.rsplit("/", 1)[-1]:
                forms.append((a.rsplit(".", 1)[0], b.rsplit(".", 1)[0]))
        if o.startswith("tests/"):
            forms = [(o, n)]
        out.update(forms)
    # Windows spelling (onboarding pages): backslash separators, same mapping
    out.update({k.replace("/", "\\"): v.replace("/", "\\") for k, v in list(out.items())})
    return out


def cmd_rewrite_refs(paths):
    forms = ref_forms(read_table())
    keys = sorted(forms, key=len, reverse=True)
    pat = re.compile(rb"(?<![\w/\\.-])(?:" + b"|".join(re.escape(k.encode()) for k in keys)
                     + rb")(?![\w])")
    for f in paths:
        p = Path(f)
        data = p.read_bytes()
        out, k = pat.subn(lambda m: forms[m.group(0).decode()].encode(), data)
        if k:
            p.write_bytes(out)
        print(f"{k:5d}  {f}")


def cmd_apply():
    rows = read_table()
    for old, new in rows:
        (ROOT / new).parent.mkdir(parents=True, exist_ok=True)
        git("mv", old, new)
    print("moved", len(rows))
    print("include files rewritten:", rewrite_includes(rows))
    print("tests retargeted to ../support:", retarget_test_support(rows))
    print("@file lines rewritten in:", rewrite_atfile(rows))
    print("path edits in tests:", apply_path_edits())
    print("CMakeLists.txt path edits:", rewrite_cmake(rows))


def show(rev, path):
    return subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"], check=True,
                          capture_output=True).stdout


def apply_path_edits():
    n = 0
    for f, edits in PATH_EDITS.items():
        p = ROOT / f
        data = p.read_bytes()
        for old, new in edits:
            assert old.encode() in data, (f, old)
            data = data.replace(old.encode(), new.encode())
        p.write_bytes(data)
        n += 1
    return n


def lines_ok(a, b, edits=()):
    """Two blobs differ only on #include and @file lines (and the listed text edits)."""
    # The working tree is CRLF (core.autocrlf) while blobs are LF: compare as LF.
    la = a.replace(b"\r\n", b"\n").split(b"\n")
    lb = b.replace(b"\r\n", b"\n").split(b"\n")
    if len(la) != len(lb):
        return False
    for x, y in zip(la, lb):
        if x != y:
            ok = (INC_RE.match(x) and INC_RE.match(y)) or (b"@file" in x and b"@file" in y)                 or any(x.replace(o.encode(), n.encode()) == y for o, n in edits)
            if not ok:
                return False
    return True


def cmd_check(base):
    rows = read_table()
    bad = 0
    moved_new = {n for _, n in rows}
    for old, new in rows:
        if not lines_ok(show(base, old), (ROOT / new).read_bytes(), PATH_EDITS.get(new, ())):
            print("CONTENT DIFFERS beyond include/@file lines:", old, "->", new)
            bad += 1
    print(f"{len(rows)} moved files compared with base {base[:8]}: "
          f"{'all identical beyond include/@file lines' if not bad else str(bad) + ' differ'}")
    # every other modified code file differs from base on include lines only
    names = git("diff", "--name-only", "--no-renames", base).splitlines()
    others = [f for f in names if f.endswith(CODE_EXT) and f not in moved_new
              and (ROOT / f).exists()]
    ob = 0
    for f in others:
        if not lines_ok(show(base, f), (ROOT / f).read_bytes()):
            print("UNMOVED FILE CHANGED beyond include lines:", f)
            ob += 1
    print(f"{len(others)} unmoved code files changed: "
          f"{'include lines only' if not ob else str(ob) + ' differ'}")
    # stale references: old include string or old path left in code or CMake
    stale = 0
    old_inc = {inc_string(o): o for o, _ in rows if inc_string(o)}
    old_paths = [o for o, _ in rows]
    for f in tracked():
        if f.startswith("third_party/") or not (f.endswith(CODE_EXT) or f == "CMakeLists.txt"):
            continue
        p = ROOT / f
        if not p.exists():
            continue
        data = p.read_bytes()
        for m in INC_RE.finditer(data):
            s = m.group(2).decode("utf-8", "replace")
            if s in old_inc:
                print("STALE INCLUDE:", f, s)
                stale += 1
        if f == "CMakeLists.txt":
            for o in old_paths:
                if o.encode() in data:
                    print("STALE CMAKE PATH:", o)
                    stale += 1
    print("stale include/CMake references:", stale)
    for old, new in rows:
        if (ROOT / old).exists() or not (ROOT / new).exists():
            print("MOVE INCOMPLETE:", old)
            stale += 1
    return 1 if (bad or ob or stale) else 0


def own_of(accepted):
    out = set()
    for h in accepted:
        out.add(h.replace("include/", "src/core/", 1).rsplit(".", 1)[0] + ".cpp")
    return out


def cmd_survey():
    rows = build_table()
    files = [f for f in tracked() if f.endswith(CODE_EXT) and not f.startswith("third_party/")]
    moved = {o for o, _ in rows}
    inc_to_path = {inc_string(f): f for f in files if inc_string(f)}
    users = {}
    for f in files:
        data = (ROOT / f).read_bytes() if (ROOT / f).exists() else b""
        for m in INC_RE.finditer(data):
            s = m.group(2).decode("utf-8", "replace")
            if s in inc_to_path:
                users.setdefault(inc_to_path[s], set()).add(f)

    def own(h):
        stem = h.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        return {f for f in files if f.rsplit("/", 1)[-1].startswith(stem)
                and f.rsplit("/", 1)[-1].rsplit(".", 1)[0] in (stem, stem + "Sites", stem + "Setup")}

    print("# Headers that stay, whose production includers are all moved files or candidates")
    print("# (closure over rounds; tests listed apart):")
    accepted = set()
    for rnd in range(1, 6):
        new_round = []
        for h, us in sorted(users.items()):
            if h in moved or h in accepted or not h.startswith("include/"):
                continue
            prod = {u for u in us - own(h) if not u.startswith("tests/")}
            if prod and all(u in moved or u in accepted or u in own_of(accepted) for u in prod):
                new_round.append((h, prod, sorted(u[6:] for u in us if u.startswith("tests/"))))
        if not new_round:
            break
        for h, prod, tests in new_round:
            accepted.add(h)
            print(f"  r{rnd} {h}  <- {len(prod)} includers, tests: {tests}")
    print("# Network* files that stay (outside the table), with their non-moved includers:")
    for f in files:
        name = f.rsplit("/", 1)[-1]
        if name.startswith(("Network", "ApplicationNetwork")) and f not in moved:
            ex = sorted(users.get(f, set()) - moved - own(f)) if f.startswith("include/") else []
            print(f"  {f}  external includers: {len(ex)}  {ex[:3]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    for a in ("emit", "apply", "check", "survey"):
        g.add_argument("--" + a, action="store_true")
    g.add_argument("--rewrite-refs", nargs="+", metavar="FILE")
    ap.add_argument("--base", default=None)
    a = ap.parse_args()
    if a.emit:
        cmd_emit()
    elif a.apply:
        cmd_apply()
    elif a.survey:
        cmd_survey()
    elif a.rewrite_refs:
        cmd_rewrite_refs(a.rewrite_refs)
    else:
        base = a.base or git("merge-base", "main", "HEAD").strip()
        sys.exit(cmd_check(base))


if __name__ == "__main__":
    main()
