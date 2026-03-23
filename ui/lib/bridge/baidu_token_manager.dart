// Baidu API token lifecycle manager (Dart port of C++ BaiduTokenManager).

import 'dart:convert';
import 'dart:io';

class BaiduTokenManager {
  final String apiKey;
  final String secretKey;
  final String tokenUrl;
  final String tokenPath;

  String _accessToken = '';
  DateTime _expiresAt = DateTime(2000);

  BaiduTokenManager({
    required this.apiKey,
    required this.secretKey,
    this.tokenUrl = 'https://aip.baidubce.com',
    this.tokenPath = '/oauth/2.0/token',
  });

  /// Returns a valid access_token, refreshing if expired.
  Future<String> getToken() async {
    if (_accessToken.isNotEmpty && DateTime.now().isBefore(_expiresAt)) {
      return _accessToken;
    }
    await _refresh();
    return _accessToken;
  }

  Future<void> _refresh() async {
    final uri = Uri.parse('$tokenUrl$tokenPath');
    final body =
        'grant_type=client_credentials&client_id=$apiKey&client_secret=$secretKey';

    final client = HttpClient()..connectionTimeout = const Duration(seconds: 10);
    try {
      final request = await client.postUrl(uri);
      request.headers.contentType =
          ContentType('application', 'x-www-form-urlencoded');
      request.write(body);
      final response = await request.close();

      final responseBody = await response.transform(utf8.decoder).join();

      if (response.statusCode != 200) {
        throw Exception(
            'Baidu token API returned ${response.statusCode}: $responseBody');
      }

      final json = jsonDecode(responseBody) as Map<String, dynamic>;
      if (!json.containsKey('access_token')) {
        throw Exception('Baidu token response missing access_token: $responseBody');
      }

      _accessToken = json['access_token'] as String;
      final expiresIn = (json['expires_in'] as int?) ?? 2592000;
      // Subtract 5-minute safety margin.
      _expiresAt = DateTime.now()
          .add(Duration(seconds: expiresIn))
          .subtract(const Duration(minutes: 5));
    } finally {
      client.close(force: true);
    }
  }
}
