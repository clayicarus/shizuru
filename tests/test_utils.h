#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace shizuru::test {

inline void InitTestLogger() {
  if (spdlog::get("test")) return;

  auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("test", sink);
  logger->set_level(spdlog::level::trace);
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [test] %v");

  spdlog::register_logger(logger);
  spdlog::set_default_logger(logger);
}

inline std::shared_ptr<spdlog::logger> GetTestLogger() {
  auto logger = spdlog::get("test");
  if (!logger) {
    InitTestLogger();
    logger = spdlog::get("test");
  }
  return logger;
}

#define LOG(...) SPDLOG_LOGGER_INFO(::shizuru::test::GetTestLogger(), __VA_ARGS__)

}  // namespace shizuru::test
