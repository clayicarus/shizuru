import 'package:flutter/material.dart';
import '../bridge/agent_state.dart';

class StateIndicator extends StatefulWidget {
  final AgentState state;
  const StateIndicator({super.key, required this.state});

  @override
  State<StateIndicator> createState() => _StateIndicatorState();
}

class _StateIndicatorState extends State<StateIndicator>
    with SingleTickerProviderStateMixin {
  late AnimationController _controller;

  @override
  void initState() {
    super.initState();
    _controller = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1000),
    );
    _updateAnimation();
  }

  @override
  void didUpdateWidget(StateIndicator oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.state != widget.state) _updateAnimation();
  }

  void _updateAnimation() {
    switch (widget.state) {
      case AgentState.listening:
        _controller.repeat(reverse: true);
        break;
      case AgentState.thinking:
      case AgentState.routing:
      case AgentState.acting:
        _controller.repeat();
        break;
      default:
        _controller.stop();
        _controller.value = 0;
    }
  }

  Color _dotColor() {
    switch (widget.state) {
      case AgentState.idle:
      case AgentState.terminated:
        return Colors.grey;
      case AgentState.listening:
        return Colors.green;
      case AgentState.thinking:
      case AgentState.routing:
        return Colors.amber;
      case AgentState.acting:
        return Colors.blue;
      case AgentState.responding:
        return Colors.teal;
      case AgentState.error:
        return Colors.red;
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final color = _dotColor();
    final label = widget.state.displayName;

    Widget dot = Container(
      width: 10,
      height: 10,
      decoration: BoxDecoration(color: color, shape: BoxShape.circle),
    );

    if (widget.state == AgentState.listening) {
      dot = FadeTransition(opacity: _controller, child: dot);
    } else if (widget.state == AgentState.thinking ||
        widget.state == AgentState.routing ||
        widget.state == AgentState.acting) {
      dot = RotationTransition(turns: _controller, child: dot);
    }

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        dot,
        const SizedBox(width: 6),
        Text(label, style: Theme.of(context).textTheme.labelMedium),
      ],
    );
  }
}
