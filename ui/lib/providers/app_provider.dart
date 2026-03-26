import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
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
    this.systemPrompt = defaultSystemPrompt,
  });

  static const String defaultSystemPrompt =
      '你是 Shizuru，一个友好的语音 AI 助手。'
      '你的每一条回复都会通过语音合成（TTS）朗读给用户，所以请始终用简洁自然的口语中文回答，避免使用 Markdown 格式（不要用 **加粗**、列表符号等）。'
      '你完全具备语音输出能力，不要说自己不能发送语音。';

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

  static const _kLlmBaseUrl    = 'llm_base_url';
  static const _kLlmApiKey    = 'llm_api_key';
  static const _kLlmModel     = 'llm_model';
  static const _kLlmApiPath   = 'llm_api_path';
  static const _kAsrApiKey    = 'asr_api_key';
  static const _kAsrSecretKey = 'asr_secret_key';
  static const _kTtsApiKey    = 'tts_api_key';
  static const _kTtsVoiceId   = 'tts_voice_id';
  static const _kSystemPrompt = 'system_prompt';

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kLlmBaseUrl,    llmBaseUrl);
    await prefs.setString(_kLlmApiKey,    llmApiKey);
    await prefs.setString(_kLlmModel,     llmModel);
    await prefs.setString(_kLlmApiPath,   llmApiPath);
    await prefs.setString(_kAsrApiKey,    asrApiKey);
    await prefs.setString(_kAsrSecretKey, asrSecretKey);
    await prefs.setString(_kTtsApiKey,    ttsApiKey);
    await prefs.setString(_kTtsVoiceId,   ttsVoiceId);
    await prefs.setString(_kSystemPrompt, systemPrompt);
  }

  static Future<AppSettings> load() async {
    final prefs = await SharedPreferences.getInstance();
    final s = AppSettings();
    s.llmBaseUrl    = prefs.getString(_kLlmBaseUrl)    ?? s.llmBaseUrl;
    s.llmApiKey     = prefs.getString(_kLlmApiKey)     ?? s.llmApiKey;
    s.llmModel      = prefs.getString(_kLlmModel)      ?? s.llmModel;
    s.llmApiPath    = prefs.getString(_kLlmApiPath)    ?? s.llmApiPath;
    s.asrApiKey     = prefs.getString(_kAsrApiKey)     ?? s.asrApiKey;
    s.asrSecretKey  = prefs.getString(_kAsrSecretKey)  ?? s.asrSecretKey;
    s.ttsApiKey     = prefs.getString(_kTtsApiKey)     ?? s.ttsApiKey;
    s.ttsVoiceId    = prefs.getString(_kTtsVoiceId)    ?? s.ttsVoiceId;
    s.systemPrompt  = prefs.getString(_kSystemPrompt)  ?? s.systemPrompt;
    return s;
  }
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
    _loadSettingsAndInit();
  }

  Future<void> _loadSettingsAndInit() async {
    final saved = await AppSettings.load();
    settings
      ..llmBaseUrl   = saved.llmBaseUrl
      ..llmApiKey    = saved.llmApiKey
      ..llmModel     = saved.llmModel
      ..llmApiPath   = saved.llmApiPath
      ..asrApiKey    = saved.asrApiKey
      ..asrSecretKey = saved.asrSecretKey
      ..ttsApiKey    = saved.ttsApiKey
      ..ttsVoiceId   = saved.ttsVoiceId
      ..systemPrompt = saved.systemPrompt;
    await _newSessionInternal();
  }

  Future<void> applySettings() async {
    await settings.save();
    await _newSessionInternal();
  }

  // ── Bridge callbacks ──────────────────────────────────────────────────────

  void _registerBridgeCallbacks() {
    final bridge = ShizuruBridge.instance;

    bridge.onStateChange((cppState) {
      final prevState = _agentState;
      _agentState = AgentState.values[cppState.index];
      _addLog('[State] → ${_agentState.label}');
      // FfiBridge: sendMessage() is fire-and-forget, so TTS must fire here
      // when the agent finishes responding (responding → idle/listening).
      if (bridge is FfiBridge &&
          prevState == AgentState.responding &&
          (_agentState == AgentState.idle ||
           _agentState == AgentState.listening)) {
        _triggerTtsIfEnabled();
      }
      notifyListeners();
    });

    bridge.onOutput((text) {
      if (_currentSession == null) return;

      // Special tool-call notification from mock bridge (\x00tool:<name>)
      if (text.startsWith('\x00tool:')) {
        final toolName = text.substring(6);
        _addToolCallMessage(toolName);
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

    // Register tool call notification from C++ core (FfiBridge only).
    if (bridge is FfiBridge) {
      bridge.onToolCall((toolName, arguments) {
        _addLog('[Tool] $toolName($arguments)');
        _addToolCallMessage(toolName);
      });
    }
  }

  void _addToolCallMessage(String toolName) {
    if (_currentSession == null) return;
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

  void _triggerTtsIfEnabled() {
    if (!enableTts || _currentSession == null) return;
    final msgs = _currentSession!.messages;
    if (msgs.isEmpty) return;
    final lastMsg = msgs.lastWhere(
      (m) => m.isAssistant && m.content.isNotEmpty,
      orElse: () => msgs.last,
    );
    if (lastMsg.isAssistant && lastMsg.content.isNotEmpty) {
      _addLog('[TTS] Speaking ${lastMsg.content.length} chars');
      ShizuruBridge.instance.speakText(lastMsg.content);
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
