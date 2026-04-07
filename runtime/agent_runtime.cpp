#include "agent_runtime.h"

#include <chrono>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "async_logger.h"
#include "llm/openai/openai_client.h"
#include "memory/in_memory_store.h"
#include "audit/log_audit_sink.h"
#include "runtime/tool_dispatch_device.h"

namespace shizuru::runtime {

AgentRuntime::AgentRuntime(RuntimeConfig config, services::ToolRegistry& tools)
    : config_(std::move(config)), tools_(tools) {
  core::InitLogger(config_.logger);
}

AgentRuntime::~AgentRuntime() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// Device management
// ---------------------------------------------------------------------------

void AgentRuntime::RegisterDevice(std::unique_ptr<io::IoDevice> device) {
  const std::string id = device->GetDeviceId();

  // Wire the device's output callback to DispatchFrame before taking the lock,
  // so the lambda captures `this` safely (runtime outlives devices).
  device->SetOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             io::DataFrame frame) {
        DispatchFrame(device_id, port_name, std::move(frame));
      });

  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  if (devices_.count(id)) {
    throw std::invalid_argument("Device already registered: " + id);
  }
  registration_order_.push_back(id);
  devices_[id] = std::move(device);
}

void AgentRuntime::UnregisterDevice(const std::string& device_id) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  auto it = devices_.find(device_id);
  if (it == devices_.end()) { return; }

  for (auto& route : route_table_.AllRoutes()) {
    if (route.source.device_id == device_id ||
        route.destination.device_id == device_id) {
      route_table_.RemoveRoute(route.source, route.destination);
    }
  }

  devices_.erase(it);
  registration_order_.erase(
      std::remove(registration_order_.begin(), registration_order_.end(),
                  device_id),
      registration_order_.end());
}

// ---------------------------------------------------------------------------
// Route management
// ---------------------------------------------------------------------------

void AgentRuntime::AddRoute(PortAddress source, PortAddress destination,
                             RouteOptions options) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  route_table_.AddRoute(std::move(source), std::move(destination), options);
}

void AgentRuntime::RemoveRoute(const PortAddress& source,
                                const PortAddress& destination) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  route_table_.RemoveRoute(source, destination);
}

// ---------------------------------------------------------------------------
// Backward-compatible public API
// ---------------------------------------------------------------------------

std::string AgentRuntime::StartSession() {
  {
    std::unique_lock<std::shared_mutex> lock(devices_mutex_);
    if (HasActiveSessionLocked()) {
      // Stop devices under the lock before rebuilding.
      for (auto it = registration_order_.rbegin();
           it != registration_order_.rend(); ++it) {
        auto dev_it = devices_.find(*it);
        if (dev_it != devices_.end()) {
          try { dev_it->second->Stop(); } catch (...) {}
        }
      }
      devices_.clear();
      registration_order_.clear();
      route_table_ = RouteTable{};
      core_device_ = nullptr;
    }
  }

  const std::string session_id =
      "session-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());

  // Build CoreDevice with all session dependencies.
  auto llm    = std::make_unique<services::OpenAiClient>(config_.llm);
  auto memory = std::make_unique<services::InMemoryStore>();
  auto audit  = std::make_unique<services::LogAuditSink>();

  auto core = std::make_unique<CoreDevice>(
      "core", session_id,
      config_.controller, config_.context, config_.policy,
      std::move(llm), std::move(memory), std::move(audit),
      config_.observation_filter_factory
          ? config_.observation_filter_factory() : nullptr,
      config_.tts_segment_factory
          ? config_.tts_segment_factory() : nullptr,
      config_.response_filter_factory
          ? config_.response_filter_factory() : nullptr);

  // Wire output callback before registering.
  core->SetOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             io::DataFrame frame) {
        DispatchFrame(device_id, port_name, std::move(frame));
      });

  auto tool_dispatch = std::make_unique<ToolDispatchDevice>(tools_);
  tool_dispatch->SetOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             io::DataFrame frame) {
        DispatchFrame(device_id, port_name, std::move(frame));
      });

  {
    std::unique_lock<std::shared_mutex> lock(devices_mutex_);
    core_device_ = core.get();
    registration_order_.push_back("core");
    devices_["core"] = std::move(core);

    registration_order_.push_back("tool_dispatch");
    devices_["tool_dispatch"] = std::move(tool_dispatch);

    route_table_.AddRoute(PortAddress{"core", "text_out"},
                          PortAddress{"app_output", "text_in"},
                          RouteOptions{.requires_control_plane = false});

    // Tool call async round-trip routes (Requirements 4.2, 4.3).
    route_table_.AddRoute(PortAddress{"core", "action_out"},
                          PortAddress{"tool_dispatch", "action_in"},
                          RouteOptions{.requires_control_plane = true});
    route_table_.AddRoute(PortAddress{"tool_dispatch", "result_out"},
                          PortAddress{"core", "tool_result_in"},
                          RouteOptions{.requires_control_plane = true});

    // VAD event route: DMA path (Requirement 8.6).
    route_table_.AddRoute(PortAddress{"vad_event", "vad_out"},
                          PortAddress{"core", "vad_in"},
                          RouteOptions{.requires_control_plane = false});

    // Control plane routes to IO devices (Requirement 8.7).
    route_table_.AddRoute(PortAddress{"core", "control_out"},
                          PortAddress{"baidu_asr", "control_in"},
                          RouteOptions{.requires_control_plane = true});
    route_table_.AddRoute(PortAddress{"core", "control_out"},
                          PortAddress{"elevenlabs_tts", "control_in"},
                          RouteOptions{.requires_control_plane = true});
    route_table_.AddRoute(PortAddress{"core", "control_out"},
                          PortAddress{"audio_playout", "control_in"},
                          RouteOptions{.requires_control_plane = true});
  }

  // Start all devices (outside the lock — Start() may call back into DispatchFrame).
  std::vector<std::string> order;
  {
    std::shared_lock<std::shared_mutex> lock(devices_mutex_);
    order = registration_order_;
  }
  for (const auto& id : order) {
    std::shared_lock<std::shared_mutex> lock(devices_mutex_);
    auto it = devices_.find(id);
    if (it != devices_.end()) { it->second->Start(); }
  }

  LOG_INFO("[{}] Session started: {}", MODULE_NAME, session_id);
  return session_id;
}

void AgentRuntime::SendMessage(const std::string& content) {
  if (!core_device_) {
    LOG_WARN("[{}] SendMessage called with no active session", MODULE_NAME);
    return;
  }

  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload = std::vector<uint8_t>(content.begin(), content.end());
  frame.source_device = "user";
  frame.source_port = "text";
  frame.timestamp = std::chrono::steady_clock::now();

  // Deliver directly to CoreDevice's text_in port (Requirement 11.6).
  core_device_->OnInput("text_in", std::move(frame));
}

void AgentRuntime::OnOutput(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void AgentRuntime::Shutdown() {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  if (devices_.empty()) { return; }

  // Stop devices in reverse registration order (Requirement 8.6).
  for (auto it = registration_order_.rbegin();
       it != registration_order_.rend(); ++it) {
    auto dev_it = devices_.find(*it);
    if (dev_it != devices_.end()) {
      try {
        dev_it->second->Stop();
      } catch (const std::exception& e) {
        LOG_ERROR("[{}] Error stopping device {}: {}", MODULE_NAME, *it,
                  e.what());
      }
    }
  }

  devices_.clear();
  registration_order_.clear();
  route_table_ = RouteTable{};
  core_device_ = nullptr;

  LOG_INFO("[{}] Session shut down", MODULE_NAME);
}

core::State AgentRuntime::GetState() const {
  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  if (!core_device_) { return core::State::kTerminated; }
  return core_device_->GetState();
}

bool AgentRuntime::HasActiveSession() const {
  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  return core_device_ != nullptr;
}

// ---------------------------------------------------------------------------
// Internal routing
// ---------------------------------------------------------------------------

void AgentRuntime::DispatchFrame(const std::string& device_id,
                                  const std::string& port_name,
                                  io::DataFrame frame) {
  PortAddress source{device_id, port_name};

  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto destinations = route_table_.Lookup(source);

  if (destinations.empty()) {
    // No routes — silently discard (Requirement 3.5).
    return;
  }

  for (const auto& [dest, opts] : destinations) {
    // Handle the virtual app_output sink inline.
    if (dest.device_id == "app_output") {
      if (frame.type == "text/plain") {
        RuntimeOutput output;
        output.text = std::string(frame.payload.begin(), frame.payload.end());
        output.is_partial = (frame.metadata.count("streaming") &&
                             frame.metadata.at("streaming") == "1");
        OutputCallback cb;
        {
          std::lock_guard<std::mutex> lock(output_cb_mutex_);
          cb = output_cb_;
        }
        if (cb) { cb(output); }
      }
      continue;
    }

    // Deliver to registered device.
    auto it = devices_.find(dest.device_id);
    if (it == devices_.end()) {
      LOG_WARN("[{}] Route destination not found: {}", MODULE_NAME,
               dest.device_id);
      continue;
    }

    try {
      // Zero transformation: frame passes through untouched (Requirement 8.1).
      it->second->OnInput(dest.port_name, frame);
    } catch (const std::exception& e) {
      LOG_ERROR("[{}] Error delivering frame to {}:{} — {}", MODULE_NAME,
                dest.device_id, dest.port_name, e.what());
    }
  }
}

}  // namespace shizuru::runtime
