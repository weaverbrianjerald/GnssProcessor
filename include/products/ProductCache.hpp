#pragma once
/**
 * @file ProductCache.hpp
 * @brief Singleton cache for GNSS product data to avoid repeated loading.
 *
 * Provides a thread-safe singleton that loads and caches all product data
 * (orbits, clocks, antennas, biases, ERPs, Klobuchar coefficients, clock
 * reference signals) once and shares them across all model components.
 *
 * NOTE: Binary persistence helpers for antenna data (save/load, intended to
 * avoid reparsing ATX on later runs) are declared below as private static
 * methods. loadProducts() does not call them; see the note at their declarations.
 */

#include "common/PppTypes.hpp"
#include "logging/LogManager.hpp"
#include <memory>
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
 * Instance creation is thread-safe (function-local static), and loadProducts()
 * serializes loading with a mutex and double-checked flag. The accessors take no
 * lock: concurrent reads are safe once loading has completed, but not while
 * clearCache() runs.
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

    // Const accessors for cached products (deref the shared owner; always non-null).
    const std::vector<OrbitData>& getOrbits() const { return *orbits_; }
    const std::vector<ClockData>& getClocks() const { return *clocks_; }
    const std::vector<AntennaData>& getAntennas() const { return *antennas_; }
    const std::vector<BiasData>& getBiases() const { return *biases_; }
    const std::vector<ErpData>& getErps() const { return *erps_; }
    const std::vector<double>& getKlobucharCoeffs() const { return klobuchar_coeffs_; }
    const ClockRefMap& getClockRefSignals() const { return clock_ref_signals_; }

    // Shared owners. Per-instance model components (Geometry, GnssCorrections) hold
    // these so they reference the single cached copy instead of each copying the
    // read-only product and antenna vectors (MB to GB). Without them, a network run
    // holds one copy per station (about 0.5 GB × N stations). The vectors are const,
    // so sharing is safe; an SSR consumer that overrides clocks rebinds its own
    // shared_ptr.
    std::shared_ptr<const std::vector<OrbitData>>   getOrbitsShared() const { return orbits_; }
    std::shared_ptr<const std::vector<ClockData>>   getClocksShared() const { return clocks_; }
    std::shared_ptr<const std::vector<AntennaData>> getAntennasShared() const { return antennas_; }
    std::shared_ptr<const std::vector<BiasData>>    getBiasesShared() const { return biases_; }
    std::shared_ptr<const std::vector<ErpData>>     getErpsShared() const { return erps_; }

    // Get product counts for diagnostics
    size_t getOrbitCount() const { return orbits_->size(); }
    size_t getClockCount() const { return clocks_->size(); }
    size_t getAntennaCount() const { return antennas_->size(); }
    size_t getBiasCount() const { return biases_->size(); }
    size_t getErpCount() const { return erps_->size(); }

    // Delete copy constructor and assignment operator
    //ProductCache(const ProductCache&) = delete;
    //ProductCache& operator=(const ProductCache&) = delete;

private:
    // Private constructor for singleton
    ProductCache() = default;
    ~ProductCache() = default;

    /**
     * @brief Reset every cached product to empty and mark the cache unloaded.
     *
     * The body shared by clearCache() and the failure path of loadProducts().
     * The caller must hold @c load_mutex_; the mutex is not recursive, so taking it
     * here would deadlock a caller that already holds it.
     */
    void clearCacheLocked_();

    // Thread-safe loading flag
    std::atomic<bool> loaded_{false};
    
    // Mutex for thread-safe loading
    mutable std::mutex load_mutex_;
    
    // Cached product data.  The large read-only vectors are held behind a
    // shared_ptr<const ...> so model components can share the single copy (see the
    // getXShared accessors).  Initialized non-null (empty) so getX() is always safe
    // before loadProducts().
    std::shared_ptr<const std::vector<OrbitData>>   orbits_{std::make_shared<const std::vector<OrbitData>>()};
    std::shared_ptr<const std::vector<ClockData>>   clocks_{std::make_shared<const std::vector<ClockData>>()};
    std::shared_ptr<const std::vector<AntennaData>> antennas_{std::make_shared<const std::vector<AntennaData>>()};
    std::shared_ptr<const std::vector<BiasData>>    biases_{std::make_shared<const std::vector<BiasData>>()};
    std::shared_ptr<const std::vector<ErpData>>     erps_{std::make_shared<const std::vector<ErpData>>()};
    std::vector<double> klobuchar_coeffs_;
    ClockRefMap clock_ref_signals_;

    //
    // Binary persistence helpers for antenna data
    //
    // These helpers load/store `antennas_` to a compact binary file in the process
    // current working directory (filename "antennas.bin"). They are private and
    // static so the singleton holds no state beyond the cached vectors.
    // loadProducts() does not call them, and ProductCache.cpp defines same-named
    // file-local free functions rather than these members, so these declarations
    // have no definition.
    //

    // Return the default antenna binary path (process current working directory).
    static std::filesystem::path getAntennaBinPath() noexcept;

    // Save the provided antennas to the default path (logs via LogManager).
    // Returns true on success.
    static bool saveAntennasToBinary(const std::vector<AntennaData>& antennas, LogManager& log_manager);

    // Load antennas from the default binary cache into the provided vector.
    // Returns true if the file existed and loaded successfully.
    static bool loadAntennasFromBinary(std::vector<AntennaData>& antennas, LogManager& log_manager);

};

} // namespace GnssProcessor