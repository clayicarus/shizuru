import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/agent_state.dart';
import '../providers/app_provider.dart';
import 'chat_bubble.dart';
import 'input_bar.dart';

class ConversationView extends StatefulWidget {
  const ConversationView({super.key});

  @override
  State<ConversationView> createState() => _ConversationViewState();
}

class _ConversationViewState extends State<ConversationView> {
  final _scrollCtrl = ScrollController();

  @override
  void dispose() {
    _scrollCtrl.dispose();
    super.dispose();
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollCtrl.hasClients &&
          _scrollCtrl.position.maxScrollExtent > 0) {
        _scrollCtrl.animateTo(
          _scrollCtrl.position.maxScrollExtent,
          duration: const Duration(milliseconds: 220),
          curve: Curves.easeOut,
        );
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final prov = context.watch<AppProvider>();
    final msgs = prov.currentSession?.messages ?? [];

    if (msgs.isNotEmpty) _scrollToBottom();

    return Column(
      children: [
        // Message list
        Expanded(
          child: msgs.isEmpty
              ? _WelcomeScreen()
              : ListView.builder(
                  controller: _scrollCtrl,
                  padding: const EdgeInsets.fromLTRB(28, 20, 28, 8),
                  itemCount: msgs.length,
                  itemBuilder: (_, i) => ChatBubble(message: msgs[i]),
                ),
        ),
        // Thinking indicator
        if (_showThinking(prov.agentState)) const _ThinkingIndicator(),
        // Input bar
        const InputBar(),
      ],
    );
  }

  bool _showThinking(AgentState s) =>
      s == AgentState.thinking || s == AgentState.routing;
}

// ─── Welcome screen shown when session is empty ───────────────────────────────

class _WelcomeScreen extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 72,
            height: 72,
            decoration: BoxDecoration(
              color: cs.primaryContainer.withOpacity(0.3),
              shape: BoxShape.circle,
            ),
            child: Icon(
              Icons.auto_awesome_rounded,
              size: 36,
              color: cs.primary.withOpacity(0.6),
            ),
          ),
          const SizedBox(height: 20),
          Text(
            'Shizuru',
            style: tt.headlineMedium?.copyWith(
              color: cs.onSurface.withOpacity(0.5),
              fontWeight: FontWeight.w300,
              letterSpacing: 2,
            ),
          ),
          const SizedBox(height: 8),
          Text(
            '有什么我可以帮你的？',
            style: tt.bodyMedium?.copyWith(
              color: cs.onSurface.withOpacity(0.3),
            ),
          ),
          const SizedBox(height: 40),
          // Suggestion chips
          Wrap(
            spacing: 8,
            runSpacing: 8,
            alignment: WrapAlignment.center,
            children: const [
              _SuggestionChip('帮我写一段代码'),
              _SuggestionChip('解释一个概念'),
              _SuggestionChip('语音对话'),
            ],
          ),
        ],
      ),
    );
  }
}

class _SuggestionChip extends StatelessWidget {
  final String label;
  const _SuggestionChip(this.label);

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return ActionChip(
      label: Text(label, style: const TextStyle(fontSize: 13)),
      backgroundColor: cs.surfaceContainerHigh,
      side: BorderSide(color: cs.outline.withOpacity(0.3)),
      onPressed: () =>
          context.read<AppProvider>().sendTextMessage(label),
    );
  }
}

// ─── Animated "thinking" dots shown during kThinking / kRouting ───────────────

class _ThinkingIndicator extends StatefulWidget {
  const _ThinkingIndicator();

  @override
  State<_ThinkingIndicator> createState() => _ThinkingIndicatorState();
}

class _ThinkingIndicatorState extends State<_ThinkingIndicator>
    with SingleTickerProviderStateMixin {
  late final AnimationController _ctrl;

  @override
  void initState() {
    super.initState();
    _ctrl = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1200),
    )..repeat();
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.fromLTRB(28, 0, 28, 8),
      child: Row(
        children: [
          Container(
            width: 30,
            height: 30,
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
          const SizedBox(width: 10),
          _DotsRow(ctrl: _ctrl, color: cs.onSurface.withOpacity(0.4)),
        ],
      ),
    );
  }
}

class _DotsRow extends StatelessWidget {
  final AnimationController ctrl;
  final Color color;
  const _DotsRow({required this.ctrl, required this.color});

  @override
  Widget build(BuildContext context) {
    return Row(
      children: List.generate(3, (i) {
        final begin = i * 0.2;
        final anim = Tween(begin: 0.3, end: 1.0).animate(
          CurvedAnimation(
            parent: ctrl,
            curve: Interval(begin, begin + 0.4, curve: Curves.easeInOut),
          ),
        );
        return Padding(
          padding: const EdgeInsets.symmetric(horizontal: 2),
          child: FadeTransition(
            opacity: anim,
            child: Container(
              width: 7,
              height: 7,
              decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            ),
          ),
        );
      }),
    );
  }
}
