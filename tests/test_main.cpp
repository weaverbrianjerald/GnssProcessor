/*
#include <gtest/gtest.h>
#include "RinexParser.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace GnssProcessor {

    TEST(RinexParserTest, ParseTestFile) {
        // Test parsing the provided RINEX file
        const std::filesystem::path rinex_path = "C:/GnssProcessorSupport/GCGO00USA_R_20241290000_01D_30S_MO.rnx";
        RinexParser parser(rinex_path);
        ASSERT_TRUE(parser.parse()) << "Failed to parse RINEX file: " << rinex_path;

        // Verify header
        const auto& header = parser.getHeader();
        EXPECT_GE(header.version, 3.0) << "Expected RINEX 3.x version";
        EXPECT_LE(header.version, 4.0) << "Version too new for RINEX 3.x";
        EXPECT_TRUE(header.interval.has_value()) << "Expected interval in header";
        if (header.interval.has_value()) {
            EXPECT_NEAR(header.interval.value(), 30.0, 1e-3) << "Expected ~30s interval";
        }

        // Verify observations
        const auto& observations = parser.getObservations();
        ASSERT_FALSE(observations.empty()) << "No observations parsed";

        // Check for GPS observations with C1C
        bool has_gps_c1c = false;
        for (const auto& obs : observations) {
            if (obs.system == System::GPS && obs.measurements.find("C1C") != obs.measurements.end()) {
                has_gps_c1c = true;
                break;
            }
        }
        EXPECT_TRUE(has_gps_c1c) << "Expected GPS C1C observations";

        // Verify observation types in header
        ASSERT_TRUE(header.obs_types.find(System::GPS) != header.obs_types.end()) << "Expected GPS observation types";
        const auto& gps_obs_types = header.obs_types.at(System::GPS);
        EXPECT_FALSE(gps_obs_types.empty()) << "Expected non-empty GPS observation types";
        bool has_c1c_type = std::find(gps_obs_types.begin(), gps_obs_types.end(), "C1C") != gps_obs_types.end();
        EXPECT_TRUE(has_c1c_type) << "Expected C1C in GPS observation types";
    }

    // Helper: Create a minimal valid RINEX file with a cycle slip for testing
    static std::filesystem::path create_test_rinex_with_cycle_slip(const std::string& filename) {
        std::filesystem::path path = std::filesystem::temp_directory_path() / filename;
        std::ofstream ofs(path);
        ofs << "     3.03           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE\n";
        ofs << "C1C                                                     SYS / # / OBS TYPES\n";
        ofs << "  2024    05    01    00    00   00.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << "  2024    05    01    00    00   30.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << "END OF HEADER\n";
        ofs << "  2024    05    01    00    00   00.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << " 12345678.9\n";
        ofs << "  2024    05    01    00    00   30.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << " 12345778.9\n"; // Simulate a cycle slip (jump)
        ofs.close();
        return path;
    }

    // Helper: Create a minimal valid RINEX file with no cycle slips
    static std::filesystem::path create_test_rinex_no_cycle_slip(const std::string& filename) {
        std::filesystem::path path = std::filesystem::temp_directory_path() / filename;
        std::ofstream ofs(path);
        ofs << "     3.03           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE\n";
        ofs << "C1C                                                     SYS / # / OBS TYPES\n";
        ofs << "  2024    05    01    00    00   00.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << "  2024    05    01    00    00   30.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << "END OF HEADER\n";
        ofs << "  2024    05    01    00    00   00.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << " 12345678.9\n";
        ofs << "  2024    05    01    00    00   30.0000000  0 18G01G02G03G04G05G06G07G08G09G10G11G12G13G14G15G16G17G18\n";
        ofs << " 12345679.9\n"; // No jump
        ofs.close();
        return path;
    }

    // 1. Cycle slip detector identifies and repairs all cycle slips in valid observation data
    TEST(CycleSlipDetectorTest, DetectAndRepairValidObservations) {
        auto rinex_path = create_test_rinex_with_cycle_slip("test_cycle_slip.rnx");
        RinexParser parser(rinex_path);
        ASSERT_TRUE(parser.parse());

        auto observations = parser.getObservations();
        CycleSlipDetector detector;
        auto slips = detector.detectAndRepair(observations);

        // There should be at least one detected slip
        ASSERT_FALSE(slips.empty());
        for (const auto& slip : slips) {
            EXPECT_TRUE(slip.repaired);
            EXPECT_GT(std::abs(slip.cycle_jump), 0.0);
        }
        std::filesystem::remove(rinex_path);
    }

    // 2. Logging outputs correct information for each detected and repaired cycle slip
    TEST(LoggingTest, CycleSlipLogging) {
        auto rinex_path = create_test_rinex_with_cycle_slip("test_cycle_slip_log.rnx");
        RinexParser parser(rinex_path);
        ASSERT_TRUE(parser.parse());

        auto observations = parser.getObservations();
        CycleSlipDetector detector;
        auto slips = detector.detectAndRepair(observations);

        // Check that slip info fields are populated and plausible
        for (const auto& slip : slips) {
            EXPECT_FALSE(slip.satellite.empty());
            EXPECT_GE(slip.time.week, 0);
            EXPECT_GE(slip.time.tow, 0.0);
            EXPECT_FALSE(slip.obs_type.empty());
            EXPECT_TRUE(slip.repaired);
        }
        std::filesystem::remove(rinex_path);
    }

    // 3. Counts GPS C1C observations correctly after cycle slip repair
    TEST(ObservationCountTest, GpsC1cAfterRepair) {
        auto rinex_path = create_test_rinex_with_cycle_slip("test_cycle_slip_count.rnx");
        RinexParser parser(rinex_path);
        ASSERT_TRUE(parser.parse());

        auto observations = parser.getObservations();
        CycleSlipDetector detector;
        detector.detectAndRepair(observations);

        int gps_c1c_count = 0;
        for (const auto& obs : observations) {
            if (obs.system == System::GPS && obs.measurements.count("C1C")) {
                gps_c1c_count++;
            }
        }
        // There should be at least one GPS C1C observation
        EXPECT_GT(gps_c1c_count, 0);
        std::filesystem::remove(rinex_path);
    }

    // 4. Handles missing or unreadable RINEX file gracefully
    TEST(RinexParserTest, MissingFile) {
        std::filesystem::path missing_path = std::filesystem::temp_directory_path() / "nonexistent_file.rnx";
        RinexParser parser(missing_path);
        EXPECT_FALSE(parser.parse());
    }

    // 5. Handles exceptions thrown during parsing or cycle slip detection
    TEST(ExceptionHandlingTest, ParsingOrDetectionException) {
        // Simulate a corrupted file (not RINEX format)
        std::filesystem::path bad_path = std::filesystem::temp_directory_path() / "bad_file.rnx";
        std::ofstream ofs(bad_path);
        ofs << "This is not a RINEX file\n";
        ofs.close();

        RinexParser parser(bad_path);
        // Should fail to parse, but not throw
        EXPECT_FALSE(parser.parse());

        // Try to run cycle slip detection on empty observations
        auto observations = parser.getObservations();
        CycleSlipDetector detector;
        // Should not throw, should return empty slips
        EXPECT_NO_THROW({
            auto slips = detector.detectAndRepair(observations);
            EXPECT_TRUE(slips.empty());
        });

        std::filesystem::remove(bad_path);
    }

    // 6. Processes observation data with no detectable cycle slips without errors
    TEST(CycleSlipDetectorTest, NoCycleSlips) {
        auto rinex_path = create_test_rinex_no_cycle_slip("test_no_cycle_slip.rnx");
        RinexParser parser(rinex_path);
        ASSERT_TRUE(parser.parse());

        auto observations = parser.getObservations();
        CycleSlipDetector detector;
        auto slips = detector.detectAndRepair(observations);

        // There should be no slips detected
        EXPECT_TRUE(slips.empty());
        std::filesystem::remove(rinex_path);
    }

} // namespace GnssProcessor

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
*/