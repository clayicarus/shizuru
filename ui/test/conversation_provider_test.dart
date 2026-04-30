import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:shizuru_ui/providers/conversation_provider.dart';

String _makeItem(
  String kind,
  Map<String, dynamic> payload, {
  String? itemId,
  String? turnGroupId,
}) {
  return jsonEncode({
    if (itemId != null) 'item_id': itemId,
    if (turnGroupId != null) 'turn_group_id': turnGroupId,
    'kind': kind,
    'payload': payload,
  });
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'segments are in chronological order: think → tool_call → tool_result → text',
    () {
      final provider = ConversationProvider();
      const turnId = 'turn_1';

      // 1. Streaming think tokens
      provider.onConversationItem(
        _makeItem(
          'assistant_message',
          {'text': '<think>let me think'},
          itemId: 'stream_1',
          turnGroupId: turnId,
        ),
        true,
      );
      provider.onConversationItem(
        _makeItem(
          'assistant_message',
          {'text': '</think>'},
          itemId: 'stream_1',
          turnGroupId: turnId,
        ),
        true,
      );

      // 2. Tool call
      provider.onConversationItem(
        _makeItem(
          'tool_call',
          {
            'tool_calls': [
              {
                'id': 'c1',
                'type': 'function',
                'function': {
                  'name': 'search',
                  'arguments': {'q': 'weather'},
                },
              },
            ],
          },
          itemId: 'tool_call_1',
          turnGroupId: turnId,
        ),
        false,
      );

      // 3. Tool result
      provider.onConversationItem(
        _makeItem(
          'tool_result',
          {
            'tool_name': 'search',
            'tool_call_id': 'c1',
            'content': {'success': true, 'output': 'sunny'},
          },
          itemId: 'tool_result_1',
          turnGroupId: turnId,
        ),
        false,
      );

      // 4. Final response
      provider.onConversationItem(
        _makeItem(
          'assistant_message',
          {'text': 'It is sunny today.'},
          itemId: 'stream_2',
          turnGroupId: turnId,
        ),
        false,
      );

      expect(provider.messages.length, 1);
      final msg = provider.messages.single;
      expect(msg.isStreaming, isFalse);

      // Verify chronological segment order.
      final kinds = msg.segments.map((s) => s.kind).toList();
      expect(kinds, [
        SegmentKind.thinking,
        SegmentKind.toolCall,
        SegmentKind.toolResult,
        SegmentKind.text,
      ]);

      expect(msg.segments[0].text, 'let me think');
      expect(msg.segments[1].data!['name'], 'search');
      expect(msg.segments[2].data!['success'], true);
      expect(msg.segments[3].text, 'It is sunny today.');
    },
  );

  test('streaming text accumulates into a single text segment', () {
    final provider = ConversationProvider();

    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': 'Hello'},
        itemId: 'stream_1',
        turnGroupId: 'turn_1',
      ),
      true,
    );
    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': ' world'},
        itemId: 'stream_1',
        turnGroupId: 'turn_1',
      ),
      true,
    );

    expect(provider.messages.length, 1);
    final msg = provider.messages.single;
    expect(msg.segments.length, 1);
    expect(msg.segments[0].kind, SegmentKind.text);
    expect(msg.segments[0].text, 'Hello world');
  });

  test('think block splits into separate segment during streaming', () {
    final provider = ConversationProvider();

    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': '<think>reasoning'},
        itemId: 'stream_1',
        turnGroupId: 'turn_1',
      ),
      true,
    );
    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': '</think>answer'},
        itemId: 'stream_1',
        turnGroupId: 'turn_1',
      ),
      true,
    );

    final msg = provider.messages.single;
    expect(msg.segments.length, 2);
    expect(msg.segments[0].kind, SegmentKind.thinking);
    expect(msg.segments[0].text, 'reasoning');
    expect(msg.segments[1].kind, SegmentKind.text);
    expect(msg.segments[1].text, 'answer');
  });

  test('final response replaces streaming text but keeps other segments', () {
    final provider = ConversationProvider();
    const turnId = 'turn_1';

    // Streaming phase
    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': '<think>hmm</think>partial'},
        itemId: 'stream_before_tool',
        turnGroupId: turnId,
      ),
      true,
    );

    // Tool call arrives
    provider.onConversationItem(
      _makeItem(
        'tool_call',
        {
          'tool_calls': [
            {
              'id': 'c1',
              'type': 'function',
              'function': {'name': 'calc', 'arguments': {}},
            },
          ],
        },
        itemId: 'tool_call_1',
        turnGroupId: turnId,
      ),
      false,
    );

    // Final response (think stripped by C++ ResponseFilter)
    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': 'The answer is 42.'},
        itemId: 'stream_after_tool',
        turnGroupId: turnId,
      ),
      false,
    );

    final msg = provider.messages.single;
    expect(msg.isStreaming, isFalse);

    final kinds = msg.segments.map((s) => s.kind).toList();
    expect(kinds, [
      SegmentKind.thinking,
      SegmentKind.toolCall,
      SegmentKind.text,
    ]);
    expect(msg.segments[0].text, 'hmm');
    expect(msg.segments[2].text, 'The answer is 42.');
  });

  test(
    'final response drops earlier provisional text but preserves tool segments',
    () {
      final provider = ConversationProvider();
      const turnId = 'turn_1';

      provider.onConversationItem(
        _makeItem(
          'assistant_message',
          {'text': 'First part. '},
          itemId: 'stream_a',
          turnGroupId: turnId,
        ),
        true,
      );
      provider.onConversationItem(
        _makeItem(
          'tool_call',
          {
            'tool_calls': [
              {
                'id': 'c1',
                'type': 'function',
                'function': {'name': 'lookup', 'arguments': {}},
              },
            ],
          },
          itemId: 'tool_call_1',
          turnGroupId: turnId,
        ),
        false,
      );
      provider.onConversationItem(
        _makeItem(
          'assistant_message',
          {'text': 'draft'},
          itemId: 'stream_b',
          turnGroupId: turnId,
        ),
        true,
      );
      provider.onConversationItem(
        _makeItem(
          'assistant_message',
          {'text': 'Final answer'},
          itemId: 'stream_b',
          turnGroupId: turnId,
        ),
        false,
      );

      final msg = provider.messages.single;
      expect(msg.segments.map((s) => s.kind).toList(), [
        SegmentKind.toolCall,
        SegmentKind.text,
      ]);
      expect(
        msg.segments.where((s) => s.kind == SegmentKind.text).single.text,
        'Final answer',
      );
      expect(msg.segments[1].text, 'Final answer');
    },
  );

  test('new user message closes the interrupted assistant bubble', () {
    final provider = ConversationProvider();

    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': 'partial'},
        itemId: 'stream_1',
        turnGroupId: 'turn_1',
      ),
      true,
    );

    expect(provider.messages.single.isStreaming, isTrue);

    provider.addUserMessage('stop');

    expect(provider.messages.length, 2);
    expect(provider.messages.first.role, 'assistant');
    expect(provider.messages.first.isStreaming, isFalse);
    expect(provider.messages.last.role, 'user');
  });

  test('different turn groups never merge into the same assistant bubble', () {
    final provider = ConversationProvider();

    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': 'old'},
        itemId: 'stream_old',
        turnGroupId: 'turn_old',
      ),
      true,
    );
    provider.addUserMessage('interrupt');
    provider.onConversationItem(
      _makeItem(
        'assistant_message',
        {'text': 'new'},
        itemId: 'stream_new',
        turnGroupId: 'turn_new',
      ),
      true,
    );

    expect(provider.messages.length, 3);
    expect(provider.messages[0].role, 'assistant');
    expect(provider.messages[0].segments.single.text, 'old');
    expect(provider.messages[2].role, 'assistant');
    expect(provider.messages[2].segments.single.text, 'new');
  });

  test('replayed human_message is rendered as a user bubble', () {
    final provider = ConversationProvider();

    provider.onConversationItem(
      _makeItem(
        'human_message',
        {'text': 'loaded from history'},
        itemId: 'user_1',
      ),
      false,
    );

    expect(provider.messages.length, 1);
    expect(provider.messages.single.role, 'user');
    expect(provider.messages.single.segments.single.text, 'loaded from history');
  });

  test('plainText getter returns only text segments', () {
    final msg = ConversationMessage(
      role: 'assistant',
      timestamp: DateTime.now(),
      segments: [
        BubbleSegment.thinking('internal', itemId: 'a'),
        BubbleSegment.toolCall({'name': 'x'}, itemId: 'b'),
        BubbleSegment.text('visible', itemId: 'c'),
      ],
    );
    expect(msg.plainText, 'visible');
  });
}
