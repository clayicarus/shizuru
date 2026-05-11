#pragma once

// io/onebot/onebot_device.h — OneBot 11 reverse WebSocket IO device.
//
// Implements the **server** side of the OneBot 11 reverse WebSocket protocol.
// The OneBot implementation (go-cqhttp, Lagrange, NapCat, etc.) connects to
// us as a WebSocket client.  We listen on a local port and accept the
// Universal client connection (events + API on one socket).
//
// Protocol reference:
//   https://github.com/botuniverse/onebot-11/blob/master/communication/ws-reverse.md
//
// On connect, the OneBot client sends:
//   X-Self-ID: <bot QQ number>
//   X-Client-Role: Universal | API | Event
//   Authorization: Bearer <token>   (if configured)
//
// Output contract:
//   Parsed OneBot messages are delivered as core::ConversationItem via the
//   on_item_ callback.  Each message produces exactly one ConversationItem
//   with kind = kUserMessage.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/content_part.h"
#include "core/conversation_item.h"
#include "io/onebot/onebot_types.h"

// Forward-declare IXWebSocket types to avoid leaking the header.
namespace ix {
class WebSocketServer;
class WebSocket;
}  // namespace ix

namespace shizuru::io::onebot {

// Callback type for delivering parsed ConversationItems to Core.
using OnItemCallback = std::function<void(core::ConversationItem)>;

class OneBotDevice {
 public:
  explicit OneBotDevice(OneBotConfig config,
                        std::string device_id = "onebot");
  ~OneBotDevice();

  OneBotDevice(const OneBotDevice&) = delete;
  OneBotDevice& operator=(const OneBotDevice&) = delete;

  // Device identity.
  std::string GetDeviceId() const;

  // Lifecycle.
  void Start();
  void Stop();

  // Register the callback that delivers parsed ConversationItems to Core.
  void SetOnItemCallback(OnItemCallback cb);

  // Send a text message to a target (group or private).
  // This is the outbound path — used by Core to reply.
  void SendMessage(const std::string& message_type,
                   int64_t target_id,
                   const std::string& message);

  // Send a merged forward message to a group (合并转发).
  void SendGroupForward(int64_t group_id, const std::string& nickname,
                        const std::string& content);

  // Get the reply context for the most recent incoming message.
  struct ReplyContext {
    std::string message_type;  // "group" or "private"
    std::string target_id;     // group_id or user_id
    std::string user_id;       // sender's user_id
  };
  ReplyContext GetLastReplyContext() const;

 private:
  // Event handling.
  void HandleEvent(const std::string& json_str);
  void HandleMessageEvent(const nlohmann::json& event);
  void HandleMetaEvent(const nlohmann::json& event);

  // Send a JSON payload to the connected OneBot client.
  void WsSend(const std::string& json_str);

  // Whitelist checks.
  bool IsGroupAllowed(int64_t group_id) const;
  bool IsUserAllowed(int64_t user_id) const;

  // Parse OneBot message segments into ContentParts + mentions + reply info.
  struct ParsedMessage {
    core::ContentParts parts;             // TextPart and ImagePart entries
    std::vector<std::string> mentions;    // QQ IDs that were @'d
    std::optional<std::string> reply_to;  // message_id of replied message
  };
  static ParsedMessage ParseMessage(const nlohmann::json& message,
                                    const std::string& self_id);

  OneBotConfig config_;
  std::string device_id_;

  // WebSocket server — we listen, OneBot connects to us.
  std::unique_ptr<ix::WebSocketServer> server_;
  std::atomic<bool> active_{false};

  // The single connected OneBot client (Universal or API+Event).
  std::mutex client_mutex_;
  std::shared_ptr<ix::WebSocket> client_;

  std::string self_id_;  // Learned from X-Self-ID header or lifecycle event.

  // Whitelist sets for O(1) lookup.
  std::unordered_set<int64_t> group_whitelist_;
  std::unordered_set<int64_t> user_whitelist_;

  // Callback for delivering parsed items.
  std::mutex on_item_mutex_;
  OnItemCallback on_item_;

  // API call echo counter.
  std::atomic<int64_t> echo_counter_{0};

  // Last incoming message context for reply routing.
  mutable std::mutex reply_ctx_mutex_;
  ReplyContext last_reply_ctx_;
};

}  // namespace shizuru::io::onebot
