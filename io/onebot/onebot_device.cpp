// io/onebot/onebot_device.cpp — OneBot 11 reverse WebSocket IO device.
//
// We are the WebSocket SERVER.  The OneBot implementation connects to us.
// Incoming messages are parsed into core::ConversationItem and delivered
// via the on_item_ callback.

#include "io/onebot/onebot_device.h"

#include <chrono>
#include <utility>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include "async_logger.h"
#include "io/onebot/onebot_tags.h"
#include "io/onebot/qq_face_table.h"

namespace shizuru::io::onebot {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

OneBotDevice::OneBotDevice(OneBotConfig config, std::string device_id)
    : config_(std::move(config)), device_id_(std::move(device_id)) {
  for (auto id : config_.group_whitelist) {
    group_whitelist_.insert(id);
  }
  for (auto id : config_.user_whitelist) {
    user_whitelist_.insert(id);
  }
  if (!config_.self_id.empty()) {
    self_id_ = config_.self_id;
  }
}

OneBotDevice::~OneBotDevice() { Stop(); }

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

std::string OneBotDevice::GetDeviceId() const { return device_id_; }

void OneBotDevice::SetOnItemCallback(OnItemCallback cb) {
  std::lock_guard<std::mutex> lock(on_item_mutex_);
  on_item_ = std::move(cb);
}

void OneBotDevice::Start() {
  if (active_.exchange(true)) { return; }

  server_ = std::make_unique<ix::WebSocketServer>(
      config_.port, config_.host);

  server_->setOnConnectionCallback(
      [this](std::weak_ptr<ix::WebSocket> weak_ws,
             std::shared_ptr<ix::ConnectionState> /*state*/) {
        auto ws = weak_ws.lock();
        if (!ws) { return; }

        // Store the client for sending API calls later.
        {
          std::lock_guard<std::mutex> lock(client_mutex_);
          client_ = ws;
        }

        // Register per-connection message callback.
        ws->setOnMessageCallback(
            [this, weak_ws](const ix::WebSocketMessagePtr& msg) {
              if (!active_.load()) { return; }

              switch (msg->type) {
                case ix::WebSocketMessageType::Open: {
                  // ── Validate Authorization header ────────────────────
                  if (!config_.access_token.empty()) {
                    const auto& headers = msg->openInfo.headers;
                    std::string auth;
                    auto it = headers.find("Authorization");
                    if (it == headers.end()) {
                      it = headers.find("authorization");
                    }
                    if (it != headers.end()) {
                      auth = it->second;
                    }
                    const std::string expected =
                        "Bearer " + config_.access_token;
                    if (auth != expected) {
                      LOG_WARN("[{}] Rejected connection: invalid token",
                               device_id_);
                      if (auto ws2 = weak_ws.lock()) {
                        ws2->close(4001, "Unauthorized");
                      }
                      return;
                    }
                  }

                  // ── Extract X-Self-ID and X-Client-Role ─────────────
                  const auto& headers = msg->openInfo.headers;
                  std::string client_role;
                  {
                    auto it = headers.find("X-Client-Role");
                    if (it == headers.end()) {
                      it = headers.find("x-client-role");
                    }
                    if (it != headers.end()) {
                      client_role = it->second;
                    }
                  }
                  {
                    auto it = headers.find("X-Self-ID");
                    if (it == headers.end()) {
                      it = headers.find("x-self-id");
                    }
                    if (it != headers.end() && self_id_.empty()) {
                      self_id_ = it->second;
                      LOG_INFO("[{}] Learned self_id from header: {}",
                               device_id_, self_id_);
                    }
                  }

                  LOG_INFO(
                      "[{}] OneBot client connected (role={}, self_id={}, "
                      "uri={})",
                      device_id_, client_role, self_id_, msg->openInfo.uri);
                  break;
                }

                case ix::WebSocketMessageType::Close: {
                  LOG_INFO("[{}] OneBot client disconnected: {} {}",
                           device_id_, msg->closeInfo.code,
                           msg->closeInfo.reason);
                  {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    client_.reset();
                  }
                  break;
                }

                case ix::WebSocketMessageType::Message: {
                  HandleEvent(msg->str);
                  break;
                }

                case ix::WebSocketMessageType::Error: {
                  LOG_ERROR("[{}] WebSocket error: {}", device_id_,
                            msg->errorInfo.reason);
                  break;
                }

                default:
                  break;
              }
            });
      });

  auto res = server_->listen();
  if (!res.first) {
    LOG_ERROR("[{}] Failed to listen on {}:{} — {}",
              device_id_, config_.host, config_.port, res.second);
    active_.store(false);
    return;
  }

  server_->start();
  LOG_INFO("[{}] Reverse WS server listening on {}:{}",
           device_id_, config_.host, config_.port);
}

void OneBotDevice::Stop() {
  if (!active_.exchange(false)) { return; }

  if (server_) {
    server_->stop();
    server_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client_.reset();
  }
  LOG_INFO("[{}] OneBotDevice stopped", device_id_);
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

void OneBotDevice::HandleEvent(const std::string& json_str) {
  LOG_DEBUG("[onebot] Raw WS payload: {}", json_str);

  json event;
  try {
    event = json::parse(json_str);
  } catch (const json::parse_error& e) {
    LOG_WARN("[{}] Failed to parse OneBot event: {}", device_id_, e.what());
    return;
  }

  const std::string post_type = event.value("post_type", "");

  if (post_type == "message") {
    HandleMessageEvent(event);
  } else if (post_type == "meta_event") {
    HandleMetaEvent(event);
  }
  // notice, request — ignored for now.
}

void OneBotDevice::HandleMessageEvent(const json& event) {
  const std::string message_type = event.value("message_type", "");

  if (message_type == "group" && !config_.enable_group) { return; }
  if (message_type == "private" && !config_.enable_private) { return; }

  const auto& sender = event.value("sender", json::object());
  const int64_t user_id = event.value("user_id", int64_t{0});
  const std::string user_id_str = std::to_string(user_id);

  // Skip messages from self.
  if (!self_id_.empty() && user_id_str == self_id_) { return; }

  // Whitelist filtering.
  if (message_type == "group") {
    const int64_t group_id = event.value("group_id", int64_t{0});
    if (!IsGroupAllowed(group_id)) { return; }
  } else if (message_type == "private") {
    if (!IsUserAllowed(user_id)) { return; }
  }

  // Parse message segments into ContentParts + mentions.
  ParsedMessage parsed;
  if (event.contains("message")) {
    parsed = ParseMessage(event["message"], self_id_);
  } else {
    // Fallback: use raw_message as plain text.
    std::string raw = event.value("raw_message", "");
    if (!raw.empty()) {
      parsed.parts.emplace_back(core::TextPart{std::move(raw)});
    }
  }

  // Skip empty messages (no parts at all).
  if (parsed.parts.empty()) {
    LOG_DEBUG("[onebot] Skipping empty message (no parts)");
    return;
  }

  // Build actor.
  std::string display_name = sender.value("card", "");
  if (display_name.empty()) {
    display_name = sender.value("nickname", "");
  }
  if (display_name.empty()) {
    display_name = user_id_str;
  }

  core::ActorRef actor;
  actor.actor_id = user_id_str;
  actor.display_name = std::move(display_name);
  actor.kind = core::ActorKind::kHuman;

  // Build conversation_id from group_id or user_id.
  std::string conversation_id;
  if (message_type == "group") {
    conversation_id = "group:" + std::to_string(event.value("group_id", int64_t{0}));
  } else {
    conversation_id = "private:" + user_id_str;
  }

  // Build item_id from message_id.
  const int64_t message_id = event.value("message_id", int64_t{0});
  std::string item_id = "onebot:" + std::to_string(message_id);

  // Build wall_time from event timestamp.
  std::chrono::system_clock::time_point wall_time;
  if (event.contains("time")) {
    auto unix_seconds = event.value("time", int64_t{0});
    wall_time = std::chrono::system_clock::from_time_t(
        static_cast<std::time_t>(unix_seconds));
  } else {
    wall_time = std::chrono::system_clock::now();
  }

  // Construct the ConversationItem.
  core::ConversationItem item;
  item.item_id = std::move(item_id);
  item.conversation_id = std::move(conversation_id);
  item.kind = core::ConversationItemKind::kUserMessage;
  item.actor = std::move(actor);
  item.parts = std::move(parsed.parts);
  item.wall_time = wall_time;
  item.reply_to_item_id = std::move(parsed.reply_to);
  item.mentions = std::move(parsed.mentions);

  LOG_INFO("[{}] {} message from {} ({}): item_id={}", device_id_,
           message_type, item.actor.display_name, item.actor.actor_id,
           item.item_id);

  // Update reply context.
  {
    std::lock_guard<std::mutex> lock(reply_ctx_mutex_);
    last_reply_ctx_.message_type = message_type;
    last_reply_ctx_.user_id = user_id_str;
    if (message_type == "group") {
      last_reply_ctx_.target_id =
          std::to_string(event.value("group_id", int64_t{0}));
    } else {
      last_reply_ctx_.target_id = user_id_str;
    }
  }

  // Deliver to Core via callback.
  OnItemCallback cb;
  {
    std::lock_guard<std::mutex> lock(on_item_mutex_);
    cb = on_item_;
  }
  if (cb) {
    cb(std::move(item));
  }
}

void OneBotDevice::HandleMetaEvent(const json& event) {
  const std::string meta_event_type = event.value("meta_event_type", "");

  if (meta_event_type == "lifecycle") {
    if (event.contains("self_id") && self_id_.empty()) {
      self_id_ = std::to_string(event.value("self_id", int64_t{0}));
      LOG_INFO("[{}] Learned self_id from lifecycle: {}", device_id_, self_id_);
    }
  } else if (meta_event_type == "heartbeat") {
    LOG_DEBUG("[{}] Heartbeat received", device_id_);
  }
}

// ---------------------------------------------------------------------------
// Outbound: send messages via OneBot API
// ---------------------------------------------------------------------------

void OneBotDevice::WsSend(const std::string& json_str) {
  std::shared_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    ws = client_;
  }
  if (!ws) {
    LOG_WARN("[{}] No OneBot client connected, cannot send", device_id_);
    return;
  }
  ws->send(json_str);
}

void OneBotDevice::SendMessage(const std::string& message_type,
                               int64_t target_id,
                               const std::string& message) {
  if (!active_.load()) { return; }

  // Outbound group whitelist check.
  if (message_type == "group" && !IsGroupAllowed(target_id)) {
    LOG_WARN("[{}] Blocked outbound message to non-whitelisted group {}",
             device_id_, target_id);
    return;
  }

  json payload;
  payload["action"] = "send_msg";

  json params;
  params["message_type"] = message_type;
  params["message"] = message;

  if (message_type == "group") {
    params["group_id"] = target_id;
  } else {
    params["user_id"] = target_id;
  }

  payload["params"] = std::move(params);
  payload["echo"] = std::to_string(echo_counter_.fetch_add(1));

  const std::string json_str = payload.dump();
  LOG_DEBUG("[{}] Sending API call: {}", device_id_,
            json_str.size() > 200 ? json_str.substr(0, 200) + "..." : json_str);
  WsSend(json_str);
}

void OneBotDevice::SendGroupForward(int64_t group_id,
                                    const std::string& nickname,
                                    const std::string& content) {
  json node;
  node["type"] = "node";
  node["data"] = {
      {"user_id", self_id_.empty() ? "0" : self_id_},
      {"nickname", nickname},
      {"content", content},
  };

  json payload;
  payload["action"] = "send_group_forward_msg";
  payload["params"] = {
      {"group_id", group_id},
      {"messages", json::array({node})},
  };
  payload["echo"] = std::to_string(echo_counter_.fetch_add(1));

  const std::string json_str = payload.dump();
  LOG_DEBUG("[{}] Sending forward message: {}", device_id_,
            json_str.size() > 200 ? json_str.substr(0, 200) + "..." : json_str);
  WsSend(json_str);
}

// ---------------------------------------------------------------------------
// Reply context
// ---------------------------------------------------------------------------

OneBotDevice::ReplyContext OneBotDevice::GetLastReplyContext() const {
  std::lock_guard<std::mutex> lock(reply_ctx_mutex_);
  return last_reply_ctx_;
}

// ---------------------------------------------------------------------------
// Whitelist helpers
// ---------------------------------------------------------------------------

bool OneBotDevice::IsGroupAllowed(int64_t group_id) const {
  if (group_whitelist_.empty()) { return true; }
  return group_whitelist_.count(group_id) > 0;
}

bool OneBotDevice::IsUserAllowed(int64_t user_id) const {
  if (user_whitelist_.empty()) { return true; }
  return user_whitelist_.count(user_id) > 0;
}

// ---------------------------------------------------------------------------
// Message parsing — produces ContentParts + mentions + reply_to
// ---------------------------------------------------------------------------

OneBotDevice::ParsedMessage OneBotDevice::ParseMessage(
    const json& message, const std::string& self_id) {
  ParsedMessage result;

  if (message.is_string()) {
    // CQ-coded string.  Extract @mentions, images, and text content.
    std::string raw = message.get<std::string>();
    std::string text;
    text.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
      if (raw[i] == '[' && i + 3 < raw.size() && raw[i + 1] == 'C' &&
          raw[i + 2] == 'Q' && raw[i + 3] == ':') {
        auto end = raw.find(']', i);
        if (end != std::string::npos) {
          std::string cq = raw.substr(i + 4, end - i - 4);

          if (cq.substr(0, 3) == "at,") {
            auto qq_pos = cq.find("qq=");
            if (qq_pos != std::string::npos) {
              auto qq_end = cq.find(',', qq_pos);
              std::string qq = cq.substr(qq_pos + 3,
                  qq_end == std::string::npos ? std::string::npos
                                              : qq_end - qq_pos - 3);
              result.mentions.push_back(qq);
              using namespace onebot::tags;
              text += SelfClosingTag(kAt, kAttrId,
                                     qq == self_id ? kAtSelf : qq);
            }
          } else if (cq.substr(0, 6) == "image,") {
            // Extract URL from CQ image: [CQ:image,...,url=https://...]
            auto url_pos = cq.find("url=");
            if (url_pos != std::string::npos) {
              std::string url = cq.substr(url_pos + 4);
              // Decode &amp; entities.
              std::string decoded;
              decoded.reserve(url.size());
              for (size_t j = 0; j < url.size(); ++j) {
                if (url.compare(j, 5, "&amp;") == 0) {
                  decoded += '&'; j += 4;
                } else {
                  decoded += url[j];
                }
              }
              if (!decoded.empty()) {
                result.parts.emplace_back(core::ImagePart{std::move(decoded)});
              }
            }
          } else if (cq.substr(0, 6) == "reply,") {
            // Extract reply reference: [CQ:reply,id=12345]
            auto id_pos = cq.find("id=");
            if (id_pos != std::string::npos) {
              auto id_end = cq.find(',', id_pos);
              std::string reply_id = cq.substr(id_pos + 3,
                  id_end == std::string::npos ? std::string::npos
                                              : id_end - id_pos - 3);
              if (!reply_id.empty()) {
                result.reply_to = "onebot:" + reply_id;
              }
            }
          } else if (cq.substr(0, 5) == "face,") {
            auto id_pos = cq.find("id=");
            if (id_pos != std::string::npos) {
              auto id_end = cq.find(',', id_pos);
              std::string id_str = cq.substr(id_pos + 3,
                  id_end == std::string::npos ? std::string::npos
                                              : id_end - id_pos - 3);
              try {
                int face_id = std::stoi(id_str);
                const char* desc = QqFaceDesc(face_id);
                using namespace onebot::tags;
                text += SelfClosingTag(kFace, kAttrDesc,
                                       desc != nullptr ? desc : "表情");
              } catch (...) {
                using namespace onebot::tags;
                text += SelfClosingTag(kFace, kAttrDesc, "表情");
              }
            }
          }
          i = end + 1;
          continue;
        }
      }
      text += raw[i];
      ++i;
    }

    // Trim and add as TextPart if non-empty.
    auto start = text.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
      auto end_pos = text.find_last_not_of(" \t\n\r");
      std::string trimmed = text.substr(start, end_pos - start + 1);
      if (!trimmed.empty()) {
        result.parts.emplace_back(core::TextPart{std::move(trimmed)});
      }
    }
    return result;
  }

  if (message.is_array()) {
    std::string text;
    for (const auto& seg : message) {
      if (!seg.is_object()) { continue; }
      const std::string type = seg.value("type", "");

      if (type == "text") {
        const auto& data = seg.value("data", json::object());
        const std::string t = data.value("text", "");
        if (!t.empty()) {
          text += t;
        }
      } else if (type == "at") {
        const auto& data = seg.value("data", json::object());
        const std::string qq = data.value("qq", "");
        if (!qq.empty()) {
          result.mentions.push_back(qq);
          using namespace onebot::tags;
          text += SelfClosingTag(kAt, kAttrId,
                                 qq == self_id ? kAtSelf : qq);
        }
      } else if (type == "face") {
        const auto& data = seg.value("data", json::object());
        const std::string id_str = data.value("id", "");
        if (!id_str.empty()) {
          using namespace onebot::tags;
          try {
            int face_id = std::stoi(id_str);
            const char* desc = QqFaceDesc(face_id);
            text += SelfClosingTag(kFace, kAttrDesc,
                                   desc != nullptr ? desc : "表情");
          } catch (...) {
            text += SelfClosingTag(kFace, kAttrDesc, "表情");
          }
        }
      } else if (type == "image") {
        const auto& data = seg.value("data", json::object());
        std::string url = data.value("url", "");
        if (!url.empty()) {
          // Flush accumulated text before the image.
          auto start = text.find_first_not_of(" \t\n\r");
          if (start != std::string::npos) {
            auto end_pos = text.find_last_not_of(" \t\n\r");
            std::string trimmed = text.substr(start, end_pos - start + 1);
            if (!trimmed.empty()) {
              result.parts.emplace_back(core::TextPart{std::move(trimmed)});
            }
            text.clear();
          }
          result.parts.emplace_back(core::ImagePart{std::move(url)});
        }
      } else if (type == "reply") {
        // Reply reference segment.
        const auto& data = seg.value("data", json::object());
        const std::string reply_id = data.value("id", "");
        if (!reply_id.empty()) {
          result.reply_to = "onebot:" + reply_id;
        }
      } else if (!type.empty()) {
        // Unknown segment — emit stub tag so agent is aware.
        using namespace onebot::tags;
        text += SelfClosingTag(kMedia, kAttrType, type);
        LOG_DEBUG("[onebot] Unhandled segment type: {} data: {}",
                  type, seg.value("data", json::object()).dump());
      }
    }

    // Flush remaining text.
    auto start = text.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
      auto end_pos = text.find_last_not_of(" \t\n\r");
      std::string trimmed = text.substr(start, end_pos - start + 1);
      if (!trimmed.empty()) {
        result.parts.emplace_back(core::TextPart{std::move(trimmed)});
      }
    }
    return result;
  }

  return result;
}

}  // namespace shizuru::io::onebot
