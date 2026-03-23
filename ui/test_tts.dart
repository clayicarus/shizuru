// Standalone Baidu TTS test — no Flutter dependencies.
// Run with: dart run test_tts.dart

import 'dart:convert';
import 'dart:io';

Future<String> getToken(String apiKey, String secretKey) async {
  final uri = Uri.parse('https://aip.baidubce.com/oauth/2.0/token');
  final body =
      'grant_type=client_credentials&client_id=$apiKey&client_secret=$secretKey';

  final client = HttpClient()..connectionTimeout = const Duration(seconds: 10);
  final request = await client.postUrl(uri);
  request.headers.contentType =
      ContentType('application', 'x-www-form-urlencoded');
  request.write(body);
  final response = await request.close();
  final responseBody = await response.transform(utf8.decoder).join();
  client.close(force: true);

  if (response.statusCode != 200) {
    print('Token API error ${response.statusCode}: $responseBody');
    exit(1);
  }

  final json = jsonDecode(responseBody) as Map<String, dynamic>;
  final token = json['access_token'] as String;
  print('[OK] Got access_token: ${token.substring(0, 20)}...');
  return token;
}

Future<void> testTts(String token) async {
  final text = '你好，我是Shizuru。';
  final body = StringBuffer()
    ..write('tex=${Uri.encodeComponent(text)}')
    ..write('&tok=$token')
    ..write('&cuid=shizuru_test')
    ..write('&ctp=1')
    ..write('&lan=zh')
    ..write('&spd=5')
    ..write('&pit=5')
    ..write('&vol=5')
    ..write('&per=0')
    ..write('&aue=6'); // wav

  final uri = Uri.parse('https://tsn.baidu.com/text2audio');
  final client = HttpClient()..connectionTimeout = const Duration(seconds: 10);
  final request = await client.postUrl(uri);
  request.headers.contentType =
      ContentType('application', 'x-www-form-urlencoded');
  request.write(body.toString());
  final response = await request.close();

  final contentType =
      response.headers.value(HttpHeaders.contentTypeHeader) ?? '';
  print('[INFO] Response status: ${response.statusCode}');
  print('[INFO] Content-Type: $contentType');

  final bytes = await _collectBytes(response);
  client.close(force: true);

  if (contentType.contains('audio/')) {
    final outFile = File('test_output.wav');
    await outFile.writeAsBytes(bytes);
    print('[OK] TTS success! Audio saved to ${outFile.path} (${bytes.length} bytes)');
    print('[INFO] Try playing: start test_output.wav');
  } else {
    print('[FAIL] TTS returned error: ${utf8.decode(bytes)}');
  }
}

Future<List<int>> _collectBytes(HttpClientResponse response) async {
  final result = <int>[];
  await for (final chunk in response) {
    result.addAll(chunk);
  }
  return result;
}

void main() async {
  const apiKey = '';     // TODO: fill in your Baidu API Key
  const secretKey = '';  // TODO: fill in your Baidu Secret Key

  print('=== Baidu TTS Test ===');
  print('1. Getting token...');
  final token = await getToken(apiKey, secretKey);

  print('2. Synthesizing speech...');
  await testTts(token);

  print('=== Done ===');
}
