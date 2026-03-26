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

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'audio_service.dart';
import 'baidu_asr_client.dart';
import 'baidu_token_manager.dart';
import 'baidu_tts_client.dart';
import 'shizuru_ffi.dart';

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

// ─── FFI bridge — calls C++ AgentRuntime via dart:ffi ────────────────────────

class FfiBridge extends ShizuruBridge {
  late final ShizuruNativeBindings _bindings;
  Pointer<ShizuruRuntime>? _handle;
  CppAgentState _state = CppAgentState.idle;
  void Function(String)? _outputCb;
  void Function(CppAgentState)? _stateCb;
  void Function(String toolName, String arguments)? _toolCallCb;

  // NativeCallable instances must be kept alive for the callback pointers.
  NativeCallable<NativeOutputCallback>? _nativeOutputCb;
  NativeCallable<NativeStateCallback>? _nativeStateCb;
  NativeCallable<NativeToolCallCallback>? _nativeToolCallCb;
  NativeCallable<NativeTranscriptionCallback>? _nativeTranscriptionCb;

  // Voice configured flag (set after successful shizuru_setup_voice call).
  bool _voiceConfigured = false;

  // Audio output: polls shizuru_take_audio() after each speakText() call.
  final AudioService _audioService = AudioService();
  Timer? _audioPoller;

  // ASR recording state.
  bool _recording = false;
  Completer<String?>? _transcriptionCompleter;

  FfiBridge({String? libraryPath}) {
    if (libraryPath != null) {
      _bindings = ShizuruNativeBindings.fromPath(libraryPath);
    } else {
      _bindings = ShizuruNativeBindings();
    }
  }

  @override
  Future<void> initRuntime(RuntimeConfig config) async {
    // Destroy previous runtime if any.
    if (_handle != null) {
      _bindings.destroy(_handle!);
      _handle = null;
    }
    _disposeNativeCallbacks();

    final configJson = jsonEncode({
      'llm_base_url': config.llmBaseUrl,
      'llm_api_path': config.llmApiPath,
      'llm_api_key': config.llmApiKey,
      'llm_model': config.llmModel,
      'system_prompt': config.systemPrompt,
    });

    final configPtr = configJson.toNativeUtf8();
    try {
      _handle = _bindings.create(configPtr);
    } finally {
      calloc.free(configPtr);
    }

    if (_handle == null || _handle == nullptr) {
      throw StateError('shizuru_create returned null — check config and logs');
    }

    // Wire C++ TTS pipeline via shizuru_setup_voice.
    _voiceConfigured = false;
    final hasBaidu = config.asrApiKey != null &&
        config.asrApiKey!.isNotEmpty &&
        config.asrSecretKey != null &&
        config.asrSecretKey!.isNotEmpty;
    final hasElevenLabs = config.ttsApiKey != null &&
        config.ttsApiKey!.isNotEmpty;
    if (hasBaidu || hasElevenLabs) {
      final voiceCfg = jsonEncode({
        'tts_provider': hasElevenLabs ? 'elevenlabs' : 'baidu',
        if (config.asrApiKey != null)  'asr_api_key':    config.asrApiKey,
        if (config.asrSecretKey != null) 'asr_secret_key': config.asrSecretKey,
        if (config.ttsApiKey != null)  'tts_api_key':    config.ttsApiKey,
        if (config.ttsVoiceId != null) 'tts_voice_id':   config.ttsVoiceId,
      });
      final voicePtr = voiceCfg.toNativeUtf8();
      try {
        final result = _bindings.setupVoice(_handle!, voicePtr);
        _voiceConfigured = result != 0;
      } finally {
        calloc.free(voicePtr);
      }
    }

    if (_voiceConfigured) {
      await _audioService.init();
    }

    // Register callbacks via NativeCallable.listener (safe from any thread).
    _nativeOutputCb = NativeCallable<NativeOutputCallback>.listener(
      (Pointer<Utf8> textPtr, Pointer<Void> _) {
        final text = textPtr.toDartString();
        _bindings.freeString(textPtr);
        _outputCb?.call(text);
      },
    );

    _nativeStateCb = NativeCallable<NativeStateCallback>.listener(
      (int state, Pointer<Void> _) {
        if (state >= 0 && state < CppAgentState.values.length) {
          _state = CppAgentState.values[state];
          _stateCb?.call(_state);
        }
      },
    );

    _nativeToolCallCb = NativeCallable<NativeToolCallCallback>.listener(
      (Pointer<Utf8> namePtr, Pointer<Utf8> argsPtr, Pointer<Void> _) {
        final name = namePtr.toDartString();
        final args = argsPtr.toDartString();
        _bindings.freeString(namePtr);
        _bindings.freeString(argsPtr);
        _toolCallCb?.call(name, args);
      },
    );

    _nativeTranscriptionCb =
        NativeCallable<NativeTranscriptionCallback>.listener(
      (Pointer<Utf8> textPtr, Pointer<Void> _) {
        String? text;
        if (textPtr.address != 0) {
          text = textPtr.toDartString();
          _bindings.freeString(textPtr);
        }
        _transcriptionCompleter?.complete(text);
        _transcriptionCompleter = null;
      },
    );

    _bindings.setOutputCallback(
        _handle!, _nativeOutputCb!.nativeFunction, nullptr);
    _bindings.setStateCallback(
        _handle!, _nativeStateCb!.nativeFunction, nullptr);
    _bindings.setToolCallCallback(
        _handle!, _nativeToolCallCb!.nativeFunction, nullptr);
    _bindings.setTranscriptionCallback(
        _handle!, _nativeTranscriptionCb!.nativeFunction, nullptr);
  }

  /// Register a tool call notification callback.
  void onToolCall(void Function(String toolName, String arguments) callback) {
    _toolCallCb = callback;
  }

  @override
  Future<String> startSession() async {
    if (_handle == null) throw StateError('Runtime not initialized');
    final idPtr = _bindings.startSession(_handle!);
    if (idPtr == nullptr) throw StateError('shizuru_start_session returned null');
    final sessionId = idPtr.toDartString();
    _bindings.freeString(idPtr);
    return sessionId;
  }

  @override
  Future<void> sendMessage(String content) async {
    if (_handle == null) return;
    final contentPtr = content.toNativeUtf8();
    try {
      _bindings.sendMessage(_handle!, contentPtr);
    } finally {
      calloc.free(contentPtr);
    }
  }

  @override
  CppAgentState getState() {
    if (_handle == null) return CppAgentState.terminated;
    final raw = _bindings.getState(_handle!);
    if (raw >= 0 && raw < CppAgentState.values.length) {
      return CppAgentState.values[raw];
    }
    return CppAgentState.error;
  }

  @override
  void onOutput(void Function(String) callback) => _outputCb = callback;

  @override
  void onStateChange(void Function(CppAgentState) callback) =>
      _stateCb = callback;

  @override
  Future<void> shutdown() async {
    if (_handle == null) return;
    _bindings.shutdown(_handle!);
  }

  @override
  bool get hasActiveSession {
    if (_handle == null) return false;
    return _bindings.hasActiveSession(_handle!) != 0;
  }

  @override
  Future<bool> startRecording() async {
    if (_handle == null || !_voiceConfigured) return false;
    final ok = _bindings.startRecording(_handle!);
    if (ok != 0) {
      _recording = true;
    }
    return ok != 0;
  }

  @override
  Future<String?> stopRecordingAndTranscribe() async {
    if (_handle == null || !_recording) return null;
    _recording = false;
    // Complete any pending completer before starting a new one.
    _transcriptionCompleter?.complete(null);
    _transcriptionCompleter = Completer<String?>();
    _bindings.stopRecording(_handle!);
    return _transcriptionCompleter!.future;
  }

  @override
  Future<void> speakText(String text) async {
    if (!_voiceConfigured || text.isEmpty || _handle == null) return;
    final textPtr = text.toNativeUtf8();
    try {
      _bindings.speak(_handle!, textPtr);
    } finally {
      calloc.free(textPtr);
    }
    _pollForAudio();
  }

  // Poll shizuru_peek_audio_size() / shizuru_take_audio_into() until WAV bytes
  // arrive, then play via AudioService.  Dart owns the buffer — no cross-DLL
  // malloc/free required.
  void _pollForAudio() {
    _audioPoller?.cancel();
    var attempts = 0;
    const maxAttempts = 150; // 30 s at 200 ms
    _audioPoller = Timer.periodic(const Duration(milliseconds: 200), (timer) {
      if (_handle == null) { timer.cancel(); return; }
      try {
        final size = _bindings.peekAudioSize(_handle!);
        if (size > 0) {
          timer.cancel();
          final buf = calloc<Uint8>(size);
          try {
            final copied = _bindings.takeAudioInto(_handle!, buf, size);
            if (copied > 0) {
              final audio = Uint8List.fromList(buf.asTypedList(copied));
              _audioService.playAudioBytes(audio, 'audio/wav').ignore();
            }
          } finally {
            calloc.free(buf);
          }
        } else if (++attempts >= maxAttempts) {
          timer.cancel();
        }
      } catch (e, s) {
        timer.cancel();
      }
    });
  }

  @override
  Future<void> stopSpeaking() async {
    if (_handle == null) return;
    _bindings.stopSpeaking(_handle!);
  }

  @override
  bool get isSpeaking => false; // TODO: expose device state query in C API
  @override
  bool get isRecording => _recording;

  void _disposeNativeCallbacks() {
    _nativeOutputCb?.close();
    _nativeOutputCb = null;
    _nativeStateCb?.close();
    _nativeStateCb = null;
    _nativeToolCallCb?.close();
    _nativeToolCallCb = null;
    _nativeTranscriptionCb?.close();
    _nativeTranscriptionCb = null;
    _transcriptionCompleter?.complete(null);
    _transcriptionCompleter = null;
    _recording = false;
    _audioPoller?.cancel();
    _audioPoller = null;
  }

  /// Call when the bridge is no longer needed.
  void dispose() {
    _disposeNativeCallbacks();
    if (_handle != null) {
      _bindings.destroy(_handle!);
      _handle = null;
    }
  }
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
