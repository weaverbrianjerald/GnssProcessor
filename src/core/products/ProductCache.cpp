/**
 * @file ProductCache.cpp
 * @brief Implementation of the singleton ProductCache for GNSS products.
 */

#include "products/ProductCache.hpp"
#include "products/ProductParser.hpp"
#include <chrono>
#include <fstream>
#include <filesystem>
#include <system_error>

namespace GnssProcessor {

ProductCache& ProductCache::getInstance() {
    // Thread-safe initialization in C++11 and later
    static ProductCache instance;
    return instance;
}

static std::filesystem::path getAntennaBinPath() {
    return std::filesystem::current_path() / "antennas.bin";
}

static bool saveAntennasToBinary(const std::vector<AntennaData>& antennas, LogManager& log_manager) {
    auto path = getAntennaBinPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec); // noop if already exists

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        log_manager.getLogger("product_cache")->error("Failed to open '{}' for writing antenna cache", path.string());
        return false;
    }

    try {
        uint64_t count = static_cast<uint64_t>(antennas.size());
        ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

        for (const auto& a : antennas) {
            // type
            uint64_t tlen = static_cast<uint64_t>(a.type.size());
            ofs.write(reinterpret_cast<const char*>(&tlen), sizeof(tlen));
            if (tlen) ofs.write(a.type.data(), static_cast<std::streamsize>(tlen));

            // dazi, dzen
            ofs.write(reinterpret_cast<const char*>(&a.dazi), sizeof(a.dazi));
            ofs.write(reinterpret_cast<const char*>(&a.dzen), sizeof(a.dzen));

            // valid_from, valid_until (week as int32_t, tow as double)
            int32_t vf_week = static_cast<int32_t>(a.valid_from.week);
            int32_t vu_week = static_cast<int32_t>(a.valid_until.week);
            ofs.write(reinterpret_cast<const char*>(&vf_week), sizeof(vf_week));
            ofs.write(reinterpret_cast<const char*>(&a.valid_from.tow), sizeof(a.valid_from.tow));
            ofs.write(reinterpret_cast<const char*>(&vu_week), sizeof(vu_week));
            ofs.write(reinterpret_cast<const char*>(&a.valid_until.tow), sizeof(a.valid_until.tow));

            // pco map
            uint64_t pco_count = static_cast<uint64_t>(a.pco.size());
            ofs.write(reinterpret_cast<const char*>(&pco_count), sizeof(pco_count));
            for (const auto& kv : a.pco) {
                uint64_t klen = static_cast<uint64_t>(kv.first.size());
                ofs.write(reinterpret_cast<const char*>(&klen), sizeof(klen));
                if (klen) ofs.write(kv.first.data(), static_cast<std::streamsize>(klen));

                // Eigen::Vector3d
                double coords[3] = { kv.second.x(), kv.second.y(), kv.second.z() };
                ofs.write(reinterpret_cast<const char*>(coords), sizeof(coords));
            }

            // pcv map
            uint64_t pcv_count = static_cast<uint64_t>(a.pcv.size());
            ofs.write(reinterpret_cast<const char*>(&pcv_count), sizeof(pcv_count));
            for (const auto& kv : a.pcv) {
                uint64_t klen = static_cast<uint64_t>(kv.first.size());
                ofs.write(reinterpret_cast<const char*>(&klen), sizeof(klen));
                if (klen) ofs.write(kv.first.data(), static_cast<std::streamsize>(klen));

                const auto& table = kv.second;
                uint64_t rows = static_cast<uint64_t>(table.size());
                uint64_t cols = rows ? static_cast<uint64_t>(table.front().size()) : 0;
                ofs.write(reinterpret_cast<const char*>(&rows), sizeof(rows));
                ofs.write(reinterpret_cast<const char*>(&cols), sizeof(cols));
                for (uint64_t r = 0; r < rows; ++r) {
                    for (uint64_t c = 0; c < cols; ++c) {
                        double v = table[r][c];
                        ofs.write(reinterpret_cast<const char*>(&v), sizeof(v));
                    }
                }
            }
        }

        ofs.close();
        log_manager.getLogger("product_cache")->info("Saved {} antenna entries to '{}'", antennas.size(), path.string());
        return true;
    }
    catch (const std::exception& e) {
        log_manager.getLogger("product_cache")->error("Exception while writing antenna cache '{}': {}", path.string(), e.what());
        return false;
    }
}

static bool loadAntennasFromBinary(std::vector<AntennaData>& antennas, LogManager& log_manager) {
    auto path = getAntennaBinPath();
    if (!std::filesystem::exists(path)) {
        log_manager.getLogger("product_cache")->debug("Antenna binary cache '{}' does not exist", path.string());
        return false;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        log_manager.getLogger("product_cache")->warn("Failed to open antenna cache '{}'", path.string());
        return false;
    }

    try {
        uint64_t count = 0;
        ifs.read(reinterpret_cast<char*>(&count), sizeof(count));
        antennas.clear();
        antennas.reserve(static_cast<size_t>(count));

        for (uint64_t i = 0; i < count; ++i) {
            AntennaData a;

            uint64_t tlen = 0;
            ifs.read(reinterpret_cast<char*>(&tlen), sizeof(tlen));
            if (tlen) {
                std::string t;
                t.resize(static_cast<size_t>(tlen));
                ifs.read(&t[0], static_cast<std::streamsize>(tlen));
                a.type = std::move(t);
            }
            else {
                a.type.clear();
            }

            ifs.read(reinterpret_cast<char*>(&a.dazi), sizeof(a.dazi));
            ifs.read(reinterpret_cast<char*>(&a.dzen), sizeof(a.dzen));

            int32_t vf_week = 0;
            int32_t vu_week = 0;
            ifs.read(reinterpret_cast<char*>(&vf_week), sizeof(vf_week));
            ifs.read(reinterpret_cast<char*>(&a.valid_from.tow), sizeof(a.valid_from.tow));
            ifs.read(reinterpret_cast<char*>(&vu_week), sizeof(vu_week));
            ifs.read(reinterpret_cast<char*>(&a.valid_until.tow), sizeof(a.valid_until.tow));
            a.valid_from.week = vf_week;
            a.valid_until.week = vu_week;

            // pco map
            uint64_t pco_count = 0;
            ifs.read(reinterpret_cast<char*>(&pco_count), sizeof(pco_count));
            for (uint64_t p = 0; p < pco_count; ++p) {
                uint64_t klen = 0;
                ifs.read(reinterpret_cast<char*>(&klen), sizeof(klen));
                std::string key;
                if (klen) {
                    key.resize(static_cast<size_t>(klen));
                    ifs.read(&key[0], static_cast<std::streamsize>(klen));
                }
                double coords[3];
                ifs.read(reinterpret_cast<char*>(coords), sizeof(coords));
                a.pco.emplace(std::move(key), Eigen::Vector3d(coords[0], coords[1], coords[2]));
            }

            // pcv map
            uint64_t pcv_count = 0;
            ifs.read(reinterpret_cast<char*>(&pcv_count), sizeof(pcv_count));
            for (uint64_t p = 0; p < pcv_count; ++p) {
                uint64_t klen = 0;
                ifs.read(reinterpret_cast<char*>(&klen), sizeof(klen));
                std::string key;
                if (klen) {
                    key.resize(static_cast<size_t>(klen));
                    ifs.read(&key[0], static_cast<std::streamsize>(klen));
                }

                uint64_t rows = 0, cols = 0;
                ifs.read(reinterpret_cast<char*>(&rows), sizeof(rows));
                ifs.read(reinterpret_cast<char*>(&cols), sizeof(cols));
                std::vector<std::vector<double>> table;
                table.resize(static_cast<size_t>(rows));
                for (uint64_t r = 0; r < rows; ++r) {
                    table[r].resize(static_cast<size_t>(cols));
                    for (uint64_t c = 0; c < cols; ++c) {
                        double v = 0.0;
                        ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
                        table[r][c] = v;
                    }
                }
                a.pcv.emplace(std::move(key), std::move(table));
            }

            antennas.push_back(std::move(a));
        }

        log_manager.getLogger("product_cache")->info("Loaded {} antenna entries from '{}'", antennas.size(), path.string());
        return true;
    }
    catch (const std::exception& e) {
        log_manager.getLogger("product_cache")->error("Exception while reading antenna cache '{}': {}", path.string(), e.what());
        antennas.clear();
        return false;
    }
}

bool ProductCache::loadProducts(ProductParser& parser, LogManager& log_manager) {
    // Fast path: already loaded
    if (loaded_.load(std::memory_order_acquire)) {
        log_manager.getLogger("product_cache")->debug("Products already cached, skipping reload");
        return true;
    }

    // Thread-safe loading with double-checked locking
    std::lock_guard<std::mutex> lock(load_mutex_);

    // Check again after acquiring lock
    if (loaded_.load(std::memory_order_relaxed)) {
        log_manager.getLogger("product_cache")->debug("Products loaded by another thread");
        return true;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        log_manager.getLogger("product_cache")->info("Loading products into cache (move semantics)...");

        // MOVE data from parser - this is the key optimization
        // The parser's vectors are left empty after this, which is fine
        // since we only load products once.
        orbits_ = parser.moveOrbits();
        clocks_ = parser.moveClocks();
        antennas_ = parser.moveAntennas();
        biases_ = parser.moveBiases();
        clock_ref_signals_ = parser.moveClockRefSignals();
        erps_ = parser.moveErps();
        klobuchar_coeffs_ = parser.moveKlobucharCoeffs();

        // Validate essential products
        if (orbits_.empty()) {
            log_manager.getLogger("product_cache")->warn("No orbit data loaded");
        }
        if (clocks_.empty()) {
            log_manager.getLogger("product_cache")->warn("No clock data loaded");
        }

        // Mark as loaded with release semantics
        loaded_.store(true, std::memory_order_release);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        log_manager.getLogger("product_cache")->info(
            "Product cache loaded in {} ms (move semantics): "
            "{} orbits, {} clocks, {} antennas, {} biases, {} ERPs, {} Klobuchar coeffs",
            duration.count(),
            orbits_.size(), clocks_.size(), antennas_.size(),
            biases_.size(), erps_.size(), klobuchar_coeffs_.size()
        );

        return true;
    }
    catch (const std::exception& e) {
        log_manager.getLogger("product_cache")->error(
            "Failed to load product cache: {}", e.what()
        );
        clearCache();
        return false;
    }
}

void ProductCache::clearCache() {
    std::lock_guard<std::mutex> lock(load_mutex_);

    // Clear and deallocate
    orbits_.clear();
    orbits_.shrink_to_fit();

    clocks_.clear();
    clocks_.shrink_to_fit();

    antennas_.clear();
    antennas_.shrink_to_fit();

    biases_.clear();
    biases_.shrink_to_fit();

    erps_.clear();
    erps_.shrink_to_fit();

    klobuchar_coeffs_.clear();
    klobuchar_coeffs_.shrink_to_fit();

    loaded_.store(false, std::memory_order_release);
}

} // namespace GnssProcessor