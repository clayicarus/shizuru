#include "async_logger.h"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#endif

namespace shizuru::core {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
}  // namespace

void InitLogger(const LoggerConfig& config) {
  // Idempotent: skip if already initialized.
  if (g_logger) return;

  // Create the async thread pool (shared by all async loggers).
  spdlog::init_thread_pool(config.async_queue_size,
                           config.async_thread_count);

  // Build sinks.
  std::vector<spdlog::sink_ptr> sinks;

  if (config.enable_console) {
#ifdef __ANDROID__
    sinks.push_back(
        std::make_shared<spdlog::sinks::android_sink_mt>("shizuru"));
#else
    sinks.push_back(
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
  }

  if (!config.log_file.empty()) {
    sinks.push_back(
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.log_file, config.max_file_size, config.max_files));
  }

  // Create async logger with overflow policy: block when queue is full
  // (safer default — no log loss).
  g_logger = std::make_shared<spdlog::async_logger>(
      "shizuru",
      sinks.begin(), sinks.end(),
      spdlog::thread_pool(),
      spdlog::async_overflow_policy::block);

  g_logger->set_level(config.level);
  g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [tid:%t] %v");

  // Register as default so spdlog::get("shizuru") also works.
  spdlog::register_logger(g_logger);
  spdlog::set_default_logger(g_logger);
}

void ShutdownLogger() {
  if (g_logger) {
    g_logger->flush();
  }
  spdlog::shutdown();
  g_logger.reset();
}

std::shared_ptr<spdlog::logger> GetLogger() {
  if (!g_logger) {
    // Fallback: sync stderr logger for debug/test scenarios without InitLogger().
    // Use get() first to avoid duplicate-registration exception.
    g_logger = spdlog::get("shizuru");
    if (!g_logger) {
      g_logger = spdlog::stderr_color_mt("shizuru");
    }
  }
  return g_logger;
}

}  // namespace shizuru::core
