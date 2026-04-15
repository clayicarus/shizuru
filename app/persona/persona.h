#pragma once

// app/persona/persona.h — Shizuru persona and system prompt assembly.
//
// Responsible for building the system prompt that defines Shizuru's
// personality, behavior boundaries, and dynamic context injection.
//
// The system prompt is assembled from multiple segments:
//   1. Fixed persona (personality, tone, boundaries)
//   2. User preferences summary (learned from conversation)
//   3. Active followups list (things to check back on)
//   4. Recent emotional context (if relevant)
//
// ContextStrategy only has a single system_instruction string.
// This module builds that string by concatenating the segments.

#include <string>
#include <vector>

#include "app/memory/memory_types.h"

namespace shizuru::app {

// Build the complete system prompt for a session.
// Combines the fixed persona with dynamic user context.
std::string BuildSystemPrompt(
    const std::vector<UserPreference>& preferences,
    const std::vector<FollowUp>& active_followups);

// The fixed persona prompt — Shizuru's core personality definition.
// Extracted as a separate function so it can be tested independently.
std::string DefaultPersonaPrompt();

}  // namespace shizuru::app
