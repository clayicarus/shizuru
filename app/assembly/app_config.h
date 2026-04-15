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
  std::string persona_id;

  // ── User custom instruction ────────────────────────────────────────────
  // Appended after the persona prompt.  Allows the user to add custom
  // directives (e.g., "always reply in English") without overriding the
  // persona.  Empty = no custom instruction.
  std::string user_instruction;
};

}  // namespace shizuru::app
