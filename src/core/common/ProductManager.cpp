#include "ProductManager.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace GnssProcessor {

    ProductManager::ProductManager(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager) {
        log_manager_.getLogger()->info("ProductManager initialized");
    }

    bool ProductManager::loadProductList(const std::filesystem::path& list_path) {
        try {
            std::ifstream file(list_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open product list: {}", list_path.string());
                return false;
            }
            std::string line;
            while (std::getline(file, line)) {
                // Placeholder: Parse product type and path
                products_[line] = list_path.parent_path() / line;
            }
            log_manager_.getLogger()->info("Loaded {} products from {}", products_.size(), list_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error loading product list {}: {}", list_path.string(), e.what());
            return false;
        }
    }

    bool ProductManager::validateProducts() const {
        for (const auto& [type, path] : products_) {
            if (!std::filesystem::exists(path)) {
                log_manager_.getLogger()->warn("Product not found: {}", path.string());
                return false;
            }
        }
        return true;
    }

    std::filesystem::path ProductManager::getProductPath(const std::string& product_type) const {
        auto it = products_.find(product_type);
        return it != products_.end() ? it->second : std::filesystem::path();
    }

    std::vector<std::filesystem::path> ProductManager::getOrbitPaths() const {
        std::vector<std::filesystem::path> paths;
        for (const auto& [type, path] : products_) {
            if (type.find("orbit") != std::string::npos) {
                paths.push_back(path);
            }
        }
        return paths;
    }

} // namespace GnssProcessor