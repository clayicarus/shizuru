import 'package:flutter/material.dart';

import '../bridge/shizuru_bridge.dart';
import '../models/agent_state.dart';
import '../models/message.dart';
import '../models/session.dart';

// ─── Settings ─────────────────────────────────────────────────────────────────

class AppSettings {
  String llmBaseUrl;
  String llmApiKey;
  String llmModel;
  String llmApiPath;
  String asrApiKey;
  String asrSecretKey;
  String ttsApiKey;
  String ttsVoiceId;
  String systemPrompt;

  AppSettings({
    this.llmBaseUrl = 'https://dashscope.aliyuncs.com',
    this.llmApiKey = '',
    this.llmModel = 'qwen3-coder-next',
    this.llmApiPath = '/compatible-mode/v1/chat/completions',
    this.asrApiKey = '',
    this.asrSecretKey = '',
    this.ttsApiKey = '',
    this.ttsVoiceId = '',
    this.systemPrompt =
        '你是 Shizuru，一个友好的 AI 助手。用简洁自然的中文回答问题。',
  });

  RuntimeConfig toRuntimeConfig() => RuntimeConfig(
        llmBaseUrl: llmBaseUrl,
        llmApiKey: llmApiKey,
        llmModel: llmModel,
        llmApiPath: llmApiPath,
        asrApiKey: asrApiKey.isNotEmpty ? asrApiKey : null,
        asrSecretKey: asrSecretKey.isNotEmpty ? asrSecretKey : null,
        ttsApiKey: ttsApiKey.isNotEmpty ? ttsApiKey : null,
        ttsVoiceId: ttsVoiceId.isNotEmpty ? ttsVoiceId : null,
        systemPrompt: systemPrompt,
      );
}

// ─── Input mode ───────────────────────────────────────────────────────────────

enum InputMode { text, voice }

// ─── AppProvider ──────────────────────────────────────────────────────────────

class AppProvider extends ChangeNotifier {
  // Settings
  final AppSettings settings = AppSettings();

  // Sessions
  final List<ChatSession> _sessions = [];
  ChatSession? _currentSession;

  // Agent state
  AgentState _agentState = AgentState.idle;

  // UI toggles
  bool showSidebar = true;
  bool showDebugPanel = false;
  InputMode inputMode = InputMode.text;

  // Debug info
  final List<String> logs = [];
  int tokenUsed = 0;
  final List<String> toolCallLog = [];

  // Voice state
  bool _isRecording = false;
  bool get isRecording => _isRecording;
  bool get isSpeaking => ShizuruBridge.instance.isSpeaking;
  bool enableTts = true;

  List<ChatSession> get sessions => List.unmodifiable(_sessions);
  ChatSession? get currentSession => _currentSession;
  AgentState get agentState => _agentState;

  AppProvider() {
    _registerBridgeCallbacks();
    _newSessionInternal();
  }

  // ── Bridge callbacks ──────────────────────────────────────────────────────

  void _registerBridgeCallbacks() {
    ShizuruBridge.instance.onStateChange((cppState) {
      _agentState = AgentState.values[cppState.index];
      _addLog('[State] → ${_agentState.label}');
      notifyListeners();
    });

    ShizuruBridge.instance.onOutput((text) {
      if (_currentSession == null) return;

      // Special tool-call notification from mock bridge (\x00tool:<name>)
      if (text.startsWith('\x00tool:')) {
        final toolName = text.substring(6);
        final toolMsg = ChatMessage(
          id: _uuid(),
          role: MessageRole.toolCall,
          content: toolName,
          toolName: toolName,
          timestamp: DateTime.now(),
        );
        _currentSession!.messages.add(toolMsg);
        toolCallLog.add('⚙ $toolName');
        notifyListeners();
        return;
      }

      // Streaming assistant text
      final msgs = _currentSession!.messages;
      final lastIsStreaming =
          msgs.isNotEmpty && msgs.last.isAssistant && msgs.last.isStreaming;

      if (lastIsStreaming) {
        msgs.last.content = text;
      } else {
        msgs.add(ChatMessage(
          id: _uuid(),
          role: MessageRole.assistant,
          content: text,
          timestamp: DateTime.now(),
          status: MessageStatus.streaming,
        ));
      }
      notifyListeners();
    });
  }

  // ── Session management ────────────────────────────────────────────────────

  Future<void> _newSessionInternal() async {
    final session = ChatSession(
      id: _uuid(),
      title: '新对话',
      createdAt: DateTime.now(),
    );
    _sessions.insert(0, session);
    _currentSession = session;
    await ShizuruBridge.instance.initRuntime(settings.toRuntimeConfig());
    await ShizuruBridge.instance.startSession();
    notifyListeners();
  }

  Future<void> newSession() async {
    _finalizeStreaming();
    await _newSessionInternal();
  }

  void loadSession(String id) {
    _finalizeStreaming();
    _currentSession = _sessions.firstWhere((s) => s.id == id);
    notifyListeners();
  }

  void _finalizeStreaming() {
    _currentSession?.messages
        .where((m) => m.isStreaming)
        .forEach((m) => m.status = MessageStatus.complete);
  }

  // ── Messaging ─────────────────────────────────────────────────────────────

  Future<void> sendTextMessage(String text) async {
    final trimmed = text.trim();
    if (trimmed.isEmpty) return;
    if (_currentSession == null) await _newSessionInternal();

    // Add user message
    _currentSession!.messages.add(ChatMessage(
      id: _uuid(),
      role: MessageRole.user,
      content: trimmed,
      timestamp: DateTime.now(),
    ));

    // Auto-title from first user message
    final userMsgs =
        _currentSession!.messages.where((m) => m.isUser).toList();
    if (userMsgs.length == 1) {
      _currentSession!.title =
          trimmed.length > 24 ? '${trimmed.substring(0, 24)}…' : trimmed;
    }

    tokenUsed += (trimmed.length / 4).ceil();
    _addLog('[User] ${trimmed.length} chars → bridge.sendMessage');
    notifyListeners();

    await ShizuruBridge.instance.sendMessage(trimmed);

    // Finalise last streaming message
    final msgs = _currentSession!.messages;
    if (msgs.isNotEmpty && msgs.last.isStreaming) {
      msgs.last.status = MessageStatus.complete;
    }
    notifyListeners();

    // Auto-TTS after LLM response
    if (enableTts && msgs.isNotEmpty) {
      final lastMsg = msgs.last;
      if (lastMsg.isAssistant && lastMsg.content.isNotEmpty) {
        _addLog('[TTS] Speaking ${lastMsg.content.length} chars');
        await ShizuruBridge.instance.speakText(lastMsg.content);
      }
    }
  }

  // ── UI toggle helpers ─────────────────────────────────────────────────────

  void toggleSidebar() {
    showSidebar = !showSidebar;
    notifyListeners();
  }

  void toggleDebugPanel() {
    showDebugPanel = !showDebugPanel;
    notifyListeners();
  }

  void setInputMode(InputMode mode) {
    inputMode = mode;
    notifyListeners();
  }

  // ── Voice controls ─────────────────────────────────────────────────────

  Future<void> toggleRecording() async {
    if (_isRecording) {
      await stopRecordingAndSend();
    } else {
      await startRecording();
    }
  }

  Future<void> startRecording() async {
    final ok = await ShizuruBridge.instance.startRecording();
    if (ok) {
      _isRecording = true;
      _addLog('[ASR] Recording started');
    } else {
      _addLog('[ASR] Failed to start recording (permission?)');
    }
    notifyListeners();
  }

  Future<void> stopRecordingAndSend() async {
    _isRecording = false;
    notifyListeners();

    _addLog('[ASR] Transcribing...');
    final transcript =
        await ShizuruBridge.instance.stopRecordingAndTranscribe();

    if (transcript != null && transcript.isNotEmpty) {
      _addLog('[ASR] Transcript: "$transcript"');
      await sendTextMessage(transcript);
    } else {
      _addLog('[ASR] No transcript returned');
      _agentState = AgentState.idle;
      notifyListeners();
    }
  }

  Future<void> speakMessage(String text) async {
    _addLog('[TTS] Manual speak: ${text.length} chars');
    await ShizuruBridge.instance.speakText(text);
  }

  Future<void> stopSpeaking() async {
    await ShizuruBridge.instance.stopSpeaking();
    notifyListeners();
  }

  void toggleTts() {
    enableTts = !enableTts;
    _addLog('[TTS] ${enableTts ? "enabled" : "disabled"}');
    notifyListeners();
  }

  // ── Internals ─────────────────────────────────────────────────────────────

  void _addLog(String msg) {
    final ts = DateTime.now().toLocal();
    final hms =
        '${ts.hour.toString().padLeft(2, '0')}:${ts.minute.toString().padLeft(2, '0')}:${ts.second.toString().padLeft(2, '0')}';
    logs.add('[$hms] $msg');
    if (logs.length > 200) logs.removeAt(0);
  }

  String _uuid() => DateTime.now().microsecondsSinceEpoch.toRadixString(16);
}
