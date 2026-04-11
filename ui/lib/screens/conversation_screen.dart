import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../providers/agent_provider.dart';
import '../providers/conversation_provider.dart';
import '../widgets/debug_panel.dart';
import '../widgets/message_bubble.dart';
import '../widgets/state_indicator.dart';
import '../widgets/waveform_bar.dart';
import 'settings_screen.dart';

class ConversationScreen extends StatefulWidget {
  const ConversationScreen({super.key});

  @override
  State<ConversationScreen> createState() => _ConversationScreenState();
}

class _ConversationScreenState extends State<ConversationScreen> {
  final TextEditingController _inputController = TextEditingController();
  final GlobalKey<ScaffoldState> _scaffoldKey = GlobalKey<ScaffoldState>();

  @override
  void dispose() {
    _inputController.dispose();
    super.dispose();
  }

  void _sendMessage() {
    final text = _inputController.text.trim();
    if (text.isEmpty) return;
    _inputController.clear();

    final agent = context.read<AgentProvider>();
    final conv = context.read<ConversationProvider>();
    conv.addUserMessage(text);
    agent.sendMessage(text);
  }

  @override
  Widget build(BuildContext context) {
    final agent = context.watch<AgentProvider>();
    final conv = context.watch<ConversationProvider>();
    final messages = conv.messages;

    return Scaffold(
      key: _scaffoldKey,
      appBar: AppBar(
        title: StateIndicator(state: agent.state, activity: agent.activity),
        actions: [
          IconButton(
            icon: const Icon(Icons.bug_report_outlined),
            tooltip: 'Debug',
            onPressed: () => _scaffoldKey.currentState?.openEndDrawer(),
          ),
          IconButton(
            icon: const Icon(Icons.settings_outlined),
            tooltip: 'Settings',
            onPressed: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const SettingsScreen()),
            ),
          ),
        ],
      ),
      endDrawer: const DebugPanel(),
      body: Column(
        children: [
          Expanded(
            child: ListView.builder(
              controller: conv.scrollController,
              padding: const EdgeInsets.symmetric(vertical: 8),
              itemCount: messages.length,
              itemBuilder: (ctx, i) => MessageBubble(
                key: ValueKey('msg_$i'),
                message: messages[i],
              ),
            ),
          ),
          const Divider(height: 1),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
            child: const WaveformBar(),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(8, 0, 8, 8),
            child: Row(
              children: [
                IconButton(
                  icon: Icon(
                    agent.captureActive ? Icons.mic : Icons.mic_none,
                    color: agent.captureActive ? Colors.red : null,
                  ),
                  tooltip: agent.captureActive ? 'Stop mic' : 'Start mic',
                  onPressed: agent.isInitialized
                      ? () => agent.toggleCapture()
                      : null,
                ),
                IconButton(
                  icon: Icon(
                    agent.playoutActive
                        ? Icons.volume_up
                        : Icons.volume_off,
                    color: agent.playoutActive ? Colors.blue : null,
                  ),
                  tooltip: agent.playoutActive
                      ? 'Stop speaker'
                      : 'Start speaker',
                  onPressed: agent.isInitialized
                      ? () => agent.togglePlayout()
                      : null,
                ),
                Expanded(
                  child: TextField(
                    controller: _inputController,
                    decoration: const InputDecoration(
                      hintText: 'Type a message...',
                      border: OutlineInputBorder(),
                      contentPadding:
                          EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                    ),
                    onSubmitted: (_) => _sendMessage(),
                    textInputAction: TextInputAction.send,
                  ),
                ),
                const SizedBox(width: 8),
                ElevatedButton(
                  onPressed: agent.isInitialized ? _sendMessage : null,
                  child: const Text('Send'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
