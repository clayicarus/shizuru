enum AgentState {
  idle,        // 0
  listening,   // 1
  thinking,    // 2
  routing,     // 3
  acting,      // 4
  responding,  // 5
  error,       // 6
  terminated,  // 7
}

extension AgentStateExtension on AgentState {
  String get displayName {
    switch (this) {
      case AgentState.idle:       return 'Idle';
      case AgentState.listening:  return 'Listening...';
      case AgentState.thinking:   return 'Thinking...';
      case AgentState.routing:    return 'Planning...';
      case AgentState.acting:     return 'Using tool...';
      case AgentState.responding: return 'Responding...';
      case AgentState.error:      return 'Error';
      case AgentState.terminated: return 'Terminated';
    }
  }

  static AgentState fromInt(int value) {
    if (value >= 0 && value < AgentState.values.length) {
      return AgentState.values[value];
    }
    return AgentState.terminated;
  }
}
