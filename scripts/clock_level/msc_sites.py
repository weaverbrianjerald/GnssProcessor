"""The 16 held-out census sites, selected 2026-09-21, read from the tracked site_list.json beside this file.

Every field census (census2h.py, census6h.py, mday_census.py, gcensus.py) reads its site list
through load().  The file holds, for each site, the name, the RINEX path, the ECEF coordinate,
the receiver and the signals it latches.

The environment variable MSC_SITE_LIST names another file of the same layout (for example
site_list_sinex.json, the same sites at their weekly SINEX coordinates, written by
scripts/roster_from_sinex.py); unset, the tracked site_list.json is read.  A list may also carry
a top-level `consumer` entry (name, ecef), the held-out consumer's coordinate under the same
source; load_consumer() returns it, or None.
"""
import json
import os

PATH = (os.environ.get("MSC_SITE_LIST")
        or os.path.join(os.path.dirname(os.path.abspath(__file__)), "site_list.json")).replace("\\", "/")


def load():
    """The list of site records (the "sites" array of the site list)."""
    with open(PATH, encoding="utf-8") as fh:
        return json.load(fh)["sites"]


def load_consumer():
    """The `consumer` entry of the site list (name, ecef), or None when the list has none."""
    with open(PATH, encoding="utf-8") as fh:
        return json.load(fh).get("consumer")
