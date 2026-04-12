/// Structured activity events from the C++ controller.
/// Ordinals must match core::ActivityKind in controller/types.h.
enum ActivityKind {
  bufferingInput,     // 0
  filteringInput,     // 1
  thinkingStarted,    // 2
  thinkingRetry,      // 3
  toolDispatched,     // 4
  toolResultReceived, // 5
  speaking,           // 6
  interrupted,        // 7
  turnComplete,       // 8
  budgetExhausted,    // 9
}

extension ActivityKindExtension on ActivityKind {
  static ActivityKind fromInt(int value) {
    if (value >= 0 && value < ActivityKind.values.length) {
      return ActivityKind.values[value];
    }
    return ActivityKind.turnComplete; // safe fallback
  }
}
