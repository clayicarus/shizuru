#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Windows <windows.h> defines SendMessage as a macro expanding to
// SendMessageA or SendMessageW, which breaks any C++ method with the same
// name.  Undefine it so our AgentRuntime::SendMessage compiles correctly.
#ifdef SendMessage
#undef SendMessage
#endif

#include "controller/types.h"
#include "io/tool_registry.h"
#include "llm/config.h"
#include "async_logger.h"
#include "io/io_device.h"
#include "io/data_frame.h"
#include "runtime/route_table.h"
#include "runtime/core_device.h"
#include "strategies/observation_filter.h"
#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::runtime {

// Strategy factory types — called once per session in StartSession().
// Using std::function so callers can use lambdas, not just subclasses.
using ObservationAggregatorFactory =
    std::function<std::unique_ptr<core::ObservationAggregator>()>;
using ObservationFilterFactory =
    std::function<std::unique_ptr<core::ObservationFilter>()>;
using TtsSegmentStrategyFactory =
    std::function<std::unique_ptr<core::TtsSegmentStrategy>()>;
using ResponseFilterFactory =
    std::function<std::unique_ptr<core::ResponseFilter>()>;

// Configuration bundle for creating an AgentRuntime.
struct RuntimeConfig {
  core::ControllerConfig controller;
  core::ContextConfig context;
  core::PolicyConfig policy;
  services::OpenAiConfig llm;
  core::LoggerConfig logger;

  // Optional strategy factories.  If set, StartSession() calls them to
  // create strategy instances injected into the Controller.
  // If null, defaults are used (AcceptAll, no TTS segmentation, Passthrough).
  ObservationAggregatorFactory observation_aggregator_factory;
  ObservationFilterFactory observation_filter_factory;
  TtsSegmentStrategyFactory tts_segment_factory;
  ResponseFilterFactory response_filter_factory;
};

// Final output emitted by AgentRuntime for a user turn.
struct RuntimeOutput {
  std::string text;
  bool is_partial = false;  // true for streaming token chunks, false for final response
};

// Top-level entry point that assembles all components and manages
// the lifecycle of an AgentSession. Acts as a bus router: zero data
// transformation, purely lifecycle management and frame routing.
class AgentRuntime {
 public:
  using OutputCallback = std::function<void(const RuntimeOutput& output)>;
  using DiagnosticCallback = std::function<void(const std::string& message)>;

  AgentRuntime(RuntimeConfig config, services::ToolRegistry& tools);
  ~AgentRuntime();

  AgentRuntime(const AgentRuntime&) = delete;
  AgentRuntime& operator=(const AgentRuntime&) = delete;

  // Device management
  void RegisterDevice(std::unique_ptr<io::IoDevice> device);
  void UnregisterDevice(const std::string& device_id);

  // Route management (delegates to route_table_)
  void AddRoute(PortAddress source, PortAddress destination,
                RouteOptions options = {});
  void RemoveRoute(const PortAddress& source, const PortAddress& destination);

  // Enable or disable a route without removing it. Disabled routes exist
  // but DispatchFrame skips them. Returns true if the route was found.
  bool SetRouteEnabled(const PortAddress& source, const PortAddress& destination,
                       bool enabled);

  // Backward-compatible public API
  // Create and start a new session. Returns the session ID.
  std::string StartSession();

  // Send a user text message to the active session.
  void SendMessage(const std::string& content);

  // Register callback for final text outputs.
  void OnOutput(OutputCallback cb);

  // Register callback for diagnostic/activity events.
  void OnDiagnostic(DiagnosticCallback cb);

  // Shut down the active session.
  void Shutdown();

  // Query the current state of the active session.
  core::State GetState() const;

  // Check if a session is active.
  bool HasActiveSession() const;

 private:
  // Private helpers
  void DispatchFrame(const std::string& device_id,
                     const std::string& port_name,
                     io::DataFrame frame);
  // Non-locking version for internal use (caller must hold devices_mutex_).
  bool HasActiveSessionLocked() const { return core_device_ != nullptr; }

  static constexpr char MODULE_NAME[] = "Runtime";

  RuntimeConfig config_;
  services::ToolRegistry& tools_;

  RouteTable route_table_;
  std::unordered_map<std::string, std::unique_ptr<io::IoDevice>> devices_;
  std::vector<std::string> registration_order_;  // for ordered shutdown

  CoreDevice* core_device_ = nullptr;  // non-owning pointer into devices_

  mutable std::shared_mutex devices_mutex_;  // protects devices_ and route_table_
  mutable std::mutex output_cb_mutex_;
  OutputCallback output_cb_;
  mutable std::mutex diagnostic_cb_mutex_;
  DiagnosticCallback diagnostic_cb_;
};

}  // namespace shizuru::runtime
