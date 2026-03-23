import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/message.dart';
import '../providers/app_provider.dart';

class ChatBubble extends StatelessWidget {
  final ChatMessage message;
  const ChatBubble({super.key, required this.message});

  @override
  Widget build(BuildContext context) {
    if (message.isToolCall) return _ToolCallChip(message: message);
    if (message.isUser) return _UserBubble(message: message);
    return _AssistantBubble(message: message);
  }
}

// ─── User bubble (right-aligned) ─────────────────────────────────────────────

class _UserBubble extends StatelessWidget {
  final ChatMessage message;
  const _UserBubble({required this.message});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.only(bottom: 16, left: 72),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.end,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          Flexible(
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
              decoration: BoxDecoration(
                color: cs.primary,
                borderRadius: const BorderRadius.only(
                  topLeft: Radius.circular(18),
                  topRight: Radius.circular(18),
                  bottomLeft: Radius.circular(18),
                  bottomRight: Radius.circular(4),
                ),
              ),
              child: Text(
                message.content,
                style: TextStyle(
                  color: cs.onPrimary,
                  height: 1.55,
                  fontSize: 14,
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

// ─── Assistant bubble (left-aligned, with avatar) ─────────────────────────────

class _AssistantBubble extends StatelessWidget {
  final ChatMessage message;
  const _AssistantBubble({required this.message});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.only(bottom: 16, right: 72),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Avatar
          Container(
            width: 30,
            height: 30,
            margin: const EdgeInsets.only(right: 10, top: 2),
            decoration: BoxDecoration(
              color: cs.primaryContainer,
              shape: BoxShape.circle,
            ),
            child: Icon(
              Icons.auto_awesome_rounded,
              size: 15,
              color: cs.onPrimaryContainer,
            ),
          ),
          // Bubble
          Flexible(
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
              decoration: BoxDecoration(
                color: cs.surfaceContainerHigh,
                borderRadius: const BorderRadius.only(
                  topLeft: Radius.circular(4),
                  topRight: Radius.circular(18),
                  bottomLeft: Radius.circular(18),
                  bottomRight: Radius.circular(18),
                ),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min,
                children: [
                  message.isStreaming
                      ? Row(
                          mainAxisSize: MainAxisSize.min,
                          crossAxisAlignment: CrossAxisAlignment.end,
                          children: [
                            Flexible(
                              child: Text(
                                message.content,
                                style: TextStyle(
                                  color: cs.onSurface,
                                  height: 1.55,
                                  fontSize: 14,
                                ),
                              ),
                            ),
                            const SizedBox(width: 2),
                            _BlinkingCursor(),
                          ],
                        )
                      : Text(
                          message.content,
                          style: TextStyle(
                            color: cs.onSurface,
                            height: 1.55,
                            fontSize: 14,
                          ),
                        ),
                  // Speaker button (only when not streaming)
                  if (!message.isStreaming && message.content.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 6),
                      child: InkWell(
                        borderRadius: BorderRadius.circular(12),
                        onTap: () => context
                            .read<AppProvider>()
                            .speakMessage(message.content),
                        child: Padding(
                          padding: const EdgeInsets.symmetric(
                              horizontal: 4, vertical: 2),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(Icons.volume_up_rounded,
                                  size: 14,
                                  color: cs.onSurface.withOpacity(0.4)),
                              const SizedBox(width: 4),
                              Text(
                                '播放',
                                style: TextStyle(
                                  fontSize: 11,
                                  color: cs.onSurface.withOpacity(0.4),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

// ─── Tool call chip (inline, compact) ────────────────────────────────────────

class _ToolCallChip extends StatelessWidget {
  final ChatMessage message;
  const _ToolCallChip({required this.message});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.only(bottom: 10, left: 40),
      child: Row(
        children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
            decoration: BoxDecoration(
              color: cs.secondaryContainer.withOpacity(0.45),
              borderRadius: BorderRadius.circular(20),
              border: Border.all(color: cs.secondary.withOpacity(0.35)),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.build_circle_outlined,
                    size: 13, color: cs.secondary),
                const SizedBox(width: 5),
                Text(
                  message.toolName ?? '工具调用',
                  style: TextStyle(
                    fontSize: 12,
                    color: cs.secondary,
                    fontWeight: FontWeight.w500,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

// ─── Blinking cursor animation ────────────────────────────────────────────────

class _BlinkingCursor extends StatefulWidget {
  @override
  State<_BlinkingCursor> createState() => _BlinkingCursorState();
}

class _BlinkingCursorState extends State<_BlinkingCursor>
    with SingleTickerProviderStateMixin {
  late final AnimationController _ctrl;

  @override
  void initState() {
    super.initState();
    _ctrl = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 550),
    )..repeat(reverse: true);
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return FadeTransition(
      opacity: _ctrl,
      child: Text(
        '▌',
        style: TextStyle(
          color: Theme.of(context).colorScheme.primary,
          fontSize: 14,
        ),
      ),
    );
  }
}
