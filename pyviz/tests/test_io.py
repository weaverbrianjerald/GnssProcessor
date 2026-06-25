"""CSV/time helpers: GPS-time conversion and column fallbacks."""
import pandas as pd

from gnssviz.io import GPS_EPOCH, add_utc, first_col, gps_to_datetime


def test_gps_to_datetime_week_origin():
    assert gps_to_datetime([0], [0])[0] == GPS_EPOCH


def test_gps_to_datetime_known():
    # 2310 weeks + 3600 s after the GPS epoch.
    t = gps_to_datetime([2310], [3600.0])[0]
    expected = GPS_EPOCH + pd.to_timedelta(2310 * 604800 + 3600.0, unit="s")
    assert t == expected


def test_first_col_fallback():
    df = pd.DataFrame({"x": [1, 2], "tow": [10, 20]})
    assert first_col(df, "x_m", "x").tolist() == [1, 2]
    assert first_col(df, "tow_s", "tow").tolist() == [10, 20]
    assert first_col(df, "nope") is None


def test_add_utc_supports_both_schemas():
    a = add_utc(pd.DataFrame({"week": [2300], "tow_s": [0.0]}))
    b = add_utc(pd.DataFrame({"week": [2300], "tow": [0.0]}))
    assert a["utc"].iloc[0] == b["utc"].iloc[0]
