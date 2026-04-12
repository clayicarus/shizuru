import 'dart:convert';
import 'package:flutter/material.dart';
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

    final bubbleColor = isUser
        ? Theme.of(context).colorScheme.primaryContainer
        : Theme.of(context).colorScheme.secondaryContainer;

    final timeStr = _formatTime(widget.message.timestamp);

    return Align(
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
            if (isStreaming && !hasText)
              _DotsIndicator(controller: _dotsController)
            else if (!isUser)
              ..._buildAssistantContent(context)
            else
              Text(widget.message.text),
          ],
        ),
      ),
    );
  }

  // Parse assistant text into segments: plain text, <think>, <tool_call>, <tool_result>.
  static final _segmentRegex = RegExp(
    r'<think>(.*?)</think>|<tool_call>(.*?)</tool_call>|<tool_result>(.*?)</tool_result>',
    dotAll: true,
  );

  List<Widget> _buildAssistantContent(BuildContext context) {
    final text = widget.message.text;
    final widgets = <Widget>[];
    int lastEnd = 0;

    for (final match in _segmentRegex.allMatches(text)) {
      // Plain text before this match.
      if (match.start > lastEnd) {
        final plain = text.substring(lastEnd, match.start).trim();
        if (plain.isNotEmpty) {
          widgets.add(Text(plain));
          widgets.add(const SizedBox(height: 4));
        }
      }
      lastEnd = match.end;

      if (match.group(1) != null) {
        // <think> block
        widgets.add(_buildThinkingBlock(context, match.group(1)!));
        widgets.add(const SizedBox(height: 4));
      } else if (match.group(2) != null) {
        // <tool_call> block
        widgets.add(_buildToolCallInline(context, match.group(2)!));
        widgets.add(const SizedBox(height: 4));
      } else if (match.group(3) != null) {
        // <tool_result> block
        widgets.add(_buildToolResultInline(context, match.group(3)!));
        widgets.add(const SizedBox(height: 4));
      }
    }

    // Remaining plain text after last match.
    if (lastEnd < text.length) {
      final plain = text.substring(lastEnd).trim();
      if (plain.isNotEmpty) {
        widgets.add(Text(plain));
      }
    }

    // If no segments matched at all, just show the raw text.
    if (widgets.isEmpty && text.isNotEmpty) {
      widgets.add(Text(text));
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

  Widget _buildToolCallInline(BuildContext context, String jsonStr) {
    String name = 'tool';
    String args = '';
    try {
      final json = jsonDecode(jsonStr) as Map<String, dynamic>;
      name = json['name'] as String? ?? 'tool';
      args = json['arguments']?.toString() ?? '';
    } catch (_) {}

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

  Widget _buildToolResultInline(BuildContext context, String jsonStr) {
    bool success = false;
    String output = '';
    try {
      final json = jsonDecode(jsonStr) as Map<String, dynamic>;
      success = json['success'] as bool? ?? false;
      output = json['output']?.toString() ?? '';
    } catch (_) {}

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
