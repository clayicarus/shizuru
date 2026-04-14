#pragma once

// app/assembly/app_config.h — Product-level configuration.
//
// Everything the product needs to start: LLM endpoint, user identity,
// memory backend, persona selection, etc.
//
// This replaces the old RuntimeConfig which mixed bus config with business config.

#include <string>

#include "async_logger.h"
#include "core/controller/config.h"
#include "core/context/config.h"
#include "core/policy/config.h"
#include "services/llm/config.h"

namespace shizuru::app {

struct AppConfig {
  // ── User identity ──────────────────────────────────────────────────────
  std::string user_id;  // Persistent user identifier across sessions/devices.

  // ── LLM ────────────────────────────────────────────────────────────────
  services::OpenAiConfig llm;

  // ── Core configs ───────────────────────────────────────────────────────
  core::ControllerConfig controller;
  core::ContextConfig context;
  core::PolicyConfig policy;
  core::LoggerConfig logger;

  // ── Memory ─────────────────────────────────────────────────────────────
  std::string db_path;  // SQLite database path.  Empty = in-memory fallback.

  // ── Persona ────────────────────────────────────────────────────────────
  // If empty, the default Shizuru persona is used.
  std::string persona_id;
};

}  // namespace shizuru::app
