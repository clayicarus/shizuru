// Baidu TTS client — text to audio (Dart port of C++ BaiduTtsClient).

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'baidu_token_manager.dart';

class BaiduTtsClient {
  final BaiduTokenManager tokenManager;
  final String ttsHost;
  final String ttsPath;
  final String cuid;
  final int spd;
  final int pit;
  final int vol;
  final int per;
  final int aue;

  BaiduTtsClient({
    required this.tokenManager,
    this.ttsHost = 'https://tsn.baidu.com',
    this.ttsPath = '/text2audio',
    this.cuid = 'shizuru_agent',
    this.spd = 5,
    this.pit = 5,
    this.vol = 5,
    this.per = 0,
    this.aue = 6, // 3=mp3, 4=pcm-8k, 5=pcm-16k, 6=wav
  });

  /// Synthesize [text] into audio bytes. Returns (audioBytes, mimeType).
  Future<(Uint8List, String)> synthesize(String text) async {
    final token = await tokenManager.getToken();

    final body = StringBuffer()
      ..write('tex=${Uri.encodeComponent(text)}')
      ..write('&tok=$token')
      ..write('&cuid=$cuid')
      ..write('&ctp=1')
      ..write('&lan=zh')
      ..write('&spd=$spd')
      ..write('&pit=$pit')
      ..write('&vol=$vol')
      ..write('&per=$per')
      ..write('&aue=$aue');

    final uri = Uri.parse('$ttsHost$ttsPath');
    final client = HttpClient()
      ..connectionTimeout = const Duration(seconds: 10);

    try {
      final request = await client.postUrl(uri);
      request.headers.contentType =
          ContentType('application', 'x-www-form-urlencoded');
      request.write(body.toString());
      final response = await request.close();

      if (response.statusCode != 200) {
        final errBody = await response.transform(utf8.decoder).join();
        throw Exception('Baidu TTS status ${response.statusCode}: $errBody');
      }

      final contentType =
          response.headers.value(HttpHeaders.contentTypeHeader) ?? '';

      final bytes = await _collectBytes(response);

      if (contentType.contains('audio/')) {
        final mime = _mimeFromAue(aue);
        return (bytes, mime);
      }

      // Error: response is JSON, not audio.
      final errText = utf8.decode(bytes);
      throw Exception('Baidu TTS error: $errText');
    } finally {
      client.close(force: true);
    }
  }

  String _mimeFromAue(int aue) {
    switch (aue) {
      case 3:
        return 'audio/mp3';
      case 4:
      case 5:
        return 'audio/pcm';
      case 6:
        return 'audio/wav';
      default:
        return 'audio/mp3';
    }
  }

  Future<Uint8List> _collectBytes(HttpClientResponse response) async {
    final builder = BytesBuilder(copy: false);
    await for (final chunk in response) {
      builder.add(chunk);
    }
    return builder.takeBytes();
  }
}
