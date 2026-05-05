// io/onebot/onebot_device.cpp — OneBot 11 reverse WebSocket IO device.
//
// We are the WebSocket SERVER.  The OneBot implementation connects to us.

#include "io/onebot/onebot_device.h"

#include <chrono>
#include <ctime>
#include <utility>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include "async_logger.h"

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
// IoDevice interface
// ---------------------------------------------------------------------------

std::string OneBotDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> OneBotDevice::GetPortDescriptors() const {
  return {
      {kTextIn,  PortDirection::kInput,  "text/plain"},
      {kTextOut, PortDirection::kOutput, "text/plain"},
  };
}

void OneBotDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void OneBotDevice::Start() {
  if (active_.exchange(true)) { return; }

  server_ = std::make_unique<ix::WebSocketServer>(
      config_.port, config_.host);

  // IXWebSocketServer requires setOnConnectionCallback with a per-connection
  // setOnMessageCallback inside it.  Do NOT use setOnClientMessageCallback
  // together with setOnConnectionCallback — they are mutually exclusive.
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

void OneBotDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (!active_.load()) { return; }
  if (port_name != kTextIn) { return; }

  const std::string text(frame.payload.begin(), frame.payload.end());
  if (text.empty()) { return; }

  // Extract target info from metadata.
  std::string message_type = "private";
  int64_t target_id = 0;

  auto it_type = frame.metadata.find("message_type");
  if (it_type != frame.metadata.end()) {
    message_type = it_type->second;
  }

  auto it_target = frame.metadata.find("target_id");
  if (it_target != frame.metadata.end()) {
    try {
      target_id = std::stoll(it_target->second);
    } catch (...) {
      LOG_WARN("[{}] Invalid target_id in metadata: {}", device_id_,
               it_target->second);
      return;
    }
  }

  if (message_type == "group" && target_id == 0) {
    auto it_group = frame.metadata.find("group_id");
    if (it_group != frame.metadata.end()) {
      try {
        target_id = std::stoll(it_group->second);
      } catch (...) {}
    }
  }

  if (target_id == 0) {
    LOG_WARN("[{}] No target_id in metadata, cannot send message", device_id_);
    return;
  }

  // Outbound group whitelist check.
  if (message_type == "group" && !IsGroupAllowed(target_id)) {
    LOG_WARN("[{}] Blocked outbound message to non-whitelisted group {}",
             device_id_, target_id);
    return;
  }

  SendOneBotMessage(message_type, target_id, text);
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

void OneBotDevice::HandleEvent(const std::string& json_str) {
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

  // Whitelist filtering:
  //   - Group messages: check group_whitelist only (all group members allowed).
  //   - Private messages: check user_whitelist only.
  // Empty whitelist = allow all for that category.
  if (message_type == "group") {
    const int64_t group_id = event.value("group_id", int64_t{0});
    if (!IsGroupAllowed(group_id)) { return; }
  } else if (message_type == "private") {
    if (!IsUserAllowed(user_id)) { return; }
  }

  // Extract text and mentions.
  ParsedMessage parsed;
  if (event.contains("message")) {
    parsed = ParseMessage(event["message"], self_id_);
  } else {
    parsed.text = event.value("raw_message", "");
  }
  if (parsed.text.empty()) { return; }

  // Build metadata.
  std::unordered_map<std::string, std::string> metadata;
  metadata["actor_id"] = user_id_str;
  metadata["message_type"] = message_type;
  metadata["message_id"] =
      std::to_string(event.value("message_id", int64_t{0}));

  // OneBot event timestamp (Unix seconds) → human-readable for LLM.
  if (event.contains("time")) {
    auto t = static_cast<time_t>(event.value("time", int64_t{0}));
    char buf[20];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    metadata["timestamp"] = buf;
  }

  // Pass mentions as comma-separated QQ IDs.
  if (!parsed.mentions.empty()) {
    std::string mentions_str;
    for (size_t i = 0; i < parsed.mentions.size(); ++i) {
      if (i > 0) { mentions_str += ","; }
      mentions_str += parsed.mentions[i];
    }
    metadata["mentions"] = mentions_str;
  }

  std::string display_name = sender.value("card", "");
  if (display_name.empty()) {
    display_name = sender.value("nickname", "");
  }
  if (display_name.empty()) {
    display_name = user_id_str;
  }
  metadata["actor_name"] = display_name;

  if (message_type == "group") {
    const int64_t group_id = event.value("group_id", int64_t{0});
    metadata["group_id"] = std::to_string(group_id);
    metadata["target_id"] = std::to_string(group_id);
  } else {
    metadata["target_id"] = user_id_str;
  }

  LOG_INFO("[{}] {} message from {} ({}): {}", device_id_, message_type,
           display_name, user_id_str,
           parsed.text.size() > 80 ? parsed.text.substr(0, 80) + "..." : parsed.text);

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

  EmitText(parsed.text, std::move(metadata));
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

void OneBotDevice::SendOneBotMessage(const std::string& message_type,
                                     int64_t target_id,
                                     const std::string& message) {
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

// ---------------------------------------------------------------------------
// Output emission
// ---------------------------------------------------------------------------

void OneBotDevice::EmitText(
    const std::string& text,
    std::unordered_map<std::string, std::string> metadata) {
  OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (!cb) { return; }

  DataFrame frame;
  frame.type = "text/plain";
  frame.payload.assign(text.begin(), text.end());
  frame.source_device = device_id_;
  frame.source_port = kTextOut;
  frame.timestamp = std::chrono::steady_clock::now();
  frame.metadata = std::move(metadata);

  cb(device_id_, kTextOut, std::move(frame));
}

// ---------------------------------------------------------------------------
// Reply context
// ---------------------------------------------------------------------------

OneBotDevice::ReplyContext OneBotDevice::GetLastReplyContext() const {
  std::lock_guard<std::mutex> lock(reply_ctx_mutex_);
  return last_reply_ctx_;
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
// Message parsing
// ---------------------------------------------------------------------------

OneBotDevice::ParsedMessage OneBotDevice::ParseMessage(
    const json& message, const std::string& self_id) {
  ParsedMessage result;

  if (message.is_string()) {
    // CQ-coded string.  Extract @mentions and strip CQ codes.
    std::string raw = message.get<std::string>();
    std::string text;
    text.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
      if (raw[i] == '[' && i + 3 < raw.size() && raw[i + 1] == 'C' &&
          raw[i + 2] == 'Q' && raw[i + 3] == ':') {
        auto end = raw.find(']', i);
        if (end != std::string::npos) {
          // Parse CQ code content: [CQ:at,qq=12345]
          std::string cq = raw.substr(i + 4, end - i - 4);
          if (cq.substr(0, 3) == "at,") {
            auto qq_pos = cq.find("qq=");
            if (qq_pos != std::string::npos) {
              auto qq_end = cq.find(',', qq_pos);
              std::string qq = cq.substr(qq_pos + 3,
                  qq_end == std::string::npos ? std::string::npos
                                              : qq_end - qq_pos - 3);
              result.mentions.push_back(qq);
              if (qq == self_id) {
                text += "<at id=\"self\"/>";
              } else {
                text += "<at id=\"" + qq + "\"/>";
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
    // Trim.
    auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) { return result; }
    auto end = text.find_last_not_of(" \t\n\r");
    result.text = text.substr(start, end - start + 1);
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
          if (qq == self_id) {
            text += "<at id=\"self\"/>";
          } else {
            text += "<at id=\"" + qq + "\"/>";
          }
        }
      }
      // Skip other segment types (image, face, etc.)
    }
    // Trim.
    auto start = text.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
      auto end = text.find_last_not_of(" \t\n\r");
      result.text = text.substr(start, end - start + 1);
    }
    return result;
  }

  return result;
}

}  // namespace shizuru::io::onebot
