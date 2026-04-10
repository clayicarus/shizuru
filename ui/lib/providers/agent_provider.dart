import 'dart:async';
import 'package:flutter/foundation.dart';
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
  double _audioLevel = 0.0;
  void Function(String text, bool isPartial)? _outputCallback;
  void Function(String text)? _transcriptCallback;

  // Activity tracking for fine-grained UI status.
  String _activity = '';
  DateTime? _lastTranscriptTime;
  Timer? _activityTimer;

  // State transition log for debug panel.
  final List<ActivityLogEntry> _activityLog = [];
  static const int _maxLogEntries = 200;

  AgentState get state => _state;
  bool get captureActive => _captureActive;
  double get audioLevel => _audioLevel;
  bool get isInitialized => _bridge != null;

  /// Human-readable activity string for the UI.
  /// Returns empty string when there's nothing special to show.
  String get activity => _activity;

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
    _audioLevel = 0.0;
    _activity = '';
    _activityTimer?.cancel();
    notifyListeners();

    final bridge = ShizuruBridge.create(config);

    bridge.onStateChange((state) {
      final prev = _state;
      _state = state;
      if (prev != state) {
        _log('${prev.displayName} → ${state.displayName}');
      }
      _updateActivity();
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
        _lastTranscriptTime = DateTime.now();
        _log('ASR: "$text"');
        _startActivityTimer();
        _transcriptCallback!(text);
      });
    }

    // Wire diagnostic callback from C++ core → activity log.
    bridge.onDiagnostic((message) {
      _log(message);
      notifyListeners();
    });

    bridge.start();
    _bridge = bridge;
    notifyListeners();
  }

  void sendMessage(String text) {
    _bridge?.sendMessage(text);
  }

  void toggleCapture() {
    if (_bridge == null) return;
    if (_captureActive) {
      _bridge!.stopCapture();
      _captureActive = false;
    } else {
      _bridge!.startCapture();
      _captureActive = true;
    }
    notifyListeners();
  }

  void _updateActivity() {
    switch (_state) {
      case AgentState.listening:
        // Check if we recently received a transcript (filter may be buffering).
        if (_lastTranscriptTime != null &&
            DateTime.now().difference(_lastTranscriptTime!).inSeconds < 6) {
          _activity = 'Waiting for more input...';
        } else {
          _activity = '';
        }
        break;
      case AgentState.thinking:
        _activity = 'Thinking...';
        _lastTranscriptTime = null;
        break;
      case AgentState.acting:
        _activity = 'Using tool...';
        break;
      case AgentState.responding:
        _activity = 'Responding...';
        break;
      case AgentState.routing:
        _activity = 'Planning...';
        break;
      case AgentState.error:
        _activity = 'Error occurred';
        break;
      default:
        _activity = '';
        _lastTranscriptTime = null;
    }
  }

  void _startActivityTimer() {
    _activityTimer?.cancel();
    // Poll activity status while in listening + recent transcript.
    _activityTimer = Timer.periodic(const Duration(milliseconds: 500), (_) {
      if (_state == AgentState.listening && _lastTranscriptTime != null) {
        final elapsed = DateTime.now().difference(_lastTranscriptTime!);
        if (elapsed.inSeconds >= 6) {
          _activity = '';
          _lastTranscriptTime = null;
          _activityTimer?.cancel();
          notifyListeners();
        } else if (_activity != 'Waiting for more input...') {
          _activity = 'Waiting for more input...';
          notifyListeners();
        }
      } else {
        _activityTimer?.cancel();
      }
    });
    _updateActivity();
    notifyListeners();
  }

  @override
  void dispose() {
    _activityTimer?.cancel();
    _bridge?.destroy();
    _bridge = null;
    super.dispose();
  }
}
