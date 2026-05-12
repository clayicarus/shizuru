#pragma once

namespace shizuru::runtime {

// Payload categories routed by the runtime bus.
enum class PortPayloadKind {
  kAudioFrame,
  kConversationItem,
  kControlSignal,
};

}  // namespace shizuru::runtime
