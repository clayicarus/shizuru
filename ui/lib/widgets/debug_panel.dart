import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../bridge/agent_state.dart';
import '../providers/agent_provider.dart';

class _TransitionEntry {
  final DateTime time;
  final AgentState from;
  final AgentState to;
  _TransitionEntry(this.time, this.from, this.to);
}

class DebugPanel extends StatefulWidget {
  const DebugPanel({super.key});

  @override
  State<DebugPanel> createState() => _DebugPanelState();
}

class _DebugPanelState extends State<DebugPanel> {
  final List<_TransitionEntry> _transitions = [];
  AgentState? _lastState;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final current = context.watch<AgentProvider>().state;
    if (_lastState != null && _lastState != current) {
      _transitions.add(_TransitionEntry(DateTime.now(), _lastState!, current));
    }
    _lastState = current;
  }

  String _fmt(DateTime t) =>
      '${t.hour.toString().padLeft(2, '0')}:'
      '${t.minute.toString().padLeft(2, '0')}:'
      '${t.second.toString().padLeft(2, '0')}.'
      '${t.millisecond.toString().padLeft(3, '0')}';

  @override
  Widget build(BuildContext context) {
    final state = context.watch<AgentProvider>().state;

    return Drawer(
      child: SafeArea(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 8, 0),
              child: Row(
                children: [
                  const Text('Debug Panel',
                      style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                  const Spacer(),
                  TextButton(
                    onPressed: () => setState(() => _transitions.clear()),
                    child: const Text('Clear'),
                  ),
                ],
              ),
            ),
            const Divider(),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
              child: Text('State: ${state.displayName}',
                  style: const TextStyle(fontWeight: FontWeight.w500)),
            ),
            const Divider(),
            const Padding(
              padding: EdgeInsets.symmetric(horizontal: 16, vertical: 4),
              child: Text('Transitions',
                  style: TextStyle(fontWeight: FontWeight.w500)),
            ),
            Expanded(
              child: ListView.builder(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                itemCount: _transitions.length,
                itemBuilder: (ctx, i) {
                  final e = _transitions[_transitions.length - 1 - i];
                  return Text(
                    '${_fmt(e.time)}  ${e.from.displayName} → ${e.to.displayName}',
                    style: const TextStyle(fontSize: 12, fontFamily: 'monospace'),
                  );
                },
              ),
            ),
            const Divider(),
            const Padding(
              padding: EdgeInsets.fromLTRB(16, 4, 16, 8),
              child: Text('Tool Calls: (none)',
                  style: TextStyle(fontSize: 12, color: Colors.grey)),
            ),
            const Padding(
              padding: EdgeInsets.fromLTRB(16, 0, 16, 12),
              child: Text('Tokens: prompt: —   completion: —',
                  style: TextStyle(fontSize: 12, color: Colors.grey)),
            ),
          ],
        ),
      ),
    );
  }
}
