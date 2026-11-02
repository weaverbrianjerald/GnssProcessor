#!/usr/bin/env python3
"""Which configurations set a retired configuration key, and so warn at load.

    python scripts/retired_keys_census.py [DIR_OR_FILE ...] [--md] [--list FILE]
    python scripts/retired_keys_census.py --config CONFIG.json
    python scripts/retired_keys_census.py selftest

The network section loader logs one warning per retired key a configuration sets
(include/config/RetiredConfigKeys.hpp).  This census predicts those warnings without running the
loader.  It reads the table from that header (the one source), resolves each configuration through
`extends` the way the loader does (scripts/gnss_config_chain.py), keeps the network-mode ones
(`processing.mode` ppp-rtk-network, the one mode whose loader runs the scan) and looks each retired
key up under both spellings the old loader read: the dotted path through nested objects, then the
flat key at the top level.  A "[]" row (network.stations[].fixed) counts once for the whole array.

With no path, every *.json under config/.  The summary counts the files read, those that do not
resolve (a broken `extends` chain or a syntax error: the loader refuses them too, before any
warning), the network-mode configurations and those that warn, then the keys by the number of
configurations that set them and the directories by the number that warn.  --md prints the two
tables as Markdown.  --list FILE writes one tab-separated line per warning configuration (path,
key count, keys).  --config prints the warnings one configuration would get.
"""
import argparse
import collections
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADER = os.path.join(ROOT, "include", "config", "RetiredConfigKeys.hpp")
sys.path.insert(0, HERE)
from gnss_config_chain import ConfigChainError, load_config  # noqa: E402

# A deletion's factory (its record id and date), a family built by one, and a row.
_FACTORY = re.compile(r'constexpr RetiredKeyFamily (\w+)\(std::string_view commit, std::string_view what\) \{'
                      r'\s*return RetiredKeyFamily\{"([^"]*)", "([^"]*)", commit, what\};')
_FAMILY = re.compile(r'inline constexpr RetiredKeyFamily (\w+) =\s*(\w+)\(\s*"(\w+)",\s*"([^"]*)"\s*\);')
_ROW = re.compile(r'\{"([^"]+)", "([^"]*)", &(\w+)\}')


def read_table(path=HEADER):
    """The rows of the header's table as dicts (path, flat, card, date, commit, what), in order."""
    text = io.open(path, encoding="utf-8").read()
    factories = {name: (card, date) for name, card, date in _FACTORY.findall(text)}
    families = {var: {"card": factories[fn][0], "date": factories[fn][1], "commit": commit, "what": what}
                for var, fn, commit, what in _FAMILY.findall(text)}
    body = text[text.index("kRetiredConfigKeys[]"):]
    rows = []
    for key, flat, var in _ROW.findall(body):
        rows.append(dict(families[var], path=key, flat=flat))
    return rows


def _walk(node, dotted):
    """The value at a dotted path through nested objects, or (False, None)."""
    for part in dotted.split("."):
        if not isinstance(node, dict) or part not in node:
            return False, None
        node = node[part]
    return True, node


def is_network_mode(cfg):
    """The loader's mode test: processing.mode, else the flat processing_mode, else PPP;
    lower case, '_' read as '-'."""
    found, mode = _walk(cfg, "processing.mode")
    if not found:
        mode = cfg.get("processing_mode", "PPP")
    return isinstance(mode, str) and mode.lower().replace("_", "-") == "ppp-rtk-network"


def scan(cfg, rows):
    """The retired keys `cfg` sets: (row, spellings, value, entries), in table order."""
    hits = []
    if not isinstance(cfg, dict):
        return hits
    for row in rows:
        spellings, value, entries = [], None, 0
        if "[]." in row["path"]:
            array_path, member = row["path"].split("[].", 1)
            found, arr = _walk(cfg, array_path)
            if found and isinstance(arr, list):
                for entry in arr:
                    if isinstance(entry, dict) and member in entry:
                        if entries == 0:
                            value = entry[member]
                        entries += 1
            if entries:
                spellings.append(row["path"])
        else:
            found, v = _walk(cfg, row["path"])
            if found:
                spellings.append(row["path"])
                value = v
            if row["flat"] and row["flat"] in cfg:
                if not spellings:
                    value = cfg[row["flat"]]
                spellings.append(row["flat"])
        if spellings:
            hits.append((row, spellings, value, entries))
    return hits


def config_files(paths):
    """Every *.json under the directories of `paths` (files as given), sorted, relative to the
    repository root where they lie inside it."""
    out = []
    for p in paths:
        if os.path.isfile(p):
            out.append(p)
            continue
        for d, _, names in os.walk(p):
            out.extend(os.path.join(d, n) for n in names if n.endswith(".json"))

    def shown(p):
        full = os.path.abspath(p)
        rel = os.path.relpath(full, ROOT)
        return (full if rel.startswith("..") else rel).replace("\\", "/")
    return sorted(set(shown(p) for p in out))


def census(paths, rows):
    """Per file: ('error', message) | ('other', None) | ('network', hits)."""
    result = {}
    for path in config_files(paths):
        try:
            cfg, _ = load_config(os.path.join(ROOT, path))
        except (ConfigChainError, ValueError, OSError) as e:
            result[path] = ("error", str(e).splitlines()[0])
            continue
        if not isinstance(cfg, dict) or not is_network_mode(cfg):
            result[path] = ("other", None)
            continue
        result[path] = ("network", scan(cfg, rows))
    return result


def report(result, rows, md, list_file):
    files = len(result)
    errors = [p for p, (kind, _) in result.items() if kind == "error"]
    network = {p: hits for p, (kind, hits) in result.items() if kind == "network"}
    warn = {p: hits for p, hits in network.items() if hits}
    print("files read: %d; not resolved: %d; network mode: %d; warn: %d (%d warnings)"
          % (files, len(errors), len(network), len(warn), sum(len(h) for h in warn.values())))
    per_key = collections.Counter()
    flat_key = collections.Counter()
    for hits in warn.values():
        for row, spellings, _, _ in hits:
            per_key[row["path"]] += 1
            if row["flat"] and row["flat"] in spellings:
                flat_key[row["path"]] += 1
    by_dir_net = collections.Counter(os.path.dirname(p) for p in network)
    by_dir_warn = collections.Counter(os.path.dirname(p) for p in warn)
    order = {r["path"]: i for i, r in enumerate(rows)}
    commit_of = {r["path"]: r["commit"] for r in rows}
    keys = sorted(per_key, key=lambda k: (-per_key[k], order[k]))
    if md:
        print("\n| directory | network-mode configurations | warn |\n| --- | ---: | ---: |")
        for d in sorted(by_dir_net, key=lambda d: (-by_dir_warn[d], d)):
            print("| `%s` | %d | %d |" % (d, by_dir_net[d], by_dir_warn[d]))
        print("\n| key | configurations | of them flat | deletion commit |\n| --- | ---: | ---: | --- |")
        for k in keys:
            print("| `%s` | %d | %d | `%s` |" % (k, per_key[k], flat_key[k], commit_of[k]))
        unset = [r["path"] for r in rows if r["path"] not in per_key]
        print("\nset by no configuration (%d): %s" % (len(unset), ", ".join("`%s`" % k for k in unset)))
    else:
        print("\nby directory (network mode, warn):")
        for d in sorted(by_dir_net, key=lambda d: (-by_dir_warn[d], d)):
            print("  %-40s %5d %5d" % (d, by_dir_net[d], by_dir_warn[d]))
        print("\nby key (configurations, of them flat):")
        for k in keys:
            print("  %-60s %5d %5d  %s" % (k, per_key[k], flat_key[k], commit_of[k]))
    if errors:
        print("\nnot resolved (%d), first five:" % len(errors))
        for p in errors[:5]:
            print("  %s: %s" % (p, result[p][1]))
    if list_file:
        with io.open(list_file, "w", encoding="utf-8", newline="\n") as fh:
            for p in sorted(warn):
                fh.write("%s\t%d\t%s\n" % (p, len(warn[p]), " ".join(r["path"] for r, _, _, _ in warn[p])))
        print("\nwrote %s (%d lines)" % (list_file, len(warn)))


def show_config(path, rows):
    cfg, _ = load_config(path)
    if not is_network_mode(cfg):
        print("%s: not network mode; the loader runs no scan" % path)
        return
    hits = scan(cfg, rows)
    for row, spellings, value, entries in hits:
        print("%s\t%s\t%s\t%s" % (row["path"], " and ".join(spellings),
                                   json.dumps(value, separators=(",", ":")), row["commit"]))
    print("%d warning(s)" % len(hits))


def selftest():
    n = 0

    def chk(cond, what):
        nonlocal n
        n += 1
        assert cond, what

    rows = read_table()
    chk(len(rows) == 150, "150 rows, not %d" % len(rows))
    chk(len({r["commit"] for r in rows}) == 28, "28 families")
    chk({(r["card"], r["date"]) for r in rows} == {("RKD", "2026-10-14")}, "one deletion, its id and date")
    chk(len({r["path"] for r in rows}) == 150, "unique paths")
    chk(sum(1 for r in rows if not r["flat"]) == 1, "one row without a flat spelling")
    cfg = {"processing": {"mode": "ppp_rtk_network"},
           "network": {"mw_wl": {"enable": True}, "stations": [{"fixed": True}, {}, {"fixed": False}]},
           "network_mw_wl_enable": False, "carry_iono": True, "mw_wl_enable": True}
    chk(is_network_mode(cfg), "ppp_rtk_network is network mode")
    chk(not is_network_mode({"processing_mode": "ppp"}), "ppp is not")
    chk(is_network_mode({"processing_mode": "PPP-RTK-Network"}), "the flat mode key is read")
    hits = {r["path"]: (s, v, e) for r, s, v, e in scan(cfg, rows)}
    chk(set(hits) == {"network.mw_wl.enable", "network.stations[].fixed", "network.carry_iono"}, sorted(hits))
    chk(hits["network.mw_wl.enable"] == (["network.mw_wl.enable", "network_mw_wl_enable"], True, 0),
        "both spellings, the dotted value")
    chk(hits["network.stations[].fixed"] == (["network.stations[].fixed"], True, 2), "the roster member")
    chk(hits["network.carry_iono"] == (["carry_iono"], True, 0), "the flat spelling")
    chk(not scan({"network": {"network_mw_wl_enable": True}, "network.emit_pb_filter": True}, rows),
        "a flat spelling below the top level and a dotted literal key are not read")
    golden = os.path.join(ROOT, "config", "golden")
    for name, count in (("golden_field_refound.json", 0), ("golden_sim_refound.json", 0),
                        ("golden_field_gps_dual.json", 28)):
        got = len(scan(load_config(os.path.join(golden, name))[0], rows))
        chk(got == count, "%s sets %d retired keys, not %d" % (name, got, count))
    print("retired_keys_census selftest: %d/%d PASS" % (n, n))


def main(argv):
    if argv[:1] == ["selftest"]:
        selftest()
        return 0
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--md", action="store_true", help="print the tables as Markdown")
    ap.add_argument("--list", metavar="FILE", help="write one line per warning configuration")
    ap.add_argument("--config", metavar="CONFIG", help="print the warnings of one configuration")
    a = ap.parse_args(argv)
    rows = read_table()
    if a.config:
        show_config(a.config, rows)
        return 0
    report(census(a.paths or [os.path.join(ROOT, "config")], rows), rows, a.md, a.list)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
