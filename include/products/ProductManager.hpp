#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

namespace GnssProcessor {

    /**
     * @brief Product file registry read from a plain-text list of paths.
     *
     * loadProductList() reads one path per line and files each path under a product type
     * taken from the name of its parent directory (lower-cased, with @c clock mapped to
     * @c clk, @c orbit to @c sp3 and @c broadcast to @c rnx).  The accessors then return
     * the registered paths by type.  Type names passed to the accessors compare without
     * regard to case.
     */
    class ProductManager {
    public:
        /**
         * @brief Construct an empty registry bound to one configuration and one logger.
         *
         * Both arguments are held by reference, so each must outlive the manager.
         *
         * @param config      Loaded configuration (held but not read by the current methods).
         * @param log_manager Logging facility used for load and validation messages.
         */
        ProductManager(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Read a product list file and replace the registry with its contents.
         *
         * Blank lines and lines starting with a hash sign are skipped, and surrounding blanks
         * and tabs are trimmed.  Once the file has been opened, previously registered products
         * are discarded, also when no usable line follows; a missing file leaves them intact.
         *
         * @param list_path Text file with one product file path per line.
         * @return True when at least one product path was registered; false when the file
         *         is missing, cannot be opened, holds no usable line, or reading threw
         *         (the cause is logged).
         */
        bool loadProductList(const std::filesystem::path& list_path);

        /**
         * @brief Check that every registered product file exists on disk.
         *
         * Each missing file is logged as a warning.
         *
         * @return True when every registered path exists (also when nothing is registered);
         *         false when at least one is missing.
         */
        bool validateProducts() const;

        /**
         * @brief First registered path of a product type.
         * @param product_type Product type name such as @c sp3, @c clk or @c rnx (case-insensitive).
         * @return The first path registered for the type, or an empty path when the type is
         *         unknown.
         */
        std::filesystem::path getProductPath(const std::string& product_type) const;

        /**
         * @brief All registered paths of a product type.
         * @param product_type Product type name such as @c sp3, @c clk or @c rnx (case-insensitive).
         * @return The paths in list-file order; empty when the type is unknown.
         */
        std::vector<std::filesystem::path> getProductPaths(const std::string& product_type) const;

        /**
         * @brief All registered orbit (SP3) paths.
         * @return The paths filed under the @c sp3 type, in list-file order; empty when none.
         */
        std::vector<std::filesystem::path> getOrbitPaths() const;

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        std::map<std::string, std::vector<std::filesystem::path>> products_;
    };

} // namespace GnssProcessor