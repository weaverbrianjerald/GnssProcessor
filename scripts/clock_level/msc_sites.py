"""The 16 held-out census sites, selected 2026-09-21, read from the tracked site_list.json beside this file.

Every field census (census2h.py, census6h.py, mday_census.py, gcensus.py) reads its site list
through load().  The file holds, for each site, the name, the RINEX path, the ECEF coordinate,
the receiver and the signals it latches.
"""
import json
import os

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "site_list.json").replace("\\", "/")


def load():
    """The list of site records (the "sites" array of site_list.json)."""
    with open(PATH, encoding="utf-8") as fh:
        return json.load(fh)["sites"]
