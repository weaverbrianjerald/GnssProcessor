#include "ProductManager.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace GnssProcessor {

    ProductManager::ProductManager(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager) {
        log_manager_.getLogger()->info("ProductManager initialized");
    }

    bool ProductManager::loadProductList(const std::filesystem::path& list_path) {
        try {
            if (!std::filesystem::exists(list_path)) {
                log_manager_.getLogger()->error("Product list file not found: {}", list_path.string());
                return false;
            }

            std::ifstream file(list_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open product list: {}", list_path.string());
                return false;
            }

            std::string line;
            size_t line_count = 0;
            products_.clear(); // Clear existing products
            while (std::getline(file, line)) {
                ++line_count;
                // Skip empty or comment lines
                if (line.empty() || line[0] == '#') continue;
                // Remove leading/trailing whitespace
                line.erase(0, line.find_first_not_of(" \t"));
                line.erase(line.find_last_not_of(" \t") + 1);
                if (line.empty()) continue;

                // Convert to std::filesystem::path for robust parsing
                std::filesystem::path full_path = line;
                if (full_path.empty()) {
                    log_manager_.getLogger()->warn("Empty path at line {}", line_count);
                    continue;
                }

                // Extract product type from the parent directory name (e.g., "clk" from "clk\...CLK")
                std::string type = full_path.parent_path().filename().string();
                // Normalize product type to lowercase
                std::transform(type.begin(), type.end(), type.begin(), ::tolower);
                // Map specific types for consistency
                if (type == "clock") type = "clk";
                else if (type == "orbit") type = "sp3";
                else if (type == "broadcast") type = "rnx";

                // Store full path
                products_[type].push_back(full_path);
                log_manager_.getLogger()->debug("Loaded product type {}: {}", type, full_path.string());
            }

            if (products_.empty()) {
                log_manager_.getLogger()->warn("No products loaded from {}", list_path.string());
                return false;
            }
            log_manager_.getLogger()->info("Loaded {} product types from {}", products_.size(), list_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error loading product list {}: {}", list_path.string(), e.what());
            return false;
        }
    }

    bool ProductManager::validateProducts() const {
        bool all_valid = true;
        for (const auto& [type, paths] : products_) {
            for (const auto& path : paths) {
                if (!std::filesystem::exists(path)) {
                    log_manager_.getLogger()->warn("Product not found for type {}: {}", type, path.string());
                    all_valid = false;
                }
            }
        }
        return all_valid;
    }

    std::filesystem::path ProductManager::getProductPath(const std::string& product_type) const {
        std::string type_lower = product_type;
        std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(), ::tolower);
        auto it = products_.find(type_lower);
        if (it == products_.end() || it->second.empty()) {
            log_manager_.getLogger()->debug("No paths found for product type {}", type_lower);
            return std::filesystem::path();
        }
        return it->second[0]; // Return first path
    }

    std::vector<std::filesystem::path> ProductManager::getProductPaths(const std::string& product_type) const {
        std::string type_lower = product_type;
        std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(), ::tolower);
        auto it = products_.find(type_lower);
        if (it == products_.end()) {
            log_manager_.getLogger()->debug("No paths found for product type {}", type_lower);
            return {};
        }
        return it->second;
    }

    std::vector<std::filesystem::path> ProductManager::getOrbitPaths() const {
        return getProductPaths("sp3");
    }

} // namespace GnssProcessor