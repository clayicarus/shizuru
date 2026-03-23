import 'package:flutter/material.dart';

enum AgentState {
  idle,
  listening,
  thinking,
  routing,
  acting,
  responding,
  error,
  terminated,
}

extension AgentStateExt on AgentState {
  String get label {
    switch (this) {
      case AgentState.idle:        return '就绪';
      case AgentState.listening:   return '正在聆听';
      case AgentState.thinking:    return '思考中';
      case AgentState.routing:     return '决策中';
      case AgentState.acting:      return '调用工具';
      case AgentState.responding:  return '回复中';
      case AgentState.error:       return '出错';
      case AgentState.terminated:  return '已结束';
    }
  }

  Color get color {
    switch (this) {
      case AgentState.idle:        return const Color(0xFF4CAF50);
      case AgentState.listening:   return const Color(0xFF2196F3);
      case AgentState.thinking:    return const Color(0xFFFF9800);
      case AgentState.routing:     return const Color(0xFFFF9800);
      case AgentState.acting:      return const Color(0xFF9C27B0);
      case AgentState.responding:  return const Color(0xFF00BCD4);
      case AgentState.error:       return const Color(0xFFF44336);
      case AgentState.terminated:  return const Color(0xFF757575);
    }
  }

  IconData get icon {
    switch (this) {
      case AgentState.idle:        return Icons.circle_outlined;
      case AgentState.listening:   return Icons.mic_rounded;
      case AgentState.thinking:    return Icons.psychology_rounded;
      case AgentState.routing:     return Icons.route_rounded;
      case AgentState.acting:      return Icons.build_circle_outlined;
      case AgentState.responding:  return Icons.text_fields_rounded;
      case AgentState.error:       return Icons.error_outline_rounded;
      case AgentState.terminated:  return Icons.stop_circle_outlined;
    }
  }
}
