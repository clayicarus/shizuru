#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "async_logger.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "io/audio/audio_device/audio_frame.h"
#include "io/io_device.h"
#include "runtime/port_payload_kind.h"
#include "runtime/route_table.h"

namespace shizuru::runtime {

// Options for device registration.
struct DeviceOptions {
  // If true, StartAll() will call Start() on this device.
  // If false, the caller must start it manually via StartDevice().
  bool auto_start = true;
};

// Pure device bus — registers devices, manages routes, dispatches frames,
// controls lifecycle.  Zero data transformation, zero business logic.
//
// All session assembly (creating CoreDevice, wiring routes, choosing LLM
// vendor, registering tools) belongs in the application layer above this.
class AgentRuntime {
 public:
  AgentRuntime();
  ~AgentRuntime();

  AgentRuntime(const AgentRuntime&) = delete;
  AgentRuntime& operator=(const AgentRuntime&) = delete;

  // ── Device management ──────────────────────────────────────────────────

  // Register a device. Throws if a device with the same ID is already registered.
  void RegisterDevice(std::unique_ptr<io::IoDevice> device,
                      DeviceOptions options = {});

  // Remove a device and all routes referencing it.
  void UnregisterDevice(const std::string& device_id);

  // Non-owning lookup.  Returns nullptr if not found.
  io::IoDevice* FindDevice(const std::string& device_id);

  // ── Route management ───────────────────────────────────────────────────

  void AddRoute(PortAddress source, PortAddress destination,
                RouteOptions options = {});
  void RemoveRoute(const PortAddress& source, const PortAddress& destination);

  // Enable or disable a route without removing it.  Returns true if found.
  bool SetRouteEnabled(const PortAddress& source,
                       const PortAddress& destination,
                       bool enabled);

  // ── Lifecycle ──────────────────────────────────────────────────────────

  // Start all devices that were registered with auto_start = true,
  // in registration order.
  void StartAll();

  // Stop all devices in reverse registration order and clear all state.
  void Shutdown();

  // Start / stop a single device by ID.
  void StartDevice(const std::string& device_id);
  void StopDevice(const std::string& device_id);

  // ── App Output Sink ────────────────────────────────────────────────────

  // Register callbacks for typed payloads routed to the virtual "app_output"
  // sink. This is the only special device ID — it exists so the bus can
  // deliver typed outputs to the application layer without a real IoDevice.
  using AudioFrameSinkCallback = std::function<void(io::AudioFrame frame)>;
  using ConversationItemSinkCallback =
      std::function<void(core::ConversationItem item)>;
  using ControlSignalSinkCallback =
      std::function<void(core::ControlSignal signal)>;
  void OnAudioFrameSink(AudioFrameSinkCallback cb);
  void OnConversationItemSink(ConversationItemSinkCallback cb);
  void OnControlSignalSink(ControlSignalSinkCallback cb);

 private:
  static constexpr char kAppOutputDeviceId[] = "app_output";
  static constexpr char kAppOutputAudioInPort[] = "audio_in";
  static constexpr char kAppOutputItemInPort[] = "item_in";
  static constexpr char kAppOutputControlInPort[] = "control_in";

  std::optional<io::PortDescriptor> FindPortDescriptor(
      const std::string& device_id,
      const std::string& port_name) const;
  void DispatchAudioFrame(const std::string& device_id,
                          const std::string& port_name,
                          io::AudioFrame frame);
  void DispatchConversationItem(const std::string& device_id,
                                const std::string& port_name,
                                core::ConversationItem item);
  void DispatchControlSignal(const std::string& device_id,
                             const std::string& port_name,
                             core::ControlSignal signal);

  static constexpr char MODULE_NAME[] = "Runtime";

  struct DeviceEntry {
    std::unique_ptr<io::IoDevice> device;
    DeviceOptions options;
  };

  std::unordered_map<std::string, DeviceEntry> devices_;
  std::vector<std::string> registration_order_;
  RouteTable route_table_;

  mutable std::shared_mutex devices_mutex_;

  std::mutex sink_cb_mutex_;
  AudioFrameSinkCallback audio_sink_cb_;
  ConversationItemSinkCallback item_sink_cb_;
  ControlSignalSinkCallback control_sink_cb_;
};

}  // namespace shizuru::runtime
