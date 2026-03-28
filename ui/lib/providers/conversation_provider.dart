import 'package:flutter/widgets.dart';

class ConversationMessage {
  final String role; // 'user' | 'assistant'
  String text;
  final DateTime timestamp;
  bool isStreaming;

  ConversationMessage({
    required this.role,
    required this.text,
    required this.timestamp,
    this.isStreaming = false,
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
