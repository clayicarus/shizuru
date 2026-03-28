import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';
import 'agent_state.dart';
import 'bridge_config.dart';

// ---------------------------------------------------------------------------
// Native function typedefs (C side)
// ---------------------------------------------------------------------------

typedef _CreateNative = Pointer Function(Pointer<Utf8> configJson,
    Pointer<Utf8> errorBuf, Int32 errorBufLen);
typedef _StartNative = Int32 Function(Pointer handle);
typedef _DestroyNative = Void Function(Pointer handle);
typedef _SendMessageNative = Int32 Function(
    Pointer handle, Pointer<Utf8> text);
typedef _GetStateNative = Int32 Function(Pointer handle);
typedef _SetOutputCallbackNative = Void Function(
    Pointer handle, Pointer<NativeFunction<_OutputCallbackNative>> cb,
    Pointer userData);
typedef _SetStateCallbackNative = Void Function(
    Pointer handle, Pointer<NativeFunction<_StateCallbackNative>> cb,
    Pointer userData);
typedef _StartCaptureNative = Int32 Function(Pointer handle);
typedef _StopCaptureNative = Int32 Function(Pointer handle);
typedef _SetAudioLevelCallbackNative = Void Function(
    Pointer handle, Pointer<NativeFunction<_AudioLevelCallbackNative>> cb,
    Pointer userData);

// Callback native types
typedef _OutputCallbackNative = Void Function(
    Pointer<Utf8> text, Int32 isPartial, Pointer userData);
typedef _StateCallbackNative = Void Function(Int32 state, Pointer userData);
typedef _AudioLevelCallbackNative = Void Function(Float rms, Pointer userData);

// ---------------------------------------------------------------------------
// Dart function typedefs
// ---------------------------------------------------------------------------

typedef _CreateDart = Pointer Function(Pointer<Utf8> configJson,
    Pointer<Utf8> errorBuf, int errorBufLen);
typedef _StartDart = int Function(Pointer handle);
typedef _DestroyDart = void Function(Pointer handle);
typedef _SendMessageDart = int Function(Pointer handle, Pointer<Utf8> text);
typedef _GetStateDart = int Function(Pointer handle);
typedef _SetOutputCallbackDart = void Function(
    Pointer handle, Pointer<NativeFunction<_OutputCallbackNative>> cb,
    Pointer userData);
typedef _SetStateCallbackDart = void Function(
    Pointer handle, Pointer<NativeFunction<_StateCallbackNative>> cb,
    Pointer userData);
typedef _StartCaptureDart = int Function(Pointer handle);
typedef _StopCaptureDart = int Function(Pointer handle);
typedef _SetAudioLevelCallbackDart = void Function(
    Pointer handle, Pointer<NativeFunction<_AudioLevelCallbackNative>> cb,
    Pointer userData);

// ---------------------------------------------------------------------------
// ShizuruBridge
// ---------------------------------------------------------------------------

class ShizuruBridge {
  final Pointer _handle;
  final DynamicLibrary _lib;

  // Bound functions
  late final _StartDart _start;
  late final _DestroyDart _destroy;
  late final _SendMessageDart _sendMessage;
  late final _GetStateDart _getState;
  late final _SetOutputCallbackDart _setOutputCallback;
  late final _SetStateCallbackDart _setStateCallback;
  late final _StartCaptureDart _startCapture;
  late final _StopCaptureDart _stopCapture;
  late final _SetAudioLevelCallbackDart _setAudioLevelCallback;

  // Keep NativeCallable references alive
  NativeCallable<_OutputCallbackNative>? _outputCallable;
  NativeCallable<_StateCallbackNative>? _stateCallable;
  NativeCallable<_AudioLevelCallbackNative>? _audioLevelCallable;

  ShizuruBridge._(this._handle, this._lib) {
    _bindFunctions();
  }

  static DynamicLibrary _load() {
    if (Platform.isMacOS) {
      return DynamicLibrary.open('libshizuru_bridge.dylib');
    } else if (Platform.isLinux) {
      return DynamicLibrary.open('libshizuru_bridge.so');
    } else if (Platform.isWindows) {
      return DynamicLibrary.open('shizuru_bridge.dll');
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }

  factory ShizuruBridge.create(BridgeConfig config) {
    final lib = _load();
    final createFn = lib.lookupFunction<_CreateNative, _CreateDart>(
        'shizuru_create');

    const int errorBufLen = 512;
    final configJsonPtr = config.toJson().toNativeUtf8();
    final errorBuf = calloc<Uint8>(errorBufLen);

    try {
      final handle = createFn(
          configJsonPtr, errorBuf.cast<Utf8>(), errorBufLen);
      if (handle == nullptr) {
        final errorMsg = errorBuf.cast<Utf8>().toDartString();
        throw StateError('shizuru_create failed: $errorMsg');
      }
      return ShizuruBridge._(handle, lib);
    } finally {
      calloc.free(configJsonPtr);
      calloc.free(errorBuf);
    }
  }

  void _bindFunctions() {
    _start = _lib.lookupFunction<_StartNative, _StartDart>('shizuru_start');
    _destroy = _lib.lookupFunction<_DestroyNative, _DestroyDart>('shizuru_destroy');
    _sendMessage = _lib.lookupFunction<_SendMessageNative, _SendMessageDart>(
        'shizuru_send_message');
    _getState = _lib.lookupFunction<_GetStateNative, _GetStateDart>(
        'shizuru_get_state');
    _setOutputCallback = _lib.lookupFunction<_SetOutputCallbackNative,
        _SetOutputCallbackDart>('shizuru_set_output_callback');
    _setStateCallback = _lib.lookupFunction<_SetStateCallbackNative,
        _SetStateCallbackDart>('shizuru_set_state_callback');
    _startCapture = _lib.lookupFunction<_StartCaptureNative, _StartCaptureDart>(
        'shizuru_start_capture');
    _stopCapture = _lib.lookupFunction<_StopCaptureNative, _StopCaptureDart>(
        'shizuru_stop_capture');
    _setAudioLevelCallback = _lib.lookupFunction<_SetAudioLevelCallbackNative,
        _SetAudioLevelCallbackDart>('shizuru_set_audio_level_callback');
  }

  int start() => _start(_handle);

  void destroy() {
    _outputCallable?.close();
    _stateCallable?.close();
    _audioLevelCallable?.close();
    _outputCallable = null;
    _stateCallable = null;
    _audioLevelCallable = null;
    _destroy(_handle);
  }

  int sendMessage(String text) {
    final ptr = text.toNativeUtf8();
    try {
      return _sendMessage(_handle, ptr);
    } finally {
      calloc.free(ptr);
    }
  }

  AgentState getState() {
    final raw = _getState(_handle);
    return AgentStateExtension.fromInt(raw);
  }

  void onOutput(void Function(String text, bool isPartial) callback) {
    _outputCallable?.close();
    final freeString = _lib.lookupFunction<
        Void Function(Pointer<Utf8>),
        void Function(Pointer<Utf8>)>('shizuru_free_string');
    _outputCallable = NativeCallable<_OutputCallbackNative>.listener(
      (Pointer<Utf8> text, int isPartial, Pointer _) {
        // Copy the string content immediately before freeing the C memory.
        final dartStr = text.toDartString();
        freeString(text);
        callback(dartStr, isPartial != 0);
      },
    );
    _setOutputCallback(_handle, _outputCallable!.nativeFunction, nullptr);
  }

  void onStateChange(void Function(AgentState state) callback) {
    _stateCallable?.close();
    _stateCallable = NativeCallable<_StateCallbackNative>.listener(
      (int state, Pointer _) {
        callback(AgentStateExtension.fromInt(state));
      },
    );
    _setStateCallback(_handle, _stateCallable!.nativeFunction, nullptr);
  }

  int startCapture() => _startCapture(_handle);
  int stopCapture() => _stopCapture(_handle);

  void onAudioLevel(void Function(double rms) callback) {
    _audioLevelCallable?.close();
    _audioLevelCallable = NativeCallable<_AudioLevelCallbackNative>.listener(
      (double rms, Pointer _) {
        callback(rms);
      },
    );
    _setAudioLevelCallback(
        _handle, _audioLevelCallable!.nativeFunction, nullptr);
  }
}
