import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shizuru_ui/providers/conversation_provider.dart';
import 'package:shizuru_ui/widgets/message_bubble.dart';

void main() {
  testWidgets('MessageBubble renders structured tool events without inline tags',
      (WidgetTester tester) async {
    final message = ConversationMessage(
      role: 'assistant',
      text: 'Final answer',
      timestamp: DateTime(2026, 4, 16, 12, 0, 0),
      events: [
        ConversationToolEvent(
          kind: ConversationToolEventKind.toolCall,
          data: {
            'id': 'call_1',
            'name': 'search',
            'arguments': {'q': 'weather'},
          },
        ),
        ConversationToolEvent(
          kind: ConversationToolEventKind.toolResult,
          data: {
            'id': 'call_1',
            'name': 'search',
            'success': true,
            'result': {'success': true, 'output': 'sunny'},
          },
        ),
      ],
    );

    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: MessageBubble(message: message),
      ),
    ));

    expect(find.textContaining('search'), findsOneWidget);
    expect(find.text('Final answer'), findsOneWidget);
    expect(find.textContaining('<tool_call>'), findsNothing);
    expect(find.textContaining('<tool_result>'), findsNothing);
  });
}
