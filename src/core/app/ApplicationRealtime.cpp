/**
 * @file ApplicationRealtime.cpp
 * @brief Implementation of Application::runRealTime (NTRIP / streaming pipeline).
 *
 * Extracted from Application.cpp to keep the per-mode processing
 * pipelines in dedicated translation units.  The class definition,
 * constructor, shared helpers, and the PPP/PPP-RTK
 * @c Application::run() entry point still live in Application.cpp.
 */

#include "app/Application.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductResolver.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/RtcmStreamReader.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "slip/MwSlipDetector.hpp"
#include <chrono>
#include <set>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <fmt/format.h>

namespace GnssProcessor {

int Application::runRealTime() {
    auto log = log_manager_.getLogger("main_log");
    log->info("Starting real-time processing mode");

    // --- Phase 1b: Resolve product data (still needed for orbits/clocks) ---
    if (!resolveData()) return 1;

    // --- Initialize real-time engine ---
    RealTimeEngine engine(config_, log_manager_);
    if (!engine.initialize()) {
        log->error("Failed to initialize real-time engine");
        return 1;
    }

    const auto& rt_config = config_.getRealTimeConfig();

    // --- Select input source ---
    if (rt_config.input_type == "file") {
        // File replay mode: read RTCM binary file and feed frames.
        log->info("Real-time file replay from: {}", rt_config.input_path);

        std::ifstream stream(rt_config.input_path, std::ios::binary);
        if (!stream.is_open()) {
            log->error("Failed to open RTCM replay file: {}", rt_config.input_path);
            return 1;
        }

        RtcmStreamReader reader;
        constexpr size_t kChunkSize = 4096;
        std::vector<uint8_t> chunk(kChunkSize);

        auto wall_start = std::chrono::high_resolution_clock::now();

        while (stream.good()) {
            stream.read(reinterpret_cast<char*>(chunk.data()), kChunkSize);
            auto bytes_read = static_cast<size_t>(stream.gcount());
            if (bytes_read == 0) break;

            reader.feed(chunk.data(), bytes_read);

            while (auto frame = reader.nextFrame()) {
                engine.feedRtcmFrame(*frame);
            }
        }

        auto wall_end = std::chrono::high_resolution_clock::now();
        auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
        log->info("File replay complete: {} epochs in {} ms, {} frames extracted, {} bytes discarded",
                  engine.epochCount(), dur_ms,
                  reader.framesExtracted(), reader.bytesDiscarded());
    }
    else if (rt_config.input_type == "ntrip") {
        // NTRIP live stream mode.
        NtripConfig ntrip_cfg;
        ntrip_cfg.host              = rt_config.ntrip_host;
        ntrip_cfg.port              = rt_config.ntrip_port;
        ntrip_cfg.mountpoint        = rt_config.ntrip_mountpoint;
        ntrip_cfg.username          = rt_config.ntrip_user;
        ntrip_cfg.password          = rt_config.ntrip_password;
        ntrip_cfg.nmea_gga          = rt_config.nmea_gga;
        ntrip_cfg.reconnect_delay_s = rt_config.reconnect_delay_s;

        NtripClient client(ntrip_cfg, log_manager_);
        if (!client.start()) {
            log->error("Failed to start NTRIP client");
            return 1;
        }

        log->info("NTRIP client started, streaming from {}:{}/{}",
                  ntrip_cfg.host, ntrip_cfg.port, ntrip_cfg.mountpoint);

        RtcmStreamReader reader;
        std::vector<uint8_t> buf;

        // Main processing loop — runs until the connection is closed or
        // the process is interrupted.
        while (client.isConnected()) {
            size_t n = client.read(buf, 4096);
            if (n == 0) continue;

            reader.feed(buf);

            while (auto frame = reader.nextFrame()) {
                engine.feedRtcmFrame(*frame);
            }
        }

        client.stop();
        log->info("NTRIP session ended: {} epochs processed", engine.epochCount());
    }
    else {
        log->error("Unsupported real-time input_type: '{}'", rt_config.input_type);
        return 1;
    }

    return 0;
}

} // namespace GnssProcessor
