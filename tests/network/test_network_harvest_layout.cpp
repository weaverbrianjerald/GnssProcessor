/**
 * @file test_network_harvest_layout.cpp
 * @brief The station harvest layout of card NOM3 step HV1: harvestBlockSpans and
 *        StationHarvestBlock (app/NetworkLocalCarryHarvest.hpp).
 *
 * The layout replaced eleven per-station hand-forward vectors.  A frozen consumer saw the
 * station's block exactly when the station had one and its segment had columns, and read the
 * segment's first row from the station loop's row-offset local.  What is pinned here:
 *
 *  1. For every combination of present segments (and several ambiguity counts), each span
 *     starts at the offset the loop head computed and counts exactly the segment's columns;
 *     the spans tile the block in BlockSegment order with the ZWD last.
 *  2. segmentBlock returns the block only for a segment with columns, and null for every
 *     segment when the station has no block.
 *  3. rowOf returns the segment's first row whatever the block.
 */

#include <gtest/gtest.h>

#include "network/carry/NetworkLocalCarryHarvest.hpp"

#include <memory>

using namespace GnssProcessor;

namespace {

/// The station loop head's arithmetic for one station: column counts per segment.
struct Counts { Eigen::Index amb, iono, rxclk, rxcb, pc; bool zwd; };

StationHarvestBlock layoutOf(const Counts& c, bool with_block) {
    const Eigen::Index iono_row0  = c.amb;
    const Eigen::Index rxclk_row0 = iono_row0 + c.iono;
    const Eigen::Index rxcb_row0  = rxclk_row0 + c.rxclk;
    const Eigen::Index pc_row0    = rxcb_row0 + c.rxcb;
    const Eigen::Index n_cols     = pc_row0 + c.pc + (c.zwd ? 1 : 0);
    StationHarvestBlock sb;
    sb.ok   = true;
    sb.span = harvestBlockSpans(iono_row0, rxclk_row0, rxcb_row0, pc_row0, n_cols, c.zwd);
    sb.cols.resize(static_cast<std::size_t>(n_cols));
    if (with_block) sb.blk = std::make_shared<PlHarvestBlock>();
    return sb;
}

constexpr BlockSegment kSegments[] = {BlockSegment::Amb, BlockSegment::Iono,
                                      BlockSegment::RxClock, BlockSegment::RxCodeBias,
                                      BlockSegment::PhaseColour, BlockSegment::Zwd};

}  // namespace

// 1: every combination of present segments, three ambiguity counts
TEST(NetworkHarvestLayout, SpansTileTheBlockInSegmentOrderWithTheZwdLast) {
    for (Eigen::Index amb : {0, 1, 5}) {
        for (int mask = 0; mask < 32; ++mask) {
            const Counts c{amb, (mask & 1) ? 3 : 0, (mask & 2) ? 2 : 0, (mask & 4) ? 2 : 0,
                           (mask & 8) ? 4 : 0, (mask & 16) != 0};
            const StationHarvestBlock sb = layoutOf(c, true);
            const Eigen::Index want_n[] = {c.amb, c.iono, c.rxclk, c.rxcb, c.pc, c.zwd ? 1 : 0};
            Eigen::Index row = 0;
            for (std::size_t s = 0; s < kBlockSegmentCount; ++s) {
                SCOPED_TRACE(testing::Message() << "amb " << amb << " mask " << mask << " seg " << s);
                EXPECT_EQ(sb.span[s].row0, row);
                EXPECT_EQ(sb.span[s].n, want_n[s]);
                row += sb.span[s].n;
            }
            EXPECT_EQ(row, static_cast<Eigen::Index>(sb.cols.size()));
            if (c.zwd) {
                EXPECT_EQ(sb.rowOf(BlockSegment::Zwd),
                          static_cast<Eigen::Index>(sb.cols.size()) - 1);
            }
        }
    }
}

// 2: the block goes to a segment with columns only, and never without a block
TEST(NetworkHarvestLayout, SegmentBlockIsNullWithoutColumnsOrWithoutABlock) {
    const Counts with_all{2, 3, 1, 1, 2, true};
    const Counts iono_only{2, 3, 0, 0, 0, false};
    const StationHarvestBlock a = layoutOf(with_all, true);
    for (BlockSegment s : kSegments) EXPECT_EQ(a.segmentBlock(s), a.blk);
    const StationHarvestBlock b = layoutOf(iono_only, true);
    EXPECT_EQ(b.segmentBlock(BlockSegment::Iono), b.blk);
    EXPECT_EQ(b.segmentBlock(BlockSegment::RxClock), nullptr);
    EXPECT_EQ(b.segmentBlock(BlockSegment::RxCodeBias), nullptr);
    EXPECT_EQ(b.segmentBlock(BlockSegment::PhaseColour), nullptr);
    EXPECT_EQ(b.segmentBlock(BlockSegment::Zwd), nullptr);
    const StationHarvestBlock c = layoutOf(with_all, false);
    for (BlockSegment s : kSegments) EXPECT_EQ(c.segmentBlock(s), nullptr);
    const StationHarvestBlock skipped;   // a station the loop head skipped
    EXPECT_FALSE(skipped.ok);
    for (BlockSegment s : kSegments) EXPECT_EQ(skipped.segmentBlock(s), nullptr);
}

// 3: rowOf is the loop head's row offset, with or without a block
TEST(NetworkHarvestLayout, RowOfIsTheLoopHeadOffset) {
    const Counts c{4, 2, 1, 3, 2, true};
    for (bool blk : {true, false}) {
        const StationHarvestBlock sb = layoutOf(c, blk);
        EXPECT_EQ(sb.rowOf(BlockSegment::Iono), 4);
        EXPECT_EQ(sb.rowOf(BlockSegment::RxClock), 6);
        EXPECT_EQ(sb.rowOf(BlockSegment::RxCodeBias), 7);
        EXPECT_EQ(sb.rowOf(BlockSegment::PhaseColour), 10);
        EXPECT_EQ(sb.rowOf(BlockSegment::Zwd), 12);
    }
}
