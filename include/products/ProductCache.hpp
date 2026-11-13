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
    /** @brief Deleted: the cache is a singleton, so a second copy of the data must not exist. */
    ProductCache(const ProductCache&) = delete;
    /**
     * @brief Deleted: copy assignment would duplicate the singleton.
     * @return Not applicable; the operator cannot be called.
     */
    ProductCache& operator=(const ProductCache&) = delete;
    /** @brief Deleted: the singleton instance cannot be moved from. */
    ProductCache(ProductCache&&) = delete;
    /**
     * @brief Deleted: move assignment would empty the singleton.
     * @return Not applicable; the operator cannot be called.
     */
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
    /**
     * @brief Cached precise-orbit records (one per satellite and epoch).
     * @return Reference to the shared vector; empty before loadProducts(), and invalidated
     *         by clearCache() unless a getOrbitsShared() handle is held.
     */
    const std::vector<OrbitData>& getOrbits() const { return *orbits_; }
    /**
     * @brief Cached precise satellite clock records.
     * @return Reference to the shared vector; empty before loadProducts(), and invalidated
     *         by clearCache() unless a getClocksShared() handle is held.
     */
    const std::vector<ClockData>& getClocks() const { return *clocks_; }
    /**
     * @brief Cached antenna models (phase center offsets and variations).
     * @return Reference to the shared vector; empty before loadProducts(), and invalidated
     *         by clearCache() unless a getAntennasShared() handle is held.
     */
    const std::vector<AntennaData>& getAntennas() const { return *antennas_; }
    /**
     * @brief Cached satellite bias records.
     * @return Reference to the shared vector; empty before loadProducts(), and invalidated
     *         by clearCache() unless a getBiasesShared() handle is held.
     */
    const std::vector<BiasData>& getBiases() const { return *biases_; }
    /**
     * @brief Cached Earth rotation parameter records.
     * @return Reference to the shared vector; empty before loadProducts(), and invalidated
     *         by clearCache() unless a getErpsShared() handle is held.
     */
    const std::vector<ErpData>& getErps() const { return *erps_; }
    /**
     * @brief Cached GPS Klobuchar ionosphere coefficients.
     * @return Reference to the 8 coefficients (alpha0..alpha3, then beta0..beta3), or an
     *         empty vector when none were parsed or after clearCache().
     */
    const std::vector<double>& getKlobucharCoeffs() const { return klobuchar_coeffs_; }
    /**
     * @brief Cached per-system satellite clock reference signal pairs.
     * @return Reference to the map from system character ('G', 'E', ...) to the two code
     *         signals that system's satellite clock refers to; empty when no bias file
     *         description was parsed.  Not reset by clearCache().
     */
    const ClockRefMap& getClockRefSignals() const { return clock_ref_signals_; }

    // Shared owners. Per-instance model components (Geometry, GnssCorrections) hold
    // these so they reference the single cached copy instead of each copying the
    // read-only product and antenna vectors (MB to GB). Without them, a network run
    // holds one copy per station (about 0.5 GB × N stations). The vectors are const,
    // so sharing is safe; an SSR consumer that overrides clocks rebinds its own
    // shared_ptr.
    /**
     * @brief Shared owner of the cached orbit records.
     * @return Never null; keeps the vector alive after clearCache() until released.
     */
    std::shared_ptr<const std::vector<OrbitData>>   getOrbitsShared() const { return orbits_; }
    /**
     * @brief Shared owner of the cached clock records.
     * @return Never null; keeps the vector alive after clearCache() until released.
     */
    std::shared_ptr<const std::vector<ClockData>>   getClocksShared() const { return clocks_; }
    /**
     * @brief Shared owner of the cached antenna models.
     * @return Never null; keeps the vector alive after clearCache() until released.
     */
    std::shared_ptr<const std::vector<AntennaData>> getAntennasShared() const { return antennas_; }
    /**
     * @brief Shared owner of the cached satellite bias records.
     * @return Never null; keeps the vector alive after clearCache() until released.
     */
    std::shared_ptr<const std::vector<BiasData>>    getBiasesShared() const { return biases_; }
    /**
     * @brief Shared owner of the cached Earth rotation parameter records.
     * @return Never null; keeps the vector alive after clearCache() until released.
     */
    std::shared_ptr<const std::vector<ErpData>>     getErpsShared() const { return erps_; }

    // Get product counts for diagnostics
    /** @brief Number of cached orbit records. @return Size of getOrbits(); 0 if not loaded. */
    size_t getOrbitCount() const { return orbits_->size(); }
    /** @brief Number of cached clock records. @return Size of getClocks(); 0 if not loaded. */
    size_t getClockCount() const { return clocks_->size(); }
    /** @brief Number of cached antenna models. @return Size of getAntennas(); 0 if not loaded. */
    size_t getAntennaCount() const { return antennas_->size(); }
    /** @brief Number of cached bias records. @return Size of getBiases(); 0 if not loaded. */
    size_t getBiasCount() const { return biases_->size(); }
    /** @brief Number of cached ERP records. @return Size of getErps(); 0 if not loaded. */
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