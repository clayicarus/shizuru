import 'package:flutter/widgets.dart';

class ConversationMessage {
  final String role; // 'user' | 'assistant' | 'tool_call' | 'tool_result'
  String text;
  final DateTime timestamp;
  bool isStreaming;

  // Tool call fields.
  String? toolName;
  String? toolArguments;
  String? toolCallId;
  bool? toolSuccess;

  ConversationMessage({
    required this.role,
    required this.text,
    required this.timestamp,
    this.isStreaming = false,
    this.toolName,
    this.toolArguments,
    this.toolCallId,
    this.toolSuccess,
  });
}

class ConversationProvider extends ChangeNotifier {
  final List<ConversationMessage> _messages = [];
  final ScrollController scrollController = ScrollController();

  // Index of the current streaming assistant bubble (-1 if none).
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

  void onOutputChunk(String text, bool isPartial) {
    if (isPartial) {
      if (_streamingIndex >= 0 && _streamingIndex < _messages.length) {
        // Replace with the latest accumulated text (C++ sends cumulative).
        _messages[_streamingIndex].text = text;
      } else {
        // First partial chunk — create a new streaming bubble.
        _messages.add(ConversationMessage(
          role: 'assistant',
          text: text,
          timestamp: DateTime.now(),
          isStreaming: true,
        ));
        _streamingIndex = _messages.length - 1;
      }
    } else {
      // Final complete response.
      if (_streamingIndex >= 0 && _streamingIndex < _messages.length) {
        final bubble = _messages[_streamingIndex];
        bubble.isStreaming = false;
        if (text.isNotEmpty) bubble.text = text;
        _streamingIndex = -1;
      } else {
        _messages.add(ConversationMessage(
          role: 'assistant',
          text: text,
          timestamp: DateTime.now(),
          isStreaming: false,
        ));
      }
    }
    notifyListeners();
    // During streaming use jumpTo to avoid animation conflicts causing flicker.
    _scrollToBottom(animate: !isPartial);
  }

  void addToolCall(String name, String arguments, String callId) {
    _messages.add(ConversationMessage(
      role: 'tool_call',
      text: '',
      timestamp: DateTime.now(),
      toolName: name,
      toolArguments: arguments,
      toolCallId: callId,
    ));
    notifyListeners();
    _scrollToBottom(animate: true);
  }

  void updateToolResult(String callId, bool success, String resultText) {
    final idx = _messages.lastIndexWhere(
        (m) => m.role == 'tool_call' && m.toolCallId == callId);
    if (idx >= 0) {
      _messages[idx].toolSuccess = success;
      _messages[idx].text = resultText;
    } else {
      _messages.add(ConversationMessage(
        role: 'tool_result',
        text: resultText,
        timestamp: DateTime.now(),
        toolCallId: callId,
        toolSuccess: success,
      ));
    }
    notifyListeners();
    _scrollToBottom(animate: true);
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
