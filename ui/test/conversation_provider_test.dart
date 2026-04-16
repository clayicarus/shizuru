import 'package:flutter_test/flutter_test.dart';
import 'package:shizuru_ui/bridge/activity_kind.dart';
import 'package:shizuru_ui/providers/conversation_provider.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('tool activities attach to the current assistant bubble', () {
    final provider = ConversationProvider();

    provider.onToolActivity(
      ActivityKind.toolDispatched.index,
      '{"id":"call_1","name":"search","arguments":{"q":"weather"}}',
    );
    provider.onOutputChunk('Looking it up', true);
    provider.onToolActivity(
      ActivityKind.toolResultReceived.index,
      '{"id":"call_1","name":"search","success":true,"result":{"success":true,"output":"sunny"}}',
    );
    provider.onOutputChunk('Looking it up', false);

    expect(provider.messages.length, 1);
    final message = provider.messages.single;
    expect(message.role, 'assistant');
    expect(message.text, 'Looking it up');
    expect(message.isStreaming, isFalse);
    expect(message.events.length, 2);
    expect(message.events[0].kind, ConversationToolEventKind.toolCall);
    expect(message.events[1].kind, ConversationToolEventKind.toolResult);
    expect(message.events[0].data['name'], 'search');
  });

  test('final output no longer prepends legacy inline protocol blocks', () {
    final provider = ConversationProvider();

    provider.onToolActivity(
      ActivityKind.toolDispatched.index,
      '{"id":"call_2","name":"set_reminder","arguments":{"minutes":10}}',
    );
    provider.onOutputChunk('Done', false);

    expect(provider.messages.length, 1);
    final message = provider.messages.single;
    expect(message.text, 'Done');
    expect(message.text.contains('<tool_call>'), isFalse);
    expect(message.text.contains('<tool_result>'), isFalse);
    expect(message.events.length, 1);
  });
}
