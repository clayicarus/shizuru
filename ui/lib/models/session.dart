import 'message.dart';

class ChatSession {
  final String id;
  String title;
  final DateTime createdAt;
  final List<ChatMessage> messages;

  ChatSession({
    required this.id,
    required this.title,
    required this.createdAt,
    List<ChatMessage>? messages,
  }) : messages = messages ?? [];

  int get messageCount => messages.length;

  String get preview {
    if (messages.isEmpty) return '空对话';
    final last = messages.lastWhere(
      (m) => m.isUser || m.isAssistant,
      orElse: () => messages.last,
    );
    final text = last.content;
    return text.length > 50 ? '${text.substring(0, 50)}…' : text;
  }
}
