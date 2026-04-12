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
    final role = widget.message.role;
    if (role == 'tool_call') return _buildToolCallCard(context);
    if (role == 'tool_result') return const SizedBox.shrink();

    final isUser = role == 'user';
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

  static final _thinkRegex = RegExp(r'<think>(.*?)</think>', dotAll: true);

  List<Widget> _buildAssistantContent(BuildContext context) {
    final text = widget.message.text;
    final matches = _thinkRegex.allMatches(text);
    if (matches.isEmpty) return [Text(text)];

    final thinkingBlocks = matches.map((m) => m.group(1) ?? '').toList();
    final visibleText = text.replaceAll(_thinkRegex, '').trim();
    final widgets = <Widget>[];

    if (thinkingBlocks.isNotEmpty) {
      widgets.add(
        GestureDetector(
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
                      _thinkingExpanded
                          ? Icons.expand_less
                          : Icons.expand_more,
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
                    thinkingBlocks.join('\n'),
                    style: Theme.of(context).textTheme.bodySmall?.copyWith(
                          color: Colors.grey[600],
                        ),
                  ),
                ],
              ],
            ),
          ),
        ),
      );
      if (visibleText.isNotEmpty) widgets.add(const SizedBox(height: 6));
    }

    if (visibleText.isNotEmpty) widgets.add(Text(visibleText));
    return widgets;
  }

  Widget _buildToolCallCard(BuildContext context) {
    final msg = widget.message;
    final hasResult = msg.toolSuccess != null;
    final success = msg.toolSuccess ?? false;

    return Align(
      alignment: Alignment.centerLeft,
      child: Container(
        margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
        constraints: BoxConstraints(
          maxWidth: MediaQuery.of(context).size.width * 0.75,
        ),
        child: Card(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    if (!hasResult)
                      const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    else if (success)
                      const Icon(Icons.check_circle, size: 16, color: Colors.green)
                    else
                      const Icon(Icons.cancel, size: 16, color: Colors.red),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(
                        msg.toolName ?? 'tool',
                        style: const TextStyle(fontWeight: FontWeight.bold),
                      ),
                    ),
                  ],
                ),
                if (msg.toolArguments != null &&
                    msg.toolArguments!.isNotEmpty) ...[
                  const SizedBox(height: 6),
                  ExpansionTile(
                    tilePadding: EdgeInsets.zero,
                    title: Text(
                      'Arguments',
                      style: Theme.of(context).textTheme.labelSmall,
                    ),
                    children: [
                      SizedBox(
                        width: double.infinity,
                        child: Text(
                          msg.toolArguments!,
                          style: const TextStyle(
                            fontFamily: 'monospace',
                            fontSize: 12,
                          ),
                        ),
                      ),
                    ],
                  ),
                ],
                if (hasResult && msg.text.isNotEmpty) ...[
                  const SizedBox(height: 6),
                  Text(
                    msg.text,
                    style: Theme.of(context).textTheme.bodySmall,
                    maxLines: 5,
                    overflow: TextOverflow.ellipsis,
                  ),
                ],
              ],
            ),
          ),
        ),
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
