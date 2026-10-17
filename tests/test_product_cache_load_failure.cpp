/**
 * @file test_product_cache_load_failure.cpp
 * @brief ProductCache::loadProducts() failure path: the cache is reset without deadlocking.
 *
 * loadProducts() holds the non-recursive @c load_mutex_ for the whole load. Its catch
 * block resets the cache, and that reset used to be the public clearCache(), which locks
 * the same mutex: any exception thrown while loading deadlocked the calling thread.
 *
 * The test provokes a load exception through the logger the load body writes to, then
 * runs the call on a worker thread and waits with a timeout. A regression therefore fails
 * the test (and leaves the worker blocked) instead of hanging the whole suite.
 *
 * The exception source: loadProducts() fetches the "product_cache" logger inside its try
 * block, and a spdlog sink that throws once, with an error handler that rethrows, turns
 * the first log call into an exception that reaches the catch block.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductCache.hpp"
#include "products/ProductParser.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// A sink that throws on its first message and then behaves as a null sink.
class ThrowOnceSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::atomic<int> thrown{0};

protected:
    void sink_it_(const spdlog::details::log_msg&) override {
        if (thrown.fetch_add(1) == 0)
            throw std::runtime_error("injected product-load failure");
    }
    void flush_() override {}
};

/// Everything the worker thread touches, kept alive by shared ownership so a timed-out
/// (deadlocked) worker never reads freed stack.
struct LoadFixture {
    LogManager log;
    GnssConfig config;
    std::unique_ptr<ProductParser> parser;
    std::shared_ptr<ThrowOnceSink> sink = std::make_shared<ThrowOnceSink>();
};

std::shared_ptr<LoadFixture> makeFixture(const std::string& tag) {
    auto fx = std::make_shared<LoadFixture>();
    const fs::path dir = fs::temp_directory_path() / ("product_cache_load_failure_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // Another test in the binary may have registered these names; initialize() throws on a
    // duplicate. Only the names this fixture creates are dropped (never drop_all()).
    spdlog::drop("gnss_processor");
    spdlog::drop("product_cache");
    spdlog::drop("product_parser");
    fx->log.initialize(dir, LogLevel::Info);
    fx->parser = std::make_unique<ProductParser>(fx->config, fx->log);

    auto logger = fx->log.getLogger("product_cache");
    logger->sinks().push_back(fx->sink);
    logger->set_error_handler([](const std::string& msg) { throw std::runtime_error(msg); });
    return fx;
}

}  // namespace

TEST(ProductCacheLoadFailure, FailedLoadResetsCacheWithoutDeadlock) {
    ProductCache& cache = ProductCache::getInstance();
    cache.clearCache();   // the cache is a process-wide singleton: start from empty

    auto fx = makeFixture("a");

    auto done = std::make_shared<std::promise<bool>>();
    std::future<bool> result = done->get_future();
    std::thread worker([fx, done] {
        done->set_value(ProductCache::getInstance().loadProducts(*fx->parser, fx->log));
    });

    const bool finished = result.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
    if (!finished) {
        worker.detach();   // deadlocked: leave the worker blocked rather than hang the suite
        // The blocked worker holds the cache's mutex, which can abort the process at exit
        // and drop buffered gtest output, so name the failure on the unbuffered stream too.
        std::fputs("[  FAILED  ] ProductCacheLoadFailure: loadProducts() deadlocked after a "
                   "load exception\n", stderr);
        FAIL() << "loadProducts() did not return after a load exception: the failure path "
                  "relocked the non-recursive load mutex";
    }
    worker.join();

    EXPECT_FALSE(result.get());                    // the failure is reported to the caller
    EXPECT_GE(fx->sink->thrown.load(), 1);         // the injected exception really fired
    EXPECT_FALSE(cache.isLoaded());
    EXPECT_EQ(cache.getOrbitCount(), 0u);

    // The mutex is free again: a second load (the sink no longer throws) succeeds.
    auto retry = std::async(std::launch::async, [&] { return cache.loadProducts(*fx->parser, fx->log); });
    ASSERT_EQ(retry.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    EXPECT_TRUE(retry.get());
    EXPECT_TRUE(cache.isLoaded());

    cache.clearCache();                            // and the public reset still works
    EXPECT_FALSE(cache.isLoaded());

    spdlog::drop("gnss_processor");
    spdlog::drop("product_cache");
    spdlog::drop("product_parser");
}

}  // namespace GnssProcessor
