#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

namespace GnssProcessor {

    class ProductManager {
    public:
        ProductManager(const GnssConfig& config, LogManager& log_manager);
        bool loadProductList(const std::filesystem::path& list_path);
        bool validateProducts() const;
        std::filesystem::path getProductPath(const std::string& product_type) const;
        std::vector<std::filesystem::path> getProductPaths(const std::string& product_type) const;
        std::vector<std::filesystem::path> getOrbitPaths() const;

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        std::map<std::string, std::vector<std::filesystem::path>> products_;
    };

} // namespace GnssProcessor