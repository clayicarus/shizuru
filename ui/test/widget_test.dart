import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shizuru_ui/providers/conversation_provider.dart';
import 'package:shizuru_ui/widgets/message_bubble.dart';

void main() {
  testWidgets('MessageBubble renders segments in chronological order', (
    WidgetTester tester,
  ) async {
    final message = ConversationMessage(
      role: 'assistant',
      timestamp: DateTime(2026, 4, 16, 12, 0, 0),
      segments: [
        BubbleSegment.thinking('let me check'),
        BubbleSegment.toolCall({
          'id': 'call_1',
          'name': 'search',
          'arguments': {'q': 'weather'},
        }),
        BubbleSegment.toolResult({
          'tool_name': 'search',
          'tool_call_id': 'call_1',
          'success': true,
          'result': {'success': true, 'output': 'sunny'},
        }),
        BubbleSegment.text('It is sunny today.'),
      ],
    );

    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(body: MessageBubble(message: message)),
      ),
    );

    // All segments should be present.
    expect(find.text('Thinking...'), findsOneWidget);
    expect(find.textContaining('search'), findsOneWidget);
    expect(find.text('It is sunny today.'), findsOneWidget);

    // No inline XML tags.
    expect(find.textContaining('<think>'), findsNothing);
    expect(find.textContaining('<tool_call>'), findsNothing);
  });
}
