import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/app_provider.dart';
import 'agent_state_badge.dart';

class DebugPanel extends StatelessWidget {
  const DebugPanel({super.key});

  @override
  Widget build(BuildContext context) {
    final prov = context.watch<AppProvider>();
    final theme = Theme.of(context);
    final cs = theme.colorScheme;

    return Container(
      color: cs.surfaceContainerLow,
      child: DefaultTabController(
        length: 3,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            // ── Header ────────────────────────────────────────────────
            Container(
              padding:
                  const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
              decoration: BoxDecoration(
                border: Border(bottom: BorderSide(color: theme.dividerColor)),
              ),
              child: Row(
                children: [
                  Icon(Icons.bug_report_outlined,
                      size: 15, color: cs.primary),
                  const SizedBox(width: 7),
                  Text('调试面板',
                      style: theme.textTheme.labelLarge
                          ?.copyWith(color: cs.primary)),
                ],
              ),
            ),
            // ── State + Token ─────────────────────────────────────────
            Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  AgentStateBadge(state: prov.agentState),
                  const SizedBox(height: 10),
                  _TokenBar(used: prov.tokenUsed, total: 4096),
                ],
              ),
            ),
            Divider(height: 1, color: theme.dividerColor),
            // ── Tabs ──────────────────────────────────────────────────
            TabBar(
              tabAlignment: TabAlignment.fill,
              labelStyle: const TextStyle(fontSize: 12),
              tabs: const [
                Tab(text: '工具调用', height: 34),
                Tab(text: '日志', height: 34),
                Tab(text: '路由表', height: 34),
              ],
            ),
            Divider(height: 1, color: theme.dividerColor),
            // ── Tab content ───────────────────────────────────────────
            Expanded(
              child: TabBarView(
                children: [
                  _ToolCallList(entries: prov.toolCallLog),
                  _LogList(logs: prov.logs),
                  const _RouteTableView(),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ─── Token progress bar ───────────────────────────────────────────────────────

class _TokenBar extends StatelessWidget {
  final int used;
  final int total;
  const _TokenBar({required this.used, required this.total});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final ratio = (used / total).clamp(0.0, 1.0);
    final color = ratio > 0.85
        ? Colors.orange
        : ratio > 0.95
            ? cs.error
            : cs.primary;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text('Token 预算',
                style: Theme.of(context).textTheme.labelSmall),
            Text('$used / $total',
                style: Theme.of(context).textTheme.labelSmall),
          ],
        ),
        const SizedBox(height: 5),
        ClipRRect(
          borderRadius: BorderRadius.circular(3),
          child: LinearProgressIndicator(
            value: ratio,
            minHeight: 6,
            backgroundColor: cs.outline.withOpacity(0.15),
            valueColor: AlwaysStoppedAnimation(color),
          ),
        ),
      ],
    );
  }
}

// ─── Tool call list ───────────────────────────────────────────────────────────

class _ToolCallList extends StatelessWidget {
  final List<String> entries;
  const _ToolCallList({required this.entries});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    if (entries.isEmpty) {
      return Center(
        child: Text('暂无工具调用',
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: cs.onSurface.withOpacity(0.3),
                )),
      );
    }
    return ListView.builder(
      padding: const EdgeInsets.all(10),
      itemCount: entries.length,
      itemBuilder: (_, i) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(
          children: [
            Icon(Icons.check_circle_outline_rounded,
                size: 13, color: cs.secondary),
            const SizedBox(width: 6),
            Expanded(
              child: Text(
                entries[i],
                style: Theme.of(context).textTheme.bodySmall,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ─── Log list (reverse chronological) ────────────────────────────────────────

class _LogList extends StatelessWidget {
  final List<String> logs;
  const _LogList({required this.logs});

  @override
  Widget build(BuildContext context) {
    final style = Theme.of(context).textTheme.bodySmall?.copyWith(
          fontFamily: 'monospace',
          fontSize: 11,
          height: 1.6,
        );
    return ListView.builder(
      reverse: true,
      padding: const EdgeInsets.all(10),
      itemCount: logs.length,
      itemBuilder: (_, i) => Text(logs[logs.length - 1 - i], style: style),
    );
  }
}

// ─── Route table (static for prototype, real data from C++ bridge later) ──────

class _RouteTableView extends StatelessWidget {
  const _RouteTableView();

  static const _routes = [
    ('audio_capture', 'audio_out', 'vad', 'audio_in'),
    ('vad', 'audio_out', 'baidu_asr', 'audio_in'),
    ('vad', 'vad_out', 'vad_event', 'vad_in'),
    ('baidu_asr', 'text_out', 'core', 'text_in'),
    ('core', 'text_out', 'elevenlabs_tts', 'text_in'),
    ('elevenlabs_tts', 'audio_out', 'audio_playout', 'audio_in'),
  ];

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return ListView.builder(
      padding: const EdgeInsets.all(10),
      itemCount: _routes.length,
      itemBuilder: (_, i) {
        final (srcDev, srcPort, dstDev, dstPort) = _routes[i];
        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 4),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.center,
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    Text(srcDev,
                        style: TextStyle(
                            fontSize: 11,
                            fontFamily: 'monospace',
                            color: cs.onSurface)),
                    Text(srcPort,
                        style: TextStyle(
                            fontSize: 10,
                            fontFamily: 'monospace',
                            color: cs.onSurface.withOpacity(0.45))),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Icon(Icons.arrow_forward_rounded,
                    size: 14, color: cs.primary.withOpacity(0.7)),
              ),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(dstDev,
                        style: TextStyle(
                            fontSize: 11,
                            fontFamily: 'monospace',
                            color: cs.onSurface)),
                    Text(dstPort,
                        style: TextStyle(
                            fontSize: 10,
                            fontFamily: 'monospace',
                            color: cs.onSurface.withOpacity(0.45))),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }
}
