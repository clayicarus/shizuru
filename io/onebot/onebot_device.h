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
// Port contract:
//   Input  "text_in"  — text/plain payload; metadata must contain
//                        "message_type" ("group"|"private") and
//                        "target_id" (group_id or user_id as string).
//   Output "text_out" — text/plain payload with metadata:
//                        "actor_id"      = QQ user ID string
//                        "actor_name"    = nickname or card name
//                        "message_type"  = "group" | "private"
//                        "group_id"      = group ID (for group messages)
//                        "message_id"    = OneBot message ID

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "io/io_device.h"
#include "io/onebot/onebot_types.h"

// Forward-declare IXWebSocket types to avoid leaking the header.
namespace ix {
class WebSocketServer;
class WebSocket;
}  // namespace ix

namespace shizuru::io::onebot {

class OneBotDevice : public IoDevice {
 public:
  explicit OneBotDevice(OneBotConfig config,
                        std::string device_id = "onebot");
  ~OneBotDevice() override;

  OneBotDevice(const OneBotDevice&) = delete;
  OneBotDevice& operator=(const OneBotDevice&) = delete;

  // IoDevice interface.
  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // Port names.
  static constexpr char kTextIn[]  = "text_in";
  static constexpr char kTextOut[] = "text_out";

  // Get the reply context for the most recent incoming message.
  struct ReplyContext {
    std::string message_type;  // "group" or "private"
    std::string target_id;     // group_id or user_id
    std::string user_id;       // sender's user_id
  };
  ReplyContext GetLastReplyContext() const;

  // Send a merged forward message to a group (合并转发).
  // Used for long messages that would flood the chat as plain text.
  void SendGroupForward(int64_t group_id, const std::string& nickname,
                        const std::string& content);

 private:
  // Event handling.
  void HandleEvent(const std::string& json_str);
  void HandleMessageEvent(const nlohmann::json& event);
  void HandleMetaEvent(const nlohmann::json& event);

  // Send a JSON payload to the connected OneBot client.
  void WsSend(const std::string& json_str);

  // Send a message via OneBot API (send_msg action).
  void SendOneBotMessage(const std::string& message_type,
                         int64_t target_id,
                         const std::string& message);

  // Emit a DataFrame on the output port.
  void EmitText(const std::string& text,
                std::unordered_map<std::string, std::string> metadata);

  // Whitelist checks.
  bool IsGroupAllowed(int64_t group_id) const;
  bool IsUserAllowed(int64_t user_id) const;

  // Extract plain text from OneBot message array or CQ-coded string.
  // Also extracts @mentions.
  struct ParsedMessage {
    std::string text;
    std::vector<std::string> mentions;  // QQ IDs that were @'d
  };
  static ParsedMessage ParseMessage(const nlohmann::json& message,
                                    const std::string& self_id);

  OneBotConfig config_;
  std::string device_id_;

  // WebSocket server — we listen, OneBot connects to us.
  std::unique_ptr<ix::WebSocketServer> server_;
  std::atomic<bool> active_{false};

  // The single connected OneBot client (Universal or API+Event).
  // Protected by client_mutex_.
  std::mutex client_mutex_;
  std::shared_ptr<ix::WebSocket> client_;

  std::string self_id_;  // Learned from X-Self-ID header or lifecycle event.

  // Whitelist sets for O(1) lookup.
  std::unordered_set<int64_t> group_whitelist_;
  std::unordered_set<int64_t> user_whitelist_;

  std::mutex output_cb_mutex_;
  OutputCallback output_cb_;

  // API call echo counter.
  std::atomic<int64_t> echo_counter_{0};

  // Last incoming message context for reply routing.
  mutable std::mutex reply_ctx_mutex_;
  ReplyContext last_reply_ctx_;
};

}  // namespace shizuru::io::onebot
