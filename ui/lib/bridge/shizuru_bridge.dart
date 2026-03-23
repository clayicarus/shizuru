// dart:ffi bridge to shizuru C++ runtime.
//
// ShizuruBridge.instance returns _HttpBridge by default, which calls the
// OpenAI-compatible LLM API directly from Dart with SSE streaming.
//
// To wire the real C++ layer (Phase 7 / ROADMAP):
//   1. Expose a C API from AgentRuntime (extern "C" functions).
//   2. Replace _HttpBridge with _FfiBridge that calls those functions via
//      dart:ffi.
//   3. The abstract interface below stays unchanged — the UI never changes.

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'audio_service.dart';
import 'baidu_asr_client.dart';
import 'baidu_token_manager.dart';
import 'baidu_tts_client.dart';

// ─── Config ──────────────────────────────────────────────────────────────────

class RuntimeConfig {
  final String llmBaseUrl;
  final String llmApiKey;
  final String llmModel;
  final String llmApiPath;
  final String? asrApiKey;
  final String? asrSecretKey;
  final String? ttsApiKey;
  final String? ttsVoiceId;
  final String systemPrompt;

  const RuntimeConfig({
    required this.llmBaseUrl,
    required this.llmApiKey,
    required this.llmModel,
    this.llmApiPath = '/v1/chat/completions',
    this.asrApiKey,
    this.asrSecretKey,
    this.ttsApiKey,
    this.ttsVoiceId,
    this.systemPrompt = 'You are a helpful voice assistant.',
  });
}

// ─── C++ State enum (mirrors core::State) ────────────────────────────────────

enum CppAgentState {
  idle,
  listening,
  thinking,
  routing,
  acting,
  responding,
  error,
  terminated,
}

// ─── Abstract bridge interface ────────────────────────────────────────────────

abstract class ShizuruBridge {
  static ShizuruBridge? _instance;
  static ShizuruBridge get instance => _instance ??= _HttpBridge();

  // Switch to a different bridge implementation at startup.
  static void useInstance(ShizuruBridge bridge) => _instance = bridge;

  Future<void> initRuntime(RuntimeConfig config);
  Future<String> startSession();
  Future<void> sendMessage(String content);
  CppAgentState getState();
  void onOutput(void Function(String text) callback);
  void onStateChange(void Function(CppAgentState state) callback);
  Future<void> shutdown();
  bool get hasActiveSession;

  // Voice features
  Future<bool> startRecording();
  Future<String?> stopRecordingAndTranscribe();
  Future<void> speakText(String text);
  Future<void> stopSpeaking();
  bool get isSpeaking;
  bool get isRecording;
}

// ─── HTTP bridge — calls OpenAI-compatible API with SSE streaming ────────────

class _HttpBridge extends ShizuruBridge {
  RuntimeConfig? _config;
  CppAgentState _state = CppAgentState.idle;
  void Function(String)? _outputCb;
  void Function(CppAgentState)? _stateCb;
  bool _active = false;
  HttpClient? _httpClient;

  // Conversation history for multi-turn context.
  final List<Map<String, String>> _history = [];

  // Voice services
  BaiduTokenManager? _tokenMgr;
  BaiduTtsClient? _ttsClient;
  BaiduAsrClient? _asrClient;
  final AudioService _audioService = AudioService();
  bool _ttsEnabled = false;
  bool _asrEnabled = false;

  void _emit(CppAgentState s) {
    _state = s;
    _stateCb?.call(s);
  }

  @override
  Future<void> initRuntime(RuntimeConfig config) async {
    _config = config;
    _httpClient?.close(force: true);
    _httpClient = HttpClient()
      ..connectionTimeout = const Duration(seconds: 15);

    // Initialize Baidu voice services if keys are provided.
    if (config.asrApiKey != null &&
        config.asrApiKey!.isNotEmpty &&
        config.asrSecretKey != null &&
        config.asrSecretKey!.isNotEmpty) {
      _tokenMgr = BaiduTokenManager(
        apiKey: config.asrApiKey!,
        secretKey: config.asrSecretKey!,
      );
      _ttsClient = BaiduTtsClient(tokenManager: _tokenMgr!);
      _asrClient = BaiduAsrClient(tokenManager: _tokenMgr!);
      _ttsEnabled = true;
      _asrEnabled = true;
      await _audioService.init();
      stderr.writeln('[Bridge] Baidu voice initialized: tts=$_ttsEnabled asr=$_asrEnabled audio=${_audioService.available}');
    } else {
      stderr.writeln('[Bridge] Baidu voice NOT initialized: asrKey=${config.asrApiKey}, secretKey=${config.asrSecretKey}');
    }
  }

  @override
  Future<String> startSession() async {
    _active = true;
    _history.clear();
    if (_config != null && _config!.systemPrompt.isNotEmpty) {
      _history.add({'role': 'system', 'content': _config!.systemPrompt});
    }
    _emit(CppAgentState.idle);
    return 'session_${DateTime.now().millisecondsSinceEpoch}';
  }

  @override
  Future<void> sendMessage(String content) async {
    if (_config == null || _config!.llmApiKey.isEmpty) {
      _outputCb?.call('Error: API Key 未配置，请前往 设置 页面填写。');
      return;
    }

    _history.add({'role': 'user', 'content': content});
    _emit(CppAgentState.thinking);

    try {
      final url = Uri.parse('${_config!.llmBaseUrl}${_config!.llmApiPath}');
      final client = _httpClient ?? HttpClient();
      final request = await client.postUrl(url);

      request.headers.set('Authorization', 'Bearer ${_config!.llmApiKey}');
      request.headers.contentType = ContentType.json;

      final body = jsonEncode({
        'model': _config!.llmModel,
        'messages': _history,
        'stream': true,
        'stream_options': {'include_usage': true},
      });
      request.write(body);

      final response = await request.close();

      if (response.statusCode != 200) {
        final errBody = await response.transform(utf8.decoder).join();
        _outputCb?.call('API Error (${response.statusCode}): $errBody');
        _emit(CppAgentState.error);
        _history.removeLast();
        return;
      }

      String accumulated = '';
      String lineBuffer = '';
      bool contentStarted = false;

      await for (final chunk in response.transform(utf8.decoder)) {
        lineBuffer += chunk;

        while (lineBuffer.contains('\n')) {
          final idx = lineBuffer.indexOf('\n');
          final line = lineBuffer.substring(0, idx).trim();
          lineBuffer = lineBuffer.substring(idx + 1);

          if (!line.startsWith('data:')) continue;
          final data = line.substring(5).trim();
          if (data == '[DONE]') continue;

          try {
            final json = jsonDecode(data) as Map<String, dynamic>;
            final choices = json['choices'] as List?;
            if (choices == null || choices.isEmpty) continue;

            final delta = choices[0]['delta'] as Map<String, dynamic>?;
            if (delta == null) continue;

            // Handle reasoning_content (model thinking phase, e.g. Qwen3).
            // Keep state as 'thinking' — don't show reasoning in chat.
            final reasoning = delta['reasoning_content'] as String?;
            if (reasoning != null && reasoning.isNotEmpty && !contentStarted) {
              // Still in thinking phase — state already set.
              continue;
            }

            // Handle actual content.
            final contentDelta = delta['content'] as String?;
            if (contentDelta != null && contentDelta.isNotEmpty) {
              if (!contentStarted) {
                contentStarted = true;
                _emit(CppAgentState.responding);
              }
              accumulated += contentDelta;
              _outputCb?.call(accumulated);
            }
          } catch (_) {
            // Skip malformed JSON chunks.
          }
        }
      }

      // Add assistant response to conversation history.
      if (accumulated.isNotEmpty) {
        _history.add({'role': 'assistant', 'content': accumulated});
      }
    } catch (e) {
      _outputCb?.call('Connection Error: $e');
      _emit(CppAgentState.error);
      _history.removeLast();
      return;
    }

    _emit(CppAgentState.idle);
  }

  @override
  CppAgentState getState() => _state;

  @override
  void onOutput(void Function(String) callback) => _outputCb = callback;

  @override
  void onStateChange(void Function(CppAgentState) callback) =>
      _stateCb = callback;

  // ── Voice features ──────────────────────────────────────────────────────

  @override
  Future<bool> startRecording() async {
    if (!_asrEnabled) return false;
    _emit(CppAgentState.listening);
    return await _audioService.startRecording();
  }

  @override
  Future<String?> stopRecordingAndTranscribe() async {
    if (!_asrEnabled) return null;
    final audioBytes = await _audioService.stopRecording();
    if (audioBytes == null || audioBytes.isEmpty) {
      _emit(CppAgentState.idle);
      return null;
    }
    _emit(CppAgentState.thinking);
    try {
      final transcript =
          await _asrClient!.transcribe(audioBytes, mimeType: 'audio/wav');
      if (transcript.isEmpty) {
        _emit(CppAgentState.idle);
        return null;
      }
      return transcript;
    } catch (e) {
      _emit(CppAgentState.error);
      return null;
    }
  }

  @override
  Future<void> speakText(String text) async {
    stderr.writeln('[TTS] speakText called: ttsEnabled=$_ttsEnabled textLen=${text.length}');
    if (!_ttsEnabled || text.isEmpty) {
      stderr.writeln('[TTS] skipped: ttsEnabled=$_ttsEnabled empty=${text.isEmpty}');
      return;
    }
    // Baidu TTS has ~500 char limit; truncate if needed.
    final ttsText = text.length > 400 ? text.substring(0, 400) : text;
    try {
      stderr.writeln('[TTS] Calling synthesize (${ttsText.length} chars)...');
      final (audioBytes, mimeType) = await _ttsClient!.synthesize(ttsText);
      stderr.writeln('[TTS] Got audio: ${audioBytes.length} bytes, mime=$mimeType');
      await _audioService.playAudioBytes(audioBytes, mimeType);
      stderr.writeln('[TTS] Playback finished.');
    } catch (e, st) {
      stderr.writeln('[TTS] ERROR: $e');
      stderr.writeln('[TTS] Stack: $st');
    }
  }

  @override
  Future<void> stopSpeaking() async {
    await _audioService.stopPlayback();
  }

  @override
  bool get isSpeaking => _audioService.isPlaying;

  @override
  bool get isRecording => _audioService.isRecording;

  @override
  Future<void> shutdown() async {
    _active = false;
    _history.clear();
    _httpClient?.close(force: true);
    _httpClient = null;
    await _audioService.dispose();
    _emit(CppAgentState.idle);
  }

  @override
  bool get hasActiveSession => _active;
}

// ─── Mock bridge (kept for offline testing) ──────────────────────────────────

class MockBridge extends ShizuruBridge {
  CppAgentState _state = CppAgentState.idle;
  void Function(String)? _outputCb;
  void Function(CppAgentState)? _stateCb;
  bool _active = false;
  int _msgIdx = 0;

  @override
  Future<bool> startRecording() async => false;
  @override
  Future<String?> stopRecordingAndTranscribe() async => null;
  @override
  Future<void> speakText(String text) async {}
  @override
  Future<void> stopSpeaking() async {}
  @override
  bool get isSpeaking => false;
  @override
  bool get isRecording => false;

  static const List<String> _replies = [
    '你好！我是 Shizuru，你的 AI 助手。有什么我可以帮你的？',
    '这是个好问题。根据我的分析，建议采用以下方案：首先梳理需求，然后分阶段实现，最后验证结果。',
    '我已经处理了你的请求。是否还有其他需要帮助的地方？',
    '基于当前上下文，我认为最优解是将问题分解为更小的子任务，逐步解决。',
    '明白了。让我为你详细解释这个概念，以便你能更好地理解和应用。',
  ];

  void _emit(CppAgentState s) {
    _state = s;
    _stateCb?.call(s);
  }

  @override
  Future<void> initRuntime(RuntimeConfig config) async {}

  @override
  Future<String> startSession() async {
    _active = true;
    _emit(CppAgentState.idle);
    return 'mock_session_${DateTime.now().millisecondsSinceEpoch}';
  }

  @override
  Future<void> sendMessage(String content) async {
    _emit(CppAgentState.thinking);
    await Future.delayed(const Duration(milliseconds: 600));

    // Simulate a tool call 30% of the time
    if (_msgIdx % 3 == 1) {
      _emit(CppAgentState.acting);
      await Future.delayed(const Duration(milliseconds: 400));
      // Notify tool call via a special prefix the provider can detect
      _outputCb?.call('\x00tool:web_search');
    }

    _emit(CppAgentState.responding);
    final reply = _replies[_msgIdx % _replies.length];
    _msgIdx++;

    // Stream the reply character by character
    for (int i = 1; i <= reply.length; i++) {
      await Future.delayed(const Duration(milliseconds: 25));
      _outputCb?.call(reply.substring(0, i));
    }

    await Future.delayed(const Duration(milliseconds: 150));
    _emit(CppAgentState.idle);
  }

  @override
  CppAgentState getState() => _state;

  @override
  void onOutput(void Function(String) callback) => _outputCb = callback;

  @override
  void onStateChange(void Function(CppAgentState) callback) =>
      _stateCb = callback;

  @override
  Future<void> shutdown() async {
    _active = false;
    _emit(CppAgentState.idle);
  }

  @override
  bool get hasActiveSession => _active;
}
