import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/app_provider.dart';
import '../widgets/agent_state_badge.dart';
import '../widgets/conversation_view.dart';
import '../widgets/debug_panel.dart';
import '../widgets/session_sidebar.dart';

class MainLayout extends StatelessWidget {
  const MainLayout({super.key});

  @override
  Widget build(BuildContext context) {
    final prov = context.watch<AppProvider>();
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(
        elevation: 0,
        scrolledUnderElevation: 1,
        leading: Tooltip(
          message: prov.showSidebar ? '折叠历史栏' : '展开历史栏',
          child: IconButton(
            icon: Icon(
              prov.showSidebar
                  ? Icons.menu_open_rounded
                  : Icons.menu_rounded,
            ),
            onPressed: prov.toggleSidebar,
          ),
        ),
        title: Row(
          children: [
            const Text(
              'Shizuru',
              style: TextStyle(fontWeight: FontWeight.w600, letterSpacing: 0.5),
            ),
            const SizedBox(width: 14),
            AgentStateBadge(state: prov.agentState),
          ],
        ),
        actions: [
          // TTS toggle
          Tooltip(
            message: prov.enableTts ? '关闭语音回复' : '开启语音回复',
            child: IconButton(
              icon: Icon(
                prov.enableTts
                    ? Icons.volume_up_rounded
                    : Icons.volume_off_rounded,
                color: prov.enableTts
                    ? theme.colorScheme.primary
                    : null,
              ),
              onPressed: prov.toggleTts,
            ),
          ),
          // Debug panel toggle
          Tooltip(
            message: prov.showDebugPanel ? '关闭调试面板' : '打开调试面板',
            child: IconButton(
              icon: Icon(
                Icons.developer_mode_rounded,
                color: prov.showDebugPanel
                    ? theme.colorScheme.primary
                    : null,
              ),
              onPressed: prov.toggleDebugPanel,
            ),
          ),
          // Settings
          Tooltip(
            message: '设置',
            child: IconButton(
              icon: const Icon(Icons.settings_outlined),
              onPressed: () => Navigator.pushNamed(context, '/settings'),
            ),
          ),
          const SizedBox(width: 6),
        ],
      ),
      body: Row(
        children: [
          // ── Left: session history sidebar ─────────────────────────
          AnimatedSize(
            duration: const Duration(milliseconds: 220),
            curve: Curves.easeInOut,
            child: SizedBox(
              width: prov.showSidebar ? 248 : 0,
              child: const SessionSidebar(),
            ),
          ),
          if (prov.showSidebar)
            VerticalDivider(width: 1, color: theme.dividerColor),

          // ── Center: conversation ──────────────────────────────────
          const Expanded(child: ConversationView()),

          // ── Right: debug panel ────────────────────────────────────
          if (prov.showDebugPanel)
            VerticalDivider(width: 1, color: theme.dividerColor),
          AnimatedSize(
            duration: const Duration(milliseconds: 220),
            curve: Curves.easeInOut,
            child: SizedBox(
              width: prov.showDebugPanel ? 300 : 0,
              child: const DebugPanel(),
            ),
          ),
        ],
      ),
    );
  }
}
