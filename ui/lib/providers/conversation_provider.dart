import 'dart:convert';

import 'package:flutter/widgets.dart';

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

enum SegmentKind { text, thinking, toolCall, toolResult }

class BubbleSegment {
  final SegmentKind kind;
  final String? itemId;
  String text; // text / thinking content (mutable for streaming append)
  Map<String, dynamic>? data; // toolCall / toolResult structured data

  BubbleSegment.text(this.text, {this.itemId})
    : kind = SegmentKind.text,
      data = null;
  BubbleSegment.thinking(this.text, {this.itemId})
    : kind = SegmentKind.thinking,
      data = null;
  BubbleSegment.toolCall(this.data, {this.itemId})
    : kind = SegmentKind.toolCall,
      text = '';
  BubbleSegment.toolResult(this.data, {this.itemId})
    : kind = SegmentKind.toolResult,
      text = '';
}

class ConversationMessage {
  final String role; // 'user' | 'assistant'
  final DateTime timestamp;
  final String? turnGroupId;
  bool isStreaming;
  final List<BubbleSegment> segments;
  final Set<String> finalizedTextItemIds;

  ConversationMessage({
    required this.role,
    required this.timestamp,
    this.turnGroupId,
    this.isStreaming = false,
    List<BubbleSegment>? segments,
    Set<String>? finalizedTextItemIds,
  }) : segments = segments ?? <BubbleSegment>[],
       finalizedTextItemIds = finalizedTextItemIds ?? <String>{};

  /// Convenience: concatenated plain text (for clipboard, accessibility).
  String get plainText => segments
      .where((s) => s.kind == SegmentKind.text)
      .map((s) => s.text)
      .join();
}

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------

class ConversationProvider extends ChangeNotifier {
  final List<ConversationMessage> _messages = [];
  final ScrollController scrollController = ScrollController();

  int _fallbackStreamingIndex = -1;

  /// Streaming state for <think> tag detection across token boundaries.
  bool _inThinkBlock = false;
  String _tagBuf = '';

  List<ConversationMessage> get messages => List.unmodifiable(_messages);

  /// Suppress per-item notifications during bulk replay.
  bool _batchMode = false;

  /// Begin batch mode — notifications are suppressed until [endBatch].
  void beginBatch() {
    _batchMode = true;
  }

  /// End batch mode — fires a single notification and scrolls to bottom.
  void endBatch() {
    _batchMode = false;
    notifyListeners();
    _scrollToBottom(animate: false);
  }

  void addUserMessage(String text) {
    _closeStreamingAssistantBubbles();
    _messages.add(
      ConversationMessage(
        role: 'user',
        timestamp: DateTime.now(),
        segments: [BubbleSegment.text(text)],
      ),
    );
    notifyListeners();
    _scrollToBottom(animate: true);
  }

  /// Single entry point for all conversation items from C++.
  void onConversationItem(String itemJson, bool isDelta) {
    _dispatchItem(itemJson, isDelta, notify: true);
  }

  void _dispatchItem(String itemJson, bool isDelta, {required bool notify}) {
    Map<String, dynamic> item;
    try {
      item = jsonDecode(itemJson) as Map<String, dynamic>;
    } catch (_) {
      return;
    }

    final kind = item['kind'] as String? ?? '';
    final itemId = item['item_id'] as String? ?? '';
    final turnGroupId = item['turn_group_id'] as String? ?? '';
    final payload = item['payload'] as Map<String, dynamic>? ?? {};

    // Extract persisted wall-clock timestamp if present (replay path).
    final timestampMs = payload['timestamp_ms'] as int?;
    final timestamp = timestampMs != null
        ? DateTime.fromMillisecondsSinceEpoch(timestampMs)
        : DateTime.now();

    switch (kind) {
      case 'human_message':
        _handleHumanMessage(payload, timestamp: timestamp);
        break;
      case 'assistant_message':
        _handleAssistantMessage(
          payload,
          isDelta,
          itemId: itemId,
          turnGroupId: turnGroupId,
          timestamp: timestamp,
        );
        break;
      case 'tool_call':
        _handleToolCall(payload, itemId: itemId, turnGroupId: turnGroupId,
            timestamp: timestamp);
        break;
      case 'tool_result':
        _handleToolResult(payload, itemId: itemId, turnGroupId: turnGroupId,
            timestamp: timestamp);
        break;
      default:
        break;
    }

    if (notify && !_batchMode) {
      notifyListeners();
      _scrollToBottom(animate: !isDelta);
    }
  }

  // ── Streaming delta handling ───────────────────────────────────────────

  void _handleHumanMessage(Map<String, dynamic> payload,
      {DateTime? timestamp}) {
    final text = payload['text'] as String? ?? '';
    if (text.isEmpty) {
      return;
    }
    _closeStreamingAssistantBubbles();
    _messages.add(
      ConversationMessage(
        role: 'user',
        timestamp: timestamp ?? DateTime.now(),
        segments: [BubbleSegment.text(text)],
      ),
    );
  }

  void _handleAssistantMessage(
    Map<String, dynamic> payload,
    bool isDelta, {
    required String itemId,
    required String turnGroupId,
    DateTime? timestamp,
  }) {
    final text = payload['text'] as String? ?? '';
    if (isDelta) {
      final bubble = _ensureAssistantBubble(
        turnGroupId: turnGroupId,
        streaming: true,
        timestamp: timestamp,
      );
      _appendStreamingText(bubble, text, itemId: itemId);
    } else {
      _finalizeBubble(text, itemId: itemId, turnGroupId: turnGroupId,
          timestamp: timestamp);
    }
  }

  /// Append streaming tokens to the current bubble, splitting <think> blocks
  /// into separate segments in real time.
  void _appendStreamingText(
    ConversationMessage bubble,
    String token, {
    required String itemId,
  }) {
    for (int i = 0; i < token.length; i++) {
      final ch = token[i];
      _tagBuf += ch;

      if (_inThinkBlock) {
        // Inside <think> — accumulate into the thinking segment.
        _lastThinkingSegment(bubble, itemId: itemId).text += ch;
        // Check for </think> closing tag.
        if (_tagBuf.endsWith('</think>')) {
          // Remove the closing tag text from the segment content.
          final seg = _lastThinkingSegment(bubble, itemId: itemId);
          seg.text = seg.text.substring(0, seg.text.length - '</think>'.length);
          _inThinkBlock = false;
          _tagBuf = '';
        }
      } else {
        // Outside <think> — accumulate into the text segment.
        _lastTextSegment(bubble, itemId: itemId).text += ch;
        // Check for <think> opening tag.
        if (_tagBuf.endsWith('<think>')) {
          // Remove the opening tag text from the segment content.
          final seg = _lastTextSegment(bubble, itemId: itemId);
          seg.text = seg.text.substring(0, seg.text.length - '<think>'.length);
          // If the text segment is now empty, remove it.
          if (seg.text.isEmpty) {
            bubble.segments.remove(seg);
          }
          // Start a new thinking segment.
          bubble.segments.add(BubbleSegment.thinking('', itemId: itemId));
          _inThinkBlock = true;
          _tagBuf = '';
        }
      }
    }
  }

  BubbleSegment _lastTextSegment(
    ConversationMessage bubble, {
    required String itemId,
  }) {
    if (bubble.segments.isNotEmpty &&
        bubble.segments.last.kind == SegmentKind.text &&
        bubble.segments.last.itemId == itemId) {
      return bubble.segments.last;
    }
    final seg = BubbleSegment.text('', itemId: itemId);
    bubble.segments.add(seg);
    return seg;
  }

  BubbleSegment _lastThinkingSegment(
    ConversationMessage bubble, {
    required String itemId,
  }) {
    if (bubble.segments.isNotEmpty &&
        bubble.segments.last.kind == SegmentKind.thinking &&
        bubble.segments.last.itemId == itemId) {
      return bubble.segments.last;
    }
    final seg = BubbleSegment.thinking('', itemId: itemId);
    bubble.segments.add(seg);
    return seg;
  }

  // ── Final response handling ────────────────────────────────────────────

  void _finalizeBubble(
    String finalText, {
    required String itemId,
    required String turnGroupId,
    DateTime? timestamp,
  }) {
    final bubble = _ensureAssistantBubble(turnGroupId: turnGroupId,
        timestamp: timestamp);
    bubble.isStreaming = false;

    _replaceTextSegments(bubble, itemId: itemId, finalText: finalText);

    if (_fallbackStreamingIndex >= 0 &&
        _fallbackStreamingIndex < _messages.length &&
        identical(_messages[_fallbackStreamingIndex], bubble)) {
      _fallbackStreamingIndex = -1;
    }
    _inThinkBlock = false;
    _tagBuf = '';
  }

  /// Replace only the text segments emitted for the same assistant stream item,
  /// preserving every other segment in place.
  void _replaceTextSegments(
    ConversationMessage bubble, {
    required String itemId,
    required String finalText,
  }) {
    final replacement = <BubbleSegment>[];
    bool inserted = false;

    for (final seg in bubble.segments) {
      final provisionalTextFromEarlierStream =
          seg.kind == SegmentKind.text &&
          seg.itemId != null &&
          seg.itemId != itemId &&
          !bubble.finalizedTextItemIds.contains(seg.itemId);
      if (provisionalTextFromEarlierStream) {
        continue;
      }

      final sameStreamText =
          seg.kind == SegmentKind.text && seg.itemId == itemId;
      if (sameStreamText) {
        if (!inserted && finalText.isNotEmpty) {
          replacement.add(BubbleSegment.text(finalText, itemId: itemId));
        }
        inserted = true;
        continue;
      }
      replacement.add(seg);
    }

    if (!inserted && finalText.isNotEmpty) {
      replacement.add(BubbleSegment.text(finalText, itemId: itemId));
    }

    bubble.segments
      ..clear()
      ..addAll(replacement);
    bubble.finalizedTextItemIds.add(itemId);
  }

  // ── Tool call / result handling ────────────────────────────────────────

  void _handleToolCall(
    Map<String, dynamic> payload, {
    required String itemId,
    required String turnGroupId,
    DateTime? timestamp,
  }) {
    final bubble = _ensureAssistantBubble(turnGroupId: turnGroupId,
        timestamp: timestamp);
    bubble.isStreaming = false;
    final toolCalls = payload['tool_calls'] as List<dynamic>? ?? [];
    for (final tc in toolCalls) {
      if (tc is Map<String, dynamic>) {
        final fn = tc['function'] as Map<String, dynamic>? ?? {};
        bubble.segments.add(
          BubbleSegment.toolCall({
            'id': tc['id'] ?? '',
            'name': fn['name'] ?? 'tool',
            'arguments': fn['arguments'],
          }, itemId: itemId),
        );
      }
    }
  }

  void _handleToolResult(
    Map<String, dynamic> payload, {
    required String itemId,
    required String turnGroupId,
    DateTime? timestamp,
  }) {
    final bubble = _ensureAssistantBubble(turnGroupId: turnGroupId,
        timestamp: timestamp);
    final content = payload['content'];
    bool success = false;
    if (content is Map<String, dynamic>) {
      success = content['success'] as bool? ?? false;
    }
    bubble.segments.add(
      BubbleSegment.toolResult({
        'tool_name': payload['tool_name'] ?? '',
        'tool_call_id': payload['tool_call_id'] ?? '',
        'success': success,
        'result': content,
      }, itemId: itemId),
    );
  }

  // ── Helpers ────────────────────────────────────────────────────────────

  ConversationMessage _ensureAssistantBubble({
    required String turnGroupId,
    bool streaming = false,
    DateTime? timestamp,
  }) {
    if (turnGroupId.isNotEmpty) {
      final existingIndex = _messages.indexWhere(
        (m) => m.role == 'assistant' && m.turnGroupId == turnGroupId,
      );
      if (existingIndex >= 0) {
        final bubble = _messages[existingIndex];
        if (streaming) {
          bubble.isStreaming = true;
        }
        return bubble;
      }
    }

    if (turnGroupId.isEmpty &&
        _fallbackStreamingIndex >= 0 &&
        _fallbackStreamingIndex < _messages.length) {
      final bubble = _messages[_fallbackStreamingIndex];
      if (streaming) {
        bubble.isStreaming = true;
      }
      return bubble;
    }

    _messages.add(
      ConversationMessage(
        role: 'assistant',
        timestamp: timestamp ?? DateTime.now(),
        turnGroupId: turnGroupId.isEmpty ? null : turnGroupId,
        isStreaming: streaming,
      ),
    );
    if (turnGroupId.isEmpty) {
      _fallbackStreamingIndex = _messages.length - 1;
    }
    _inThinkBlock = false;
    _tagBuf = '';
    return _messages.last;
  }

  void _closeStreamingAssistantBubbles() {
    for (final message in _messages) {
      if (message.role == 'assistant' && message.isStreaming) {
        message.isStreaming = false;
      }
    }
    _fallbackStreamingIndex = -1;
    _inThinkBlock = false;
    _tagBuf = '';
  }

  /// Legacy callback adapter.
  void onOutputChunk(String text, bool isPartial) {
    onConversationItem(text, isPartial);
  }

  void onToolActivity(int kind, String detail) {
    // Not used for bubble construction — kept for debug panel.
  }

  void clearMessages() {
    _messages.clear();
    _fallbackStreamingIndex = -1;
    _batchMode = false;
    _inThinkBlock = false;
    _tagBuf = '';
    notifyListeners();
  }

  void _scrollToBottom({bool animate = false}) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!scrollController.hasClients) return;
      // ListView is reversed: offset 0 is the bottom (newest messages).
      if (animate) {
        scrollController.animateTo(
          0.0,
          duration: const Duration(milliseconds: 200),
          curve: Curves.easeOut,
        );
      } else {
        scrollController.jumpTo(0.0);
      }
    });
  }

  @override
  void dispose() {
    scrollController.dispose();
    super.dispose();
  }
}
