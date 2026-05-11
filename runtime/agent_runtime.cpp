#include "agent_runtime.h"

#include <algorithm>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "async_logger.h"

namespace shizuru::runtime {

constexpr char AgentRuntime::kAppOutputDeviceId[];
constexpr char AgentRuntime::kAppOutputFrameInPort[];
constexpr char AgentRuntime::kAppOutputAudioInPort[];
constexpr char AgentRuntime::kAppOutputItemInPort[];
constexpr char AgentRuntime::kAppOutputControlInPort[];

AgentRuntime::AgentRuntime() = default;

AgentRuntime::~AgentRuntime() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// Device management
// ---------------------------------------------------------------------------

void AgentRuntime::RegisterDevice(std::unique_ptr<io::IoDevice> device,
                                  DeviceOptions options) {
  const std::string id = device->GetDeviceId();

  // Wire the device's output callbacks to the runtime bus before taking the lock.
  device->SetOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             io::DataFrame frame) {
        DispatchFrame(device_id, port_name, std::move(frame));
      });
  device->SetAudioFrameOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             io::AudioFrame frame) {
        DispatchAudioFrame(device_id, port_name, std::move(frame));
      });
  device->SetConversationItemOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             core::ConversationItem item) {
        DispatchConversationItem(device_id, port_name, std::move(item));
      });
  device->SetControlSignalOutputCallback(
      [this](const std::string& device_id, const std::string& port_name,
             core::ControlSignal signal) {
        DispatchControlSignal(device_id, port_name, std::move(signal));
      });

  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  if (devices_.count(id) != 0) {
    throw std::invalid_argument("Device already registered: " + id);
  }
  registration_order_.emplace_back(id);
  devices_[id] = DeviceEntry{std::move(device), options};
}

void AgentRuntime::UnregisterDevice(const std::string& device_id) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  auto it = devices_.find(device_id);
  if (it == devices_.end()) { return; }

  for (const auto& route : route_table_.AllRoutes()) {
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

io::IoDevice* AgentRuntime::FindDevice(const std::string& device_id) {
  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto it = devices_.find(device_id);
  if (it == devices_.end()) { return nullptr; }
  return it->second.device.get();
}

// ---------------------------------------------------------------------------
// Route management
// ---------------------------------------------------------------------------

void AgentRuntime::AddRoute(PortAddress source, PortAddress destination,
                            RouteOptions options) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);

  auto find_port = [&](const std::string& device_id,
                       const std::string& port_name)
      -> std::optional<io::PortDescriptor> {
    auto dev_it = devices_.find(device_id);
    if (dev_it == devices_.end()) { return std::nullopt; }
    const auto ports = dev_it->second.device->GetPortDescriptors();
    for (const auto& port : ports) {
      if (port.name == port_name) { return port; }
    }
    return std::nullopt;
  };

  const auto src_port = find_port(source.device_id, source.port_name);
  if (!src_port.has_value()) {
    throw std::invalid_argument(
        "Route source port not found: " + source.device_id + ":" + source.port_name);
  }
  if (src_port->direction != io::PortDirection::kOutput) {
    throw std::invalid_argument(
        "Route source port is not an output: " + source.device_id + ":" + source.port_name);
  }

  // app_output remains a special virtual sink during migration, but it must
  // still obey the new typed-plane contract: the virtual port name must match
  // the payload kind.
  if (destination.device_id == kAppOutputDeviceId) {
    const bool port_matches =
        (src_port->payload_kind == PortPayloadKind::kLegacyFrame &&
         destination.port_name == kAppOutputFrameInPort) ||
        (src_port->payload_kind == PortPayloadKind::kAudioFrame &&
         destination.port_name == kAppOutputAudioInPort) ||
        (src_port->payload_kind == PortPayloadKind::kConversationItem &&
         destination.port_name == kAppOutputItemInPort) ||
        (src_port->payload_kind == PortPayloadKind::kControlSignal &&
         destination.port_name == kAppOutputControlInPort);
    if (!port_matches) {
      throw std::invalid_argument(
          "Route destination app_output port does not match payload kind");
    }
  } else {
    const auto dst_port =
        find_port(destination.device_id, destination.port_name);
    if (!dst_port.has_value()) {
      throw std::invalid_argument(
          "Route destination port not found: " + destination.device_id + ":" +
          destination.port_name);
    }
    if (dst_port->direction != io::PortDirection::kInput) {
      throw std::invalid_argument(
          "Route destination port is not an input: " + destination.device_id + ":" +
          destination.port_name);
    }
    if (src_port->payload_kind != dst_port->payload_kind) {
      throw std::invalid_argument(
          "Route payload kind mismatch between " + source.device_id + ":" +
          source.port_name + " and " + destination.device_id + ":" +
          destination.port_name);
    }
  }

  route_table_.AddRoute(std::move(source), std::move(destination), options);
}

void AgentRuntime::RemoveRoute(const PortAddress& source,
                               const PortAddress& destination) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  route_table_.RemoveRoute(source, destination);
}

bool AgentRuntime::SetRouteEnabled(const PortAddress& source,
                                   const PortAddress& destination,
                                   bool enabled) {
  std::unique_lock<std::shared_mutex> lock(devices_mutex_);
  return route_table_.SetRouteEnabled(source, destination, enabled);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AgentRuntime::StartAll() {
  std::vector<std::string> order;
  {
    std::shared_lock<std::shared_mutex> lock(devices_mutex_);
    order = registration_order_;
  }
  for (const auto& id : order) {
    std::shared_lock<std::shared_mutex> lock(devices_mutex_);
    auto it = devices_.find(id);
    if (it != devices_.end() && it->second.options.auto_start) {
      it->second.device->Start();
    }
  }
  LOG_INFO("[{}] StartAll complete ({} devices)", MODULE_NAME, order.size());
}

void AgentRuntime::Shutdown() {
  // Move all ownership out under the lock, then stop outside the lock.
  // This avoids deadlock: device callbacks may call DispatchFrame which
  // acquires a shared (read) lock.  If we held a unique (write) lock
  // while calling Stop(), the callback thread would block — deadlock.
  std::vector<std::pair<std::string, std::unique_ptr<io::IoDevice>>> to_stop;
  {
    std::unique_lock<std::shared_mutex> lock(devices_mutex_);
    if (devices_.empty()) { return; }
    auto order = std::move(registration_order_);
    registration_order_.clear();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      auto dev_it = devices_.find(*it);
      if (dev_it != devices_.end()) {
        to_stop.emplace_back(dev_it->first, std::move(dev_it->second.device));
      }
    }
    devices_.clear();
    route_table_ = RouteTable{};
  }

  for (const auto& [id, device] : to_stop) {
    try {
      device->Stop();
    } catch (const std::exception& e) {
      LOG_ERROR("[{}] Error stopping device {}: {}", MODULE_NAME, id, e.what());
    }
  }
  LOG_INFO("[{}] Shutdown complete", MODULE_NAME);
}

void AgentRuntime::StartDevice(const std::string& device_id) {
  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto it = devices_.find(device_id);
  if (it != devices_.end()) {
    it->second.device->Start();
  }
}

void AgentRuntime::StopDevice(const std::string& device_id) {
  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto it = devices_.find(device_id);
  if (it != devices_.end()) {
    it->second.device->Stop();
  }
}

// ---------------------------------------------------------------------------
// Frame sink
// ---------------------------------------------------------------------------

void AgentRuntime::OnFrameSink(FrameSinkCallback cb) {
  std::lock_guard<std::mutex> lock(sink_cb_mutex_);
  sink_cb_ = std::move(cb);
}

void AgentRuntime::OnAudioFrameSink(AudioFrameSinkCallback cb) {
  std::lock_guard<std::mutex> lock(sink_cb_mutex_);
  audio_sink_cb_ = std::move(cb);
}

void AgentRuntime::OnConversationItemSink(ConversationItemSinkCallback cb) {
  std::lock_guard<std::mutex> lock(sink_cb_mutex_);
  item_sink_cb_ = std::move(cb);
}

void AgentRuntime::OnControlSignalSink(ControlSignalSinkCallback cb) {
  std::lock_guard<std::mutex> lock(sink_cb_mutex_);
  control_sink_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Internal routing
// ---------------------------------------------------------------------------

void AgentRuntime::DispatchFrame(const std::string& device_id,
                                 const std::string& port_name,
                                 io::DataFrame frame) {
  const PortAddress source{device_id, port_name};

  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto destinations = route_table_.Lookup(source);

  if (destinations.empty()) { return; }

  for (const auto& [dest, opts] : destinations) {
    // Virtual app_output sink — deliver to the registered callback.
    if (dest.device_id == kAppOutputDeviceId) {
      FrameSinkCallback cb;
      {
        std::lock_guard<std::mutex> lock2(sink_cb_mutex_);
        cb = sink_cb_;
      }
      if (cb) { cb(frame); }
      continue;
    }

    auto it = devices_.find(dest.device_id);
    if (it == devices_.end()) {
      LOG_WARN("[{}] Route destination not found: {}", MODULE_NAME,
               dest.device_id);
      continue;
    }

    try {
      it->second.device->OnInput(dest.port_name, frame);
    } catch (const std::exception& e) {
      LOG_ERROR("[{}] Error delivering frame to {}:{} — {}", MODULE_NAME,
                dest.device_id, dest.port_name, e.what());
    }
  }
}

void AgentRuntime::DispatchAudioFrame(const std::string& device_id,
                                      const std::string& port_name,
                                      io::AudioFrame frame) {
  const PortAddress source{device_id, port_name};

  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto destinations = route_table_.Lookup(source);
  if (destinations.empty()) { return; }

  for (const auto& [dest, opts] : destinations) {
    if (dest.device_id == kAppOutputDeviceId) {
      AudioFrameSinkCallback cb;
      {
        std::lock_guard<std::mutex> lock2(sink_cb_mutex_);
        cb = audio_sink_cb_;
      }
      if (cb) { cb(frame); }
      continue;
    }

    auto it = devices_.find(dest.device_id);
    if (it == devices_.end()) { continue; }

    try {
      it->second.device->OnAudioFrame(dest.port_name, frame);
    } catch (const std::exception& e) {
      LOG_ERROR("[{}] Error delivering audio frame to {}:{} — {}", MODULE_NAME,
                dest.device_id, dest.port_name, e.what());
    }
  }
}

void AgentRuntime::DispatchConversationItem(const std::string& device_id,
                                            const std::string& port_name,
                                            core::ConversationItem item) {
  const PortAddress source{device_id, port_name};

  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto destinations = route_table_.Lookup(source);
  if (destinations.empty()) { return; }

  for (const auto& [dest, opts] : destinations) {
    if (dest.device_id == kAppOutputDeviceId) {
      ConversationItemSinkCallback cb;
      {
        std::lock_guard<std::mutex> lock2(sink_cb_mutex_);
        cb = item_sink_cb_;
      }
      if (cb) { cb(item); }
      continue;
    }

    auto it = devices_.find(dest.device_id);
    if (it == devices_.end()) { continue; }

    try {
      it->second.device->OnConversationItem(dest.port_name, item);
    } catch (const std::exception& e) {
      LOG_ERROR("[{}] Error delivering conversation item to {}:{} — {}",
                MODULE_NAME, dest.device_id, dest.port_name, e.what());
    }
  }
}

void AgentRuntime::DispatchControlSignal(const std::string& device_id,
                                         const std::string& port_name,
                                         core::ControlSignal signal) {
  const PortAddress source{device_id, port_name};

  std::shared_lock<std::shared_mutex> lock(devices_mutex_);
  auto destinations = route_table_.Lookup(source);
  if (destinations.empty()) { return; }

  for (const auto& [dest, opts] : destinations) {
    if (dest.device_id == kAppOutputDeviceId) {
      ControlSignalSinkCallback cb;
      {
        std::lock_guard<std::mutex> lock2(sink_cb_mutex_);
        cb = control_sink_cb_;
      }
      if (cb) { cb(signal); }
      continue;
    }

    auto it = devices_.find(dest.device_id);
    if (it == devices_.end()) { continue; }

    try {
      it->second.device->OnControlSignal(dest.port_name, signal);
    } catch (const std::exception& e) {
      LOG_ERROR("[{}] Error delivering control signal to {}:{} — {}",
                MODULE_NAME, dest.device_id, dest.port_name, e.what());
    }
  }
}

}  // namespace shizuru::runtime
