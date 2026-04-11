import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../bridge/agent_state.dart';
import '../providers/agent_provider.dart';

class DebugPanel extends StatelessWidget {
  const DebugPanel({super.key});

  String _fmt(DateTime t) =>
      '${t.hour.toString().padLeft(2, '0')}:'
      '${t.minute.toString().padLeft(2, '0')}:'
      '${t.second.toString().padLeft(2, '0')}.'
      '${t.millisecond.toString().padLeft(3, '0')}';

  @override
  Widget build(BuildContext context) {
    final agent = context.watch<AgentProvider>();
    final log = agent.activityLog;

    return Drawer(
      child: SafeArea(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 8, 0),
              child: Row(
                children: [
                  const Text('Activity Log',
                      style: TextStyle(
                          fontWeight: FontWeight.bold, fontSize: 16)),
                  const Spacer(),
                  Text(agent.state.displayName,
                      style: Theme.of(context).textTheme.labelMedium),
                ],
              ),
            ),
            const Divider(),
            // Device controls
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
              child: Row(
                children: [
                  const Text('Audio: ',
                      style: TextStyle(fontSize: 12, fontWeight: FontWeight.w500)),
                  TextButton(
                    onPressed: () => agent.startAllAudio(),
                    child: const Text('Start', style: TextStyle(fontSize: 11)),
                  ),
                  TextButton(
                    onPressed: () => agent.stopAllAudio(),
                    child: const Text('Stop', style: TextStyle(fontSize: 11)),
                  ),
                ],
              ),
            ),
            const Divider(),
            Expanded(
              child: ListView.builder(
                padding: const EdgeInsets.symmetric(horizontal: 12),
                itemCount: log.length,
                itemBuilder: (ctx, i) {
                  final e = log[log.length - 1 - i];
                  return Padding(
                    padding: const EdgeInsets.symmetric(vertical: 1),
                    child: Text(
                      '${_fmt(e.time)}  ${e.message}',
                      style: const TextStyle(
                          fontSize: 11, fontFamily: 'monospace'),
                    ),
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}
