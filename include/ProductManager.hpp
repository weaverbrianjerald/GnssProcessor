#pragma once
#include <filesystem>
#include <string>
#include <map>
#include <vector>
#include "GnssConfig.hpp"
#include "LogManager.hpp"

namespace GnssProcessor {

    class ProductManager {
    public:
        ProductManager(const GnssConfig& config, LogManager& log_manager);
        bool loadProductList(const std::filesystem::path& product_list_file);
        std::filesystem::path getProductPath(const std::string& type) const;
        std::vector<std::filesystem::path> getOrbitPaths() const;
        bool validateProducts() const;

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        std::map<std::string, std::filesystem::path> products_; // Product type to path
    };

} // namespace GnssProcessor

//#endif // PRODUCT_MANAGER_HPP