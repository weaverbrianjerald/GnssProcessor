"""The dashboard's blocks beyond the core block, one module per block.

Each module has `build(ctx) -> (header dict, {name: numpy array})` or None when nothing it reads is present; what
it could not find is noted with `ctx.note_missing`.  The core block (the trees, links, events, products) is built by
nv_build.py; these blocks are decoded by the page only when a view needs them:

    epoch       per-epoch series of the whole network (solution state, sigma0, the held-out user check, timing)
    receivers   per receiver and epoch: post-fit residuals, states, coordinate test, residual test
    satellites  per satellite and epoch: post-fit residuals, code-bias states, clock anchor guard, discontinuities
    quality     the stochastic model and the tests: variance factors, residuals by elevation, histograms, outlier
                and detectability tests, residual attribution, slip flags
    los         per line of sight (receiver x satellite) and epoch: residuals per signal, slip observables,
                slant ionosphere, carried float ambiguities
"""
from __future__ import annotations

import time
from pathlib import Path

import nv_tables as T


class Ctx:
    """What every block builder gets: the run, its index (from the core block) and a record of missing parts."""

    def __init__(self, run: Path, ix, tr: dict, log=print):
        self.run = run
        self.logs = run / 'logs'
        self.ix = ix
        self.tr = tr
        self.log = log
        self.missing: list[dict] = []
        self.timing: dict = {}

    def table(self, name: str, columns: dict, layer: str, part: str, key: str):
        """Read logs/name (None when absent, noted as a missing part of the layer)."""
        t0 = time.time()
        t = T.read(self.logs, name, columns)
        self.timing[name] = round(time.time() - t0, 2)
        if t is None:
            self.note_missing(layer, part, 'logs/' + name, key)
        return t

    def note_missing(self, layer: str, part: str, file: str, key: str):
        if not any(m['part'] == part and m['layer'] == layer for m in self.missing):
            self.missing.append({'layer': layer, 'part': part, 'file': file, 'key': key})


def build_all(ctx: Ctx) -> dict:
    """{block name: (header, arrays)} for every block that has something to show."""
    from layers import epoch, los, quality, receivers, satellites
    out = {}
    for name, mod in (('epoch', epoch), ('receivers', receivers), ('satellites', satellites), ('quality', quality),
                      ('los', los)):
        t0 = time.time()
        res = mod.build(ctx)
        dt = time.time() - t0
        if res is not None:
            out[name] = res
        ctx.log(f'  block {name:10s} {dt:6.2f} s' + ('' if res is not None else '  (nothing to show)'))
    return out
