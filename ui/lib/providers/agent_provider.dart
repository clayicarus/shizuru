import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';
import '../bridge/activity_kind.dart';
import '../bridge/agent_state.dart';
import '../bridge/bridge_config.dart';
import '../bridge/shizuru_ffi.dart';

class ActivityLogEntry {
  final DateTime time;
  final String message;
  ActivityLogEntry(this.time, this.message);
}

class AgentProvider extends ChangeNotifier {
  ShizuruBridge? _bridge;
  AgentState _state = AgentState.idle;
  bool _captureActive = false;
  bool _playoutActive = false;
  double _audioLevel = 0.0;
  void Function(String text, bool isPartial)? _outputCallback;
  void Function(String text)? _transcriptCallback;

  // Activity tracking for fine-grained UI status.
  String _activity = '';

  // Thinking mode toggle (UI-only for now, bridge API TBD).
  bool _thinkingEnabled = false;

  // Forward activity events to other providers (e.g. ConversationProvider).
  void Function(int kind, String detail)? _activityForwardCallback;

  // State transition log for debug panel.
  final List<ActivityLogEntry> _activityLog = [];
  static const int _maxLogEntries = 200;

  AgentState get state => _state;
  bool get captureActive => _captureActive;
  bool get playoutActive => _playoutActive;
  double get audioLevel => _audioLevel;
  bool get isInitialized => _bridge != null;

  /// Human-readable activity string for the UI.
  /// Returns empty string when there's nothing special to show.
  String get activity => _activity;

  bool get thinkingEnabled => _thinkingEnabled;

  void toggleThinking() {
    _thinkingEnabled = !_thinkingEnabled;
    notifyListeners();
  }

  void setActivityForwardCallback(void Function(int kind, String detail) cb) {
    _activityForwardCallback = cb;
  }

  /// Activity log entries for the debug panel.
  List<ActivityLogEntry> get activityLog => List.unmodifiable(_activityLog);

  void _log(String message) {
    _activityLog.add(ActivityLogEntry(DateTime.now(), message));
    if (_activityLog.length > _maxLogEntries) {
      _activityLog.removeAt(0);
    }
  }

  void setOutputCallback(void Function(String text, bool isPartial) cb) {
    _outputCallback = cb;
    _bridge?.onOutput(cb);
  }

  void setTranscriptCallback(void Function(String text) cb) {
    _transcriptCallback = cb;
    _bridge?.onTranscript(cb);
  }

  Future<void> initialize(BridgeConfig config) async {
    _bridge?.destroy();
    _bridge = null;
    _state = AgentState.idle;
    _captureActive = false;
    _playoutActive = false;
    _audioLevel = 0.0;
    _activity = '';
    notifyListeners();

    try {
      final bridge = ShizuruBridge.create(config);

      bridge.onStateChange((state) {
        final prev = _state;
        _state = state;
        if (prev != state) {
          _log('${prev.displayName} → ${state.displayName}');
        }
        notifyListeners();
      });

      bridge.onAudioLevel((rms) {
        _audioLevel = rms / 32767.0;
        notifyListeners();
      });

      if (_outputCallback != null) {
        bridge.onOutput(_outputCallback!);
      }

      if (_transcriptCallback != null) {
        bridge.onTranscript((text) {
          _log('ASR: "$text"');
          _transcriptCallback!(text);
        });
      }

      // Wire diagnostic callback from C++ core → activity log.
      bridge.onDiagnostic((message) {
        _log(message);
        notifyListeners();
      });

      // Wire structured activity callback from C++ core → UI activity status.
      bridge.onActivity(_onActivity);

      bridge.start();
      _bridge = bridge;

      notifyListeners();
    } catch (e) {
      _log('Initialization failed: $e');
      notifyListeners();
      rethrow;
    }
  }

  void sendMessage(String text) {
    _bridge?.sendMessage(text);
  }

  static const _audioChannel = MethodChannel('com.example.shizuru_ui/audio');

  void toggleCapture() {
    if (_bridge == null) return;
    _captureActive = !_captureActive;
    _bridge!.setVoiceInput(_captureActive);
    notifyListeners();
  }

  void togglePlayout() {
    if (_bridge == null) return;
    _playoutActive = !_playoutActive;
    _bridge!.setVoiceOutput(_playoutActive);
    notifyListeners();
  }

  /// Set up audio routing and start both capture + playout in the correct order.
  /// On Android: set speakerphone → start playout → start capture.
  Future<void> startAllAudio() async {
    if (Platform.isAndroid || Platform.isIOS) {
      final status = await Permission.microphone.request();
      if (!status.isGranted) {
        _log('Microphone permission denied');
        notifyListeners();
        return;
      }
    }
    if (Platform.isAndroid) {
      try {
        await _audioChannel.invokeMethod('setSpeakerphoneOn', {'on': true});
        _log('Speakerphone enabled');
      } catch (_) {}
    }
    _bridge?.startPlayout();
    _log('Playout device started');
    _bridge?.startCapture();
    _log('Capture device started');
    notifyListeners();
  }

  void stopAllAudio() {
    _bridge?.stopCapture();
    _bridge?.stopPlayout();
    _log('All audio devices stopped');
    notifyListeners();
  }

  void _onActivity(int kind, String detail) {
    final ak = ActivityKindExtension.fromInt(kind);
    switch (ak) {
      case ActivityKind.bufferingInput:
        _activity = 'Waiting for more input...';
        break;
      case ActivityKind.filteringInput:
        _activity = 'Evaluating input...';
        break;
      case ActivityKind.thinkingStarted:
        _activity = 'Thinking...';
        break;
      case ActivityKind.thinkingRetry:
        _activity = 'Retrying (attempt $detail)...';
        break;
      case ActivityKind.toolDispatched:
        _activity = detail.isNotEmpty ? 'Using tool: $detail' : 'Using tool...';
        break;
      case ActivityKind.toolResultReceived:
        _activity = 'Tool result received';
        break;
      case ActivityKind.speaking:
        _activity = 'Speaking...';
        break;
      case ActivityKind.interrupted:
        _activity = 'Interrupted';
        break;
      case ActivityKind.turnComplete:
        _activity = '';
        break;
      case ActivityKind.budgetExhausted:
        _activity = 'Turn limit reached';
        break;
    }
    _activityForwardCallback?.call(kind, detail);
    notifyListeners();
  }

  @override
  void dispose() {
    _bridge?.destroy();
    _bridge = null;
    super.dispose();
  }
}
