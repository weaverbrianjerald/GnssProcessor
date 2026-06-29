"""Tests for the network station roster reader (config JSON + roster CSV)."""
import numpy as np

from gnssviz.network_config import (ROLE_DATUM, ROLE_HELD_OUT, ROLE_NETWORK,
                                    load_stations, stations_from_config,
                                    stations_from_roster_csv)

_JSONC = """\
// a JSONC network config (comments + trailing commas)
{
  "network": {
    "clock_datum_station": "AAAA00AAA",
    "stations": [
      { "name": "AAAA00AAA", "position_ecef": [-2493304.99, -4655214.79, 3565497.67] },
      { "name": "BBBB00BBB", "position_ecef": [-2281621.75, -1453585.08, 5756964.89] },
    ],
    "closed_loop": {
      "enable": true,
      "station": { "name": "CCCC00CCC", "position_ecef": [-2491490.72, -4660802.85, 3559129.19] }
    },
  },
}
"""


def test_jsonc_config_roles(tmp_path):
    p = tmp_path / "net.jsonc"
    p.write_text(_JSONC, encoding="utf-8")
    sts = stations_from_config(str(p))
    by = {s.id: s for s in sts}
    assert by["AAAA00AAA"].role == ROLE_DATUM
    assert by["BBBB00BBB"].role == ROLE_NETWORK
    assert by["CCCC00CCC"].role == ROLE_HELD_OUT
    assert by["AAAA00AAA"].label == "AAAA"


def test_config_latlon_reasonable(tmp_path):
    p = tmp_path / "net.jsonc"
    p.write_text(_JSONC, encoding="utf-8")
    s = {x.id: x for x in stations_from_config(str(p))}["AAAA00AAA"]
    # JPL-ish ECEF -> ~34 N, ~-118 E.
    assert 33.0 < s.lat < 35.0
    assert -119.0 < s.lon < -117.0


def test_roster_csv(tmp_path):
    p = tmp_path / "network_stations.csv"
    p.write_text("station,role,ecef_x,ecef_y,ecef_z\n"
                 "AAAA00AAA,datum,-2493304.99,-4655214.79,3565497.67\n"
                 "BBBB00BBB,network,-2281621.75,-1453585.08,5756964.89\n", encoding="utf-8")
    sts = stations_from_roster_csv(str(p))
    assert len(sts) == 2
    assert {s.role for s in sts} == {ROLE_DATUM, ROLE_NETWORK}


def test_load_prefers_roster_then_config(tmp_path):
    cfg = tmp_path / "net.jsonc"; cfg.write_text(_JSONC, encoding="utf-8")
    roster = tmp_path / "network_stations.csv"
    roster.write_text("station,role,ecef_x,ecef_y,ecef_z\n"
                      "ONLY0ROST,network,-2281621.75,-1453585.08,5756964.89\n",
                      encoding="utf-8")
    # Roster present -> roster wins (single station).
    sts = load_stations(str(roster), str(cfg))
    assert [s.id for s in sts] == ["ONLY0ROST"]
    # Roster absent -> config fallback (three stations).
    assert len(load_stations(str(tmp_path / "missing.csv"), str(cfg))) == 3
    # Neither -> empty.
    assert load_stations(None, None) == []
