#pragma once

// io/onebot/onebot_types.h — Configuration and types for OneBot 11 device.

#include <cstdint>
#include <string>
#include <vector>

namespace shizuru::io::onebot {

// Configuration for the OneBot 11 reverse WebSocket server.
//
// In the OneBot 11 reverse WS model, the OneBot implementation (go-cqhttp,
// Lagrange, NapCat, etc.) is the WebSocket **client** that connects to us.
// We are the **server** listening on a local port.
struct OneBotConfig {
  // Host to bind the WebSocket server to.
  std::string host = "0.0.0.0";

  // Port to listen on.  The OneBot implementation connects to ws://<host>:<port>.
  int port = 8080;

  // Access token for authentication.  If non-empty, the server validates the
  // Authorization header sent by the OneBot client on connect.
  // Empty = no auth.
  std::string access_token;

  // Self QQ number — used to identify messages from the bot itself.
  // If empty, the device will learn it from the X-Self-ID header or the
  // first lifecycle event.
  std::string self_id;

  // If non-empty, only process messages from these group IDs.
  // Empty = process all groups.
  std::vector<int64_t> group_whitelist;

  // If non-empty, only process private messages from these user IDs.
  // Empty = process all private messages.
  std::vector<int64_t> user_whitelist;

  // Whether to include group messages (default: true).
  bool enable_group = true;

  // Whether to include private messages (default: true).
  bool enable_private = true;
};

}  // namespace shizuru::io::onebot
