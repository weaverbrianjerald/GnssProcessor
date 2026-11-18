// Unit tests of the process noise of a carried receiver code-bias column (include/network/carry/RxCodeBiasCarry.hpp,
// rxCbCarryQ; network.rx_extra_band_delay_q): a receiver IFB column takes the extra-band delay's own noise when it is
// set; every other column, and every column when it is 0, takes network.rx_cb_carry_q.

#include "network/carry/RxCodeBiasCarry.hpp"
#include "network/state/StationParams.hpp"
#include "filter/StateKeySchema.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using namespace GnssProcessor;
namespace K = GnssProcessor::StateKeys;

TEST(RxExtraBandDelayQ, AnIfbColumnTakesItsOwnNoiseWhenSet) {
    const std::optional<RxCbKey> ifb = receiverCodeBiasKey(K::makeIfbKey("GPS", '5'));
    ASSERT_TRUE(ifb.has_value());
    EXPECT_DOUBLE_EQ(rxCbCarryQ(*ifb, 1e-9, 3e-6), 3e-6);
    EXPECT_DOUBLE_EQ(rxCbCarryQ(K::makeIfbKey("GPS", '5'), 1e-9, 3e-6), 3e-6);
}

TEST(RxExtraBandDelayQ, TheDcbAndAnUnsetNoiseKeepTheCarryNoise) {
    const std::optional<RxCbKey> dcb = receiverCodeBiasKey(K::makeDcbKey("GPS"));
    const std::optional<RxCbKey> ifb = receiverCodeBiasKey(K::makeIfbKey("Galileo", '7'));
    ASSERT_TRUE(dcb.has_value());
    ASSERT_TRUE(ifb.has_value());
    EXPECT_DOUBLE_EQ(rxCbCarryQ(*dcb, 1e-9, 3e-6), 1e-9);
    EXPECT_DOUBLE_EQ(rxCbCarryQ(*ifb, 1e-9, 0.0), 1e-9);
    EXPECT_DOUBLE_EQ(rxCbCarryQ(K::makeDcbKey("GPS"), 1e-9, 3e-6), 1e-9);
    EXPECT_DOUBLE_EQ(rxCbCarryQ(std::string("not a key"), 1e-9, 3e-6), 1e-9);
}
