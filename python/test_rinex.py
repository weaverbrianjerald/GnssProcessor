from gnss_processor import RinexParser, System
import pytest

def test_rinex_parser():
    """Test RinexParser with the provided RINEX file."""
    rinex_path = "C:/GnssProcessorSupport/GCGO00USA_R_20241290000_01D_30S_MO.rnx"
    parser = RinexParser(rinex_path)
    
    # Test parsing
    assert parser.parse(), f"Failed to parse RINEX file: {rinex_path}"
    
    # Test header
    header = parser.get_header()
    assert header.version >= 3.0, "Expected RINEX 3.x version"
    assert header.version < 4.0, "Version too new for RINEX 3.x"
    assert header.interval is None or header.interval == pytest.approx(30.0, abs=1e-3), "Expected ~30s interval"
    
    # Test observations
    obs = parser.get_observations()
    assert len(obs) > 0, "No observations parsed"
    
    # Test GPS observations with C1C
    has_gps_c1c = any(o.system == System.GPS and "C1C" in o.measurements for o in obs)
    assert has_gps_c1c, "Expected GPS C1C observations"
    
    # Test System enum string conversion
    assert str(System.GPS) == "GPS", "System enum string conversion failed"
    
    # Test observation types in header
    assert System.GPS in header.obs_types, "Expected GPS observation types"
    assert "C1C" in header.obs_types[System.GPS], "Expected C1C in GPS observation types"
