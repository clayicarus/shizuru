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
    // Start/stop dots animation when streaming state changes.
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

  @override
  Widget build(BuildContext context) {
    final isUser = widget.message.role == 'user';
    final isStreaming = widget.message.isStreaming;
    final hasText = widget.message.text.isNotEmpty;

    final bubbleColor = isUser
        ? Theme.of(context).colorScheme.primaryContainer
        : Theme.of(context).colorScheme.secondaryContainer;

    final timeStr =
        '${widget.message.timestamp.hour.toString().padLeft(2, '0')}:'
        '${widget.message.timestamp.minute.toString().padLeft(2, '0')}:'
        '${widget.message.timestamp.second.toString().padLeft(2, '0')}';

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
            else
              Text(widget.message.text),
          ],
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
