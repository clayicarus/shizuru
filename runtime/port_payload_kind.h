#pragma once

namespace shizuru::runtime {

// Payload categories routed by the runtime bus.
//
// kLegacyFrame exists only as a transitional compatibility kind while the
// system migrates away from the old DataFrame-only bus contract.
enum class PortPayloadKind {
  kLegacyFrame,
  kAudioFrame,
  kConversationItem,
  kControlSignal,
};

}  // namespace shizuru::runtime
