import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../providers/conversation_provider.dart';

class MessageBubble extends StatefulWidget {
  final ConversationMessage message;
  const MessageBubble({super.key, required this.message});

  @override
  State<MessageBubble> createState() => _MessageBubbleState();
}

class _MessageBubbleState extends State<MessageBubble>
    with SingleTickerProviderStateMixin {
  late AnimationController _dotsController;
  bool _thinkingExpanded = false;

  @override
  void initState() {
    super.initState();
    _dotsController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 900),
    );
    if (widget.message.isStreaming) _dotsController.repeat();
  }

  @override
  void didUpdateWidget(MessageBubble oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.message.isStreaming && !_dotsController.isAnimating) {
      _dotsController.repeat();
    } else if (!widget.message.isStreaming && _dotsController.isAnimating) {
      _dotsController.stop();
    }
  }

  @override
  void dispose() {
    _dotsController.dispose();
    super.dispose();
  }

  String _formatTime(DateTime t) =>
      '${t.hour.toString().padLeft(2, '0')}:'
      '${t.minute.toString().padLeft(2, '0')}:'
      '${t.second.toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context) {
    final isUser = widget.message.role == 'user';
    final isStreaming = widget.message.isStreaming;
    final hasText = widget.message.text.isNotEmpty;
    final hasEvents = widget.message.events.isNotEmpty;

    final bubbleColor = isUser
        ? Theme.of(context).colorScheme.primaryContainer
        : Theme.of(context).colorScheme.secondaryContainer;

    final timeStr = _formatTime(widget.message.timestamp);

    return GestureDetector(
      onLongPress: () => _copyToClipboard(context),
      child: Align(
        alignment: isUser ? Alignment.centerRight : Alignment.centerLeft,
        child: Container(
          margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
          constraints: BoxConstraints(
            maxWidth: MediaQuery.of(context).size.width * 0.75,
          ),
          decoration: BoxDecoration(
            color: bubbleColor,
            borderRadius: BorderRadius.circular(12),
          ),
          child: Column(
            crossAxisAlignment:
                isUser ? CrossAxisAlignment.end : CrossAxisAlignment.start,
            children: [
              Text(
                '${isUser ? 'you' : 'assistant'}  $timeStr',
                style: Theme.of(context).textTheme.labelSmall,
              ),
            const SizedBox(height: 4),
            if (isStreaming && !hasText && !hasEvents)
              _DotsIndicator(controller: _dotsController)
            else if (!isUser)
              ..._buildAssistantContent(context)
            else
              Text(widget.message.text),
          ],
        ),
      ),
    ),
    );
  }

  void _copyToClipboard(BuildContext context) {
    final text = widget.message.text
        .replaceAll(RegExp(r'<think>.*?</think>', dotAll: true), '')
        .trim();
    if (text.isEmpty) { return; }
    Clipboard.setData(ClipboardData(text: text));
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Copied'), duration: Duration(seconds: 1)),
    );
  }

  // Parse assistant text into segments: plain text and <think>.
  static final _segmentRegex = RegExp(
    r'<think>(.*?)</think>',
    dotAll: true,
  );

  List<Widget> _buildAssistantContent(BuildContext context) {
    final widgets = <Widget>[];

    for (final event in widget.message.events) {
      switch (event.kind) {
        case ConversationToolEventKind.toolCall:
          widgets.add(_buildToolCallInline(context, event.data));
          widgets.add(const SizedBox(height: 4));
          break;
        case ConversationToolEventKind.toolResult:
          widgets.add(_buildToolResultInline(context, event.data));
          widgets.add(const SizedBox(height: 4));
          break;
      }
    }

    widgets.addAll(_buildAssistantTextContent(context, widget.message.text));
    if (widgets.isNotEmpty && widgets.last is SizedBox) {
      widgets.removeLast();
    }

    return widgets;
  }

  List<Widget> _buildAssistantTextContent(BuildContext context, String text) {
    final widgets = <Widget>[];
    int lastEnd = 0;

    for (final match in _segmentRegex.allMatches(text)) {
      if (match.start > lastEnd) {
        final plain = text.substring(lastEnd, match.start).trim();
        if (plain.isNotEmpty) {
          widgets.add(Text(plain));
          widgets.add(const SizedBox(height: 4));
        }
      }
      lastEnd = match.end;

      if (match.group(1) != null) {
        widgets.add(_buildThinkingBlock(context, match.group(1)!));
        widgets.add(const SizedBox(height: 4));
      }
    }

    if (lastEnd < text.length) {
      final plain = text.substring(lastEnd).trim();
      if (plain.isNotEmpty) {
        widgets.add(Text(plain));
        widgets.add(const SizedBox(height: 4));
      }
    }

    if (widgets.isEmpty && text.isNotEmpty) {
      widgets.add(Text(text));
      widgets.add(const SizedBox(height: 4));
    }

    return widgets;
  }

  Widget _buildThinkingBlock(BuildContext context, String content) {
    return GestureDetector(
      onTap: () => setState(() => _thinkingExpanded = !_thinkingExpanded),
      child: Container(
        width: double.infinity,
        padding: const EdgeInsets.all(8),
        decoration: BoxDecoration(
          color: Colors.grey.withValues(alpha: 0.15),
          borderRadius: BorderRadius.circular(8),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  _thinkingExpanded ? Icons.expand_less : Icons.expand_more,
                  size: 16,
                ),
                const SizedBox(width: 4),
                Text(
                  'Thinking...',
                  style: Theme.of(context).textTheme.labelSmall?.copyWith(
                        fontStyle: FontStyle.italic,
                      ),
                ),
              ],
            ),
            if (_thinkingExpanded) ...[
              const SizedBox(height: 4),
              Text(
                content.trim(),
                style: Theme.of(context)
                    .textTheme
                    .bodySmall
                    ?.copyWith(color: Colors.grey[600]),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildToolCallInline(BuildContext context, Map<String, dynamic> json) {
    final String name = json['name'] as String? ?? 'tool';
    final dynamic argsValue = json['arguments'];
    final String args = _formatValue(argsValue);

    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(8),
      decoration: BoxDecoration(
        color: Colors.blue.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: Colors.blue.withValues(alpha: 0.2)),
      ),
      child: Row(
        children: [
          const Icon(Icons.build_outlined, size: 14, color: Colors.blue),
          const SizedBox(width: 6),
          Expanded(
            child: Text(
              name + (args.isNotEmpty && args != '{}' ? '($args)' : ''),
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: Colors.blue[700],
                    fontFamily: 'monospace',
                  ),
              maxLines: 2,
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildToolResultInline(BuildContext context, Map<String, dynamic> json) {
    final bool success = json['success'] as bool? ?? false;
    final dynamic result = json['result'];
    String output = '';
    if (result is Map<String, dynamic>) {
      if (result.containsKey('output')) {
        output = _formatValue(result['output']);
      } else if (result.containsKey('error')) {
        output = _formatValue(result['error']);
      } else {
        output = _formatValue(result);
      }
    } else {
      output = _formatValue(result);
    }

    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(8),
      decoration: BoxDecoration(
        color: (success ? Colors.green : Colors.red).withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(
          color: (success ? Colors.green : Colors.red).withValues(alpha: 0.2),
        ),
      ),
      child: Row(
        children: [
          Icon(
            success ? Icons.check_circle_outline : Icons.cancel_outlined,
            size: 14,
            color: success ? Colors.green : Colors.red,
          ),
          const SizedBox(width: 6),
          Expanded(
            child: Text(
              output.isNotEmpty ? output : (success ? 'Done' : 'Failed'),
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    fontFamily: 'monospace',
                  ),
              maxLines: 3,
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ],
      ),
    );
  }

  String _formatValue(dynamic value) {
    if (value == null) { return ''; }
    if (value is String) { return value; }
    return const JsonEncoder().convert(value);
  }
}

class _DotsIndicator extends AnimatedWidget {
  const _DotsIndicator({required AnimationController controller})
      : super(listenable: controller);

  @override
  Widget build(BuildContext context) {
    final t = (listenable as AnimationController).value;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: List.generate(3, (i) {
        final opacity = ((t * 3 - i) % 1.0).clamp(0.2, 1.0);
        return Padding(
          padding: const EdgeInsets.symmetric(horizontal: 2),
          child: Opacity(
            opacity: opacity,
            child: const CircleAvatar(radius: 4),
          ),
        );
      }),
    );
  }
}
