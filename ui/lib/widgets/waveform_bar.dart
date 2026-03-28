import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../bridge/agent_state.dart';
import '../providers/agent_provider.dart';

class WaveformBar extends StatelessWidget {
  const WaveformBar({super.key});

  @override
  Widget build(BuildContext context) {
    final agent = context.watch<AgentProvider>();
    final isListening = agent.state == AgentState.listening;
    final level = isListening ? agent.audioLevel.clamp(0.0, 1.0) : 0.0;

    return SizedBox(
      height: 24,
      child: LayoutBuilder(
        builder: (context, constraints) {
          final maxWidth = constraints.maxWidth;
          return Stack(
            alignment: Alignment.centerLeft,
            children: [
              Container(
                height: 4,
                width: maxWidth,
                decoration: BoxDecoration(
                  color: Theme.of(context).colorScheme.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(2),
                ),
              ),
              AnimatedContainer(
                duration: const Duration(milliseconds: 80),
                height: 4,
                width: maxWidth * level,
                decoration: BoxDecoration(
                  color: Theme.of(context).colorScheme.primary,
                  borderRadius: BorderRadius.circular(2),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}
