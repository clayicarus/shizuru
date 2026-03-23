enum MessageRole { user, assistant, toolCall }

enum MessageStatus { complete, streaming, error }

class ChatMessage {
  final String id;
  final MessageRole role;
  String content;
  final DateTime timestamp;
  final String? toolName;
  MessageStatus status;

  ChatMessage({
    required this.id,
    required this.role,
    required this.content,
    required this.timestamp,
    this.toolName,
    this.status = MessageStatus.complete,
  });

  bool get isUser => role == MessageRole.user;
  bool get isAssistant => role == MessageRole.assistant;
  bool get isToolCall => role == MessageRole.toolCall;
  bool get isStreaming => status == MessageStatus.streaming;
  bool get isError => status == MessageStatus.error;
}
