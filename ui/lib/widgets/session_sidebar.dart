import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/session.dart';
import '../providers/app_provider.dart';

class SessionSidebar extends StatelessWidget {
  const SessionSidebar({super.key});

  @override
  Widget build(BuildContext context) {
    final prov = context.watch<AppProvider>();
    final theme = Theme.of(context);

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        // Header
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 14, 12, 8),
          child: Text(
            '对话历史',
            style: theme.textTheme.labelLarge?.copyWith(
              color: theme.colorScheme.onSurface.withOpacity(0.5),
              letterSpacing: 0.8,
            ),
          ),
        ),
        // New session button
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
          child: OutlinedButton.icon(
            icon: const Icon(Icons.add_rounded, size: 16),
            label: const Text('新建对话'),
            style: OutlinedButton.styleFrom(
              alignment: Alignment.centerLeft,
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
            ),
            onPressed: prov.newSession,
          ),
        ),
        const SizedBox(height: 4),
        Divider(height: 1, color: theme.dividerColor),
        // Session list
        Expanded(
          child: prov.sessions.isEmpty
              ? Center(
                  child: Text(
                    '没有对话记录',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color:
                          theme.colorScheme.onSurface.withOpacity(0.3),
                    ),
                  ),
                )
              : ListView.builder(
                  padding: const EdgeInsets.symmetric(vertical: 6),
                  itemCount: prov.sessions.length,
                  itemBuilder: (ctx, i) {
                    final s = prov.sessions[i];
                    final isActive = s.id == prov.currentSession?.id;
                    return _SessionTile(
                      session: s,
                      isActive: isActive,
                      onTap: () => prov.loadSession(s.id),
                    );
                  },
                ),
        ),
      ],
    );
  }
}

class _SessionTile extends StatelessWidget {
  final ChatSession session;
  final bool isActive;
  final VoidCallback onTap;

  const _SessionTile({
    required this.session,
    required this.isActive,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(8),
        child: Container(
          margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
          decoration: BoxDecoration(
            color: isActive
                ? theme.colorScheme.primaryContainer.withOpacity(0.25)
                : Colors.transparent,
            borderRadius: BorderRadius.circular(8),
            border: isActive
                ? Border.all(
                    color: theme.colorScheme.primary.withOpacity(0.3))
                : null,
          ),
          child: Row(
            children: [
              Icon(
                Icons.chat_bubble_outline_rounded,
                size: 15,
                color: isActive
                    ? theme.colorScheme.primary
                    : theme.colorScheme.onSurface.withOpacity(0.4),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      session.title,
                      style: theme.textTheme.bodySmall?.copyWith(
                        fontWeight: isActive
                            ? FontWeight.w600
                            : FontWeight.normal,
                        color: isActive
                            ? theme.colorScheme.primary
                            : null,
                      ),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                    if (session.messageCount > 0) ...[
                      const SizedBox(height: 2),
                      Text(
                        session.preview,
                        style: theme.textTheme.bodySmall?.copyWith(
                          fontSize: 11,
                          color: theme.colorScheme.onSurface
                              .withOpacity(0.35),
                        ),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ],
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
