// Baidu ASR client — audio to text (Dart port of C++ BaiduAsrClient).

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'baidu_token_manager.dart';

class BaiduAsrClient {
  final BaiduTokenManager tokenManager;
  final String asrHost;
  final String asrPath;
  final String cuid;
  final String defaultFormat;
  final int rate;
  final int devPid;

  BaiduAsrClient({
    required this.tokenManager,
    this.asrHost = 'https://vop.baidu.com',
    this.asrPath = '/server_api',
    this.cuid = 'shizuru_agent',
    this.defaultFormat = 'wav',
    this.rate = 16000,
    this.devPid = 1537, // mandarin + punctuation
  });

  /// Transcribe [audioData] to text. Returns transcript or empty string.
  Future<String> transcribe(Uint8List audioData, {String mimeType = 'audio/wav'}) async {
    if (audioData.isEmpty) return '';

    final token = await tokenManager.getToken();
    final format = _detectFormat(mimeType);
    final speech = base64Encode(audioData);

    final body = jsonEncode({
      'format': format,
      'rate': rate,
      'channel': 1,
      'cuid': cuid,
      'token': token,
      'dev_pid': devPid,
      'speech': speech,
      'len': audioData.length,
    });

    final uri = Uri.parse('$asrHost$asrPath');
    final client = HttpClient()
      ..connectionTimeout = const Duration(seconds: 10);

    try {
      final request = await client.postUrl(uri);
      request.headers.contentType = ContentType.json;
      request.write(body);
      final response = await request.close();

      final responseBody = await response.transform(utf8.decoder).join();

      if (response.statusCode != 200) {
        return '';
      }

      final json = jsonDecode(responseBody) as Map<String, dynamic>;
      final errNo = json['err_no'] as int? ?? -1;
      if (errNo != 0) {
        return '';
      }

      final result = json['result'] as List<dynamic>?;
      if (result != null && result.isNotEmpty) {
        return result[0] as String;
      }
      return '';
    } finally {
      client.close(force: true);
    }
  }

  String _detectFormat(String mimeType) {
    if (mimeType.contains('wav')) return 'wav';
    if (mimeType.contains('pcm')) return 'pcm';
    if (mimeType.contains('amr')) return 'amr';
    if (mimeType.contains('m4a')) return 'm4a';
    return defaultFormat;
  }
}
