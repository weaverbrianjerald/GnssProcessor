#pragma once
/**
 * @file ProductCache.hpp
 * @brief Singleton cache for GNSS product data to avoid repeated loading.
 *
 * Provides a thread-safe singleton that loads and caches all product data
 * (orbits, clocks, antennas, biases, ERPs, Klobuchar coefficients) once
 * and shares them across all model components.
 *
 * NOTE: Binary persistence helpers for antenna data (save/load) have been
 * added to avoid reparsing ATX on subsequent runs. Helpers are declared
 * below as private static methods.
 */

#include "PppTypes.hpp"
#include "LogManager.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <string>

namespace GnssProcessor {

// Forward declaration
class ProductParser;

/**
 * @class ProductCache
 * @brief Thread-safe singleton cache for GNSS product data.
 *
 * This class implements a singleton pattern with lazy initialization
 * to ensure products are loaded only once and shared across all
 * components that need them (PppModel, Geometry, Ionosphere, etc.).
 *
 * Thread-safety is guaranteed for both initialization and access.
 */
class ProductCache {
public:
    /**
     * @brief Get the singleton instance of ProductCache.
     * @return Reference to the singleton ProductCache instance.
     */
    static ProductCache& getInstance();

    // Delete copy/move operations for singleton
    ProductCache(const ProductCache&) = delete;
    ProductCache& operator=(const ProductCache&) = delete;
    ProductCache(ProductCache&&) = delete;
    ProductCache& operator=(ProductCache&&) = delete;

    /**
     * @brief Load all products from the parser if not already loaded.
     * 
     * This method is thread-safe and will only load products once.
     * Subsequent calls will return immediately if products are already loaded.
     * 
     * @param parser ProductParser instance containing parsed products.
     * @param log_manager LogManager for logging load status.
     * @return true if products were successfully loaded or already loaded, false on error.
     */
    bool loadProducts(ProductParser& parser, LogManager& log_manager);

    /**
     * @brief Check if products have been loaded.
     * @return true if products are loaded, false otherwise.
     */
    bool isLoaded() const { return loaded_.load(std::memory_order_acquire); }

    /**
     * @brief Clear the cache and reset to unloaded state.
     * 
     * This should be called when products need to be reloaded
     * (e.g., when processing a new dataset).
     */
    void clearCache();

    // Const accessors for cached products
    const std::vector<OrbitData>& getOrbits() const { return orbits_; }
    const std::vector<ClockData>& getClocks() const { return clocks_; }
    const std::vector<AntennaData>& getAntennas() const { return antennas_; }
    const std::vector<BiasData>& getBiases() const { return biases_; }
    const std::vector<ErpData>& getErps() const { return erps_; }
    const std::vector<double>& getKlobucharCoeffs() const { return klobuchar_coeffs_; }
    const std::vector<SunEphemeris>& getSunEphemeris() const { return sun_ephemeris_; }

    // Get product counts for diagnostics
    size_t getOrbitCount() const { return orbits_.size(); }
    size_t getClockCount() const { return clocks_.size(); }
    size_t getAntennaCount() const { return antennas_.size(); }
    size_t getBiasCount() const { return biases_.size(); }
    size_t getErpCount() const { return erps_.size(); }

    // Delete copy constructor and assignment operator
    //ProductCache(const ProductCache&) = delete;
    //ProductCache& operator=(const ProductCache&) = delete;

private:
    // Private constructor for singleton
    ProductCache() = default;
    ~ProductCache() = default;

    // Thread-safe loading flag
    std::atomic<bool> loaded_{false};
    
    // Mutex for thread-safe loading
    mutable std::mutex load_mutex_;
    
    // Cached product data
    std::vector<OrbitData> orbits_;
    std::vector<ClockData> clocks_;
    std::vector<AntennaData> antennas_;
    std::vector<BiasData> biases_;
    std::vector<ErpData> erps_;
    std::vector<double> klobuchar_coeffs_;
    std::vector<SunEphemeris> sun_ephemeris_;

    //
    // Binary persistence helpers for antenna data
    //
    // These helpers are used internally by loadProducts() to attempt to
    // load/store `antennas_` to a compact binary file in the project
    // directory (default filename: "antennas.bin"). They are intentionally
    // private and static to avoid adding state to the singleton beyond the
    // existing cached vectors.
    //

    // Return the default antenna binary path (project current working directory).
    static std::filesystem::path getAntennaBinPath() noexcept;

    // Save the provided antennas to the given path (logs via LogManager).
    // Returns true on success.
    static bool saveAntennasToBinary(const std::vector<AntennaData>& antennas, LogManager& log_manager);

    // Load antennas from the default binary cache into the provided vector.
    // Returns true if the file existed and loaded successfully.
    static bool loadAntennasFromBinary(std::vector<AntennaData>& antennas, LogManager& log_manager);

};

} // namespace GnssProcessor