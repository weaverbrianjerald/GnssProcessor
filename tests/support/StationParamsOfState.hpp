#pragma once
/**
 * @file StationParamsOfState.hpp
 * @brief Rebuild a test station's parameter set from its state manager.
 *
 * A reader that takes a station's columns from its parameter set sees only what the set holds.
 * A test that adds columns to the station's state manager by hand calls @ref syncStationParams
 * afterwards, as the station's split does in a run: the set then lists the columns with their
 * typed identities and values and no dispositions (nothing split them).
 */
#include "measurement/AbsorbedSharedStates.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/StationParamsShadow.hpp"

namespace gnss_test {

/**
 * @brief Rebuild a station's parameter set from its state manager.
 * @param st The station; its @c sm is read and its @c params overwritten.
 */
inline void syncStationParams(GnssProcessor::NetStation& st) {
    GnssProcessor::buildStationParams(*st.sm, {}, GnssProcessor::StationAbsorbed{}, st.params);
}

}  // namespace gnss_test
