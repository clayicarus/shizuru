import 'dart:convert';

import 'package:flutter/widgets.dart';

enum ConversationToolEventKind {
  toolCall,
  toolResult,
}

class ConversationToolEvent {
  final ConversationToolEventKind kind;
  final Map<String, dynamic> data;

  ConversationToolEvent({
    required this.kind,
    required this.data,
  });
}

class ConversationMessage {
  final String role; // 'user' | 'assistant'
  String text;
  final DateTime timestamp;
  bool isStreaming;
  final List<ConversationToolEvent> events;

  ConversationMessage({
    required this.role,
    required this.text,
    required this.timestamp,
    this.isStreaming = false,
    List<ConversationToolEvent>? events,
  }) : events = events ?? <ConversationToolEvent>[];
}

class ConversationProvider extends ChangeNotifier {
  final List<ConversationMessage> _messages = [];
  final ScrollController scrollController = ScrollController();

  int _streamingIndex = -1;

  List<ConversationMessage> get messages => List.unmodifiable(_messages);

  void addUserMessage(String text) {
    _messages.add(ConversationMessage(
      role: 'user',
      text: text,
      timestamp: DateTime.now(),
    ));
    notifyListeners();
    _scrollToBottom(animate: true);
  }

  /// Single entry point for all conversation items from C++.
  /// [itemJson] is a serialized ConversationItem.
  /// [isDelta] is true for streaming token deltas.
  void onConversationItem(String itemJson, bool isDelta) {
    Map<String, dynamic> item;
    try {
      item = jsonDecode(itemJson) as Map<String, dynamic>;
    } catch (_) {
      return;
    }

    final kind = item['kind'] as String? ?? '';
    final payload = item['payload'] as Map<String, dynamic>? ?? {};

    switch (kind) {
      case 'assistant_message':
        _handleAssistantMessage(payload, isDelta);
        break;
      case 'tool_call':
        _handleToolCall(payload);
        break;
      case 'tool_result':
        _handleToolResult(payload);
        break;
      default:
        // Ignore unknown kinds (human_message, system_event, etc.)
        break;
    }

    notifyListeners();
    _scrollToBottom(animate: !isDelta);
  }

  void _handleAssistantMessage(Map<String, dynamic> payload, bool isDelta) {
    final text = payload['text'] as String? ?? '';
    if (isDelta) {
      // Streaming token delta — append to current bubble.
      final bubble = _ensureAssistantBubble();
      bubble.text += text;
    } else {
      // Final complete response.
      if (_streamingIndex >= 0 && _streamingIndex < _messages.length) {
        final bubble = _messages[_streamingIndex];
        bubble.isStreaming = false;
        // Preserve <think> blocks from streaming, append final text.
        final thinkRegex = RegExp(r'<think>.*?</think>', dotAll: true);
        final blocks = thinkRegex.allMatches(bubble.text)
            .map((m) => m.group(0)!)
            .join();
        bubble.text = blocks.isNotEmpty ? '$blocks$text' : text;
        _streamingIndex = -1;
      } else {
        _messages.add(ConversationMessage(
          role: 'assistant',
          text: text,
          timestamp: DateTime.now(),
        ));
      }
    }
  }

  void _handleToolCall(Map<String, dynamic> payload) {
    final bubble = _ensureAssistantBubble();
    final toolCalls = payload['tool_calls'] as List<dynamic>? ?? [];
    for (final tc in toolCalls) {
      if (tc is Map<String, dynamic>) {
        final fn = tc['function'] as Map<String, dynamic>? ?? {};
        bubble.events.add(ConversationToolEvent(
          kind: ConversationToolEventKind.toolCall,
          data: {
            'id': tc['id'] ?? '',
            'name': fn['name'] ?? 'tool',
            'arguments': fn['arguments'],
          },
        ));
      }
    }
  }

  void _handleToolResult(Map<String, dynamic> payload) {
    final bubble = _ensureAssistantBubble();
    final content = payload['content'];
    bool success = false;
    if (content is Map<String, dynamic>) {
      success = content['success'] as bool? ?? false;
    }
    bubble.events.add(ConversationToolEvent(
      kind: ConversationToolEventKind.toolResult,
      data: {
        'tool_name': payload['tool_name'] ?? '',
        'tool_call_id': payload['tool_call_id'] ?? '',
        'success': success,
        'result': content,
      },
    ));
  }

  ConversationMessage _ensureAssistantBubble() {
    if (_streamingIndex >= 0 && _streamingIndex < _messages.length) {
      return _messages[_streamingIndex];
    }
    _messages.add(ConversationMessage(
      role: 'assistant',
      text: '',
      timestamp: DateTime.now(),
      isStreaming: true,
    ));
    _streamingIndex = _messages.length - 1;
    return _messages[_streamingIndex];
  }

  /// Legacy callback adapter — routes to onConversationItem.
  /// Used by existing onOutput bridge callback.
  void onOutputChunk(String text, bool isPartial) {
    onConversationItem(text, isPartial);
  }

  void onToolActivity(int kind, String detail) {
    // No longer used for bubble construction — activity events are
    // now delivered as ConversationItems through the output callback.
    // Keep for debug panel only.
  }

  void clearMessages() {
    _messages.clear();
    _streamingIndex = -1;
    notifyListeners();
  }

  void _scrollToBottom({bool animate = false}) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!scrollController.hasClients) return;
      final pos = scrollController.position;
      if (animate) {
        scrollController.animateTo(
          pos.maxScrollExtent,
          duration: const Duration(milliseconds: 200),
          curve: Curves.easeOut,
        );
      } else {
        scrollController.jumpTo(pos.maxScrollExtent);
      }
    });
  }

  @override
  void dispose() {
    scrollController.dispose();
    super.dispose();
  }
}
