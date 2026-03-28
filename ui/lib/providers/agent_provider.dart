import 'package:flutter/foundation.dart';
import '../bridge/agent_state.dart';
import '../bridge/bridge_config.dart';
import '../bridge/shizuru_ffi.dart';

class AgentProvider extends ChangeNotifier {
  ShizuruBridge? _bridge;
  AgentState _state = AgentState.idle;
  bool _captureActive = false;
  double _audioLevel = 0.0;
  void Function(String text, bool isPartial)? _outputCallback;

  AgentState get state => _state;
  bool get captureActive => _captureActive;
  double get audioLevel => _audioLevel;
  bool get isInitialized => _bridge != null;

  void setOutputCallback(void Function(String text, bool isPartial) cb) {
    _outputCallback = cb;
    _bridge?.onOutput(cb);
  }

  Future<void> initialize(BridgeConfig config) async {
    _bridge?.destroy();
    _bridge = null;
    _state = AgentState.idle;
    _captureActive = false;
    _audioLevel = 0.0;
    notifyListeners();

    final bridge = ShizuruBridge.create(config);

    bridge.onStateChange((state) {
      _state = state;
      notifyListeners();
    });

    bridge.onAudioLevel((rms) {
      _audioLevel = rms / 32767.0;
      notifyListeners();
    });

    if (_outputCallback != null) {
      bridge.onOutput(_outputCallback!);
    }

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

  @override
  void dispose() {
    _bridge?.destroy();
    _bridge = null;
    super.dispose();
  }
}
