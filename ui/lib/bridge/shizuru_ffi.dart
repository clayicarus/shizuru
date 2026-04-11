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
typedef _StartPlayoutNative = Int32 Function(Pointer handle);
typedef _StopPlayoutNative = Int32 Function(Pointer handle);
typedef _SetVoiceInputNative = Int32 Function(Pointer handle, Int32 enable);
typedef _SetVoiceOutputNative = Int32 Function(Pointer handle, Int32 enable);
typedef _SetAudioLevelCallbackNative = Void Function(
    Pointer handle, Pointer<NativeFunction<_AudioLevelCallbackNative>> cb,
    Pointer userData);
typedef _SetTranscriptCallbackNative = Void Function(
    Pointer handle, Pointer<NativeFunction<_TranscriptCallbackNative>> cb,
    Pointer userData);
typedef _SetDiagnosticCallbackNative = Void Function(
    Pointer handle, Pointer<NativeFunction<_DiagnosticCallbackNative>> cb,
    Pointer userData);

// Callback native types
typedef _OutputCallbackNative = Void Function(
    Pointer<Utf8> text, Int32 isPartial, Pointer userData);
typedef _StateCallbackNative = Void Function(Int32 state, Pointer userData);
typedef _AudioLevelCallbackNative = Void Function(Float rms, Pointer userData);
typedef _TranscriptCallbackNative = Void Function(
    Pointer<Utf8> text, Pointer userData);
typedef _DiagnosticCallbackNative = Void Function(
    Pointer<Utf8> message, Pointer userData);

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
typedef _StartPlayoutDart = int Function(Pointer handle);
typedef _StopPlayoutDart = int Function(Pointer handle);
typedef _SetVoiceInputDart = int Function(Pointer handle, int enable);
typedef _SetVoiceOutputDart = int Function(Pointer handle, int enable);
typedef _SetAudioLevelCallbackDart = void Function(
    Pointer handle, Pointer<NativeFunction<_AudioLevelCallbackNative>> cb,
    Pointer userData);
typedef _SetTranscriptCallbackDart = void Function(
    Pointer handle, Pointer<NativeFunction<_TranscriptCallbackNative>> cb,
    Pointer userData);
typedef _SetDiagnosticCallbackDart = void Function(
    Pointer handle, Pointer<NativeFunction<_DiagnosticCallbackNative>> cb,
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
  late final _StartPlayoutDart _startPlayout;
  late final _StopPlayoutDart _stopPlayout;
  late final _SetVoiceInputDart _setVoiceInput;
  late final _SetVoiceOutputDart _setVoiceOutput;
  late final _SetAudioLevelCallbackDart _setAudioLevelCallback;
  late final _SetTranscriptCallbackDart _setTranscriptCallback;
  late final _SetDiagnosticCallbackDart _setDiagnosticCallback;

  // Keep NativeCallable references alive
  NativeCallable<_OutputCallbackNative>? _outputCallable;
  NativeCallable<_StateCallbackNative>? _stateCallable;
  NativeCallable<_AudioLevelCallbackNative>? _audioLevelCallable;
  NativeCallable<_TranscriptCallbackNative>? _transcriptCallable;
  NativeCallable<_DiagnosticCallbackNative>? _diagnosticCallable;

  ShizuruBridge._(this._handle, this._lib) {
    _bindFunctions();
  }

  static DynamicLibrary _load() {
    if (Platform.isMacOS) {
      return DynamicLibrary.open('libshizuru_bridge.dylib');
    } else if (Platform.isLinux || Platform.isAndroid) {
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
    _startPlayout = _lib.lookupFunction<_StartPlayoutNative, _StartPlayoutDart>(
        'shizuru_start_playout');
    _stopPlayout = _lib.lookupFunction<_StopPlayoutNative, _StopPlayoutDart>(
        'shizuru_stop_playout');
    _setVoiceInput = _lib.lookupFunction<_SetVoiceInputNative, _SetVoiceInputDart>(
        'shizuru_set_voice_input');
    _setVoiceOutput = _lib.lookupFunction<_SetVoiceOutputNative, _SetVoiceOutputDart>(
        'shizuru_set_voice_output');
    _setAudioLevelCallback = _lib.lookupFunction<_SetAudioLevelCallbackNative,
        _SetAudioLevelCallbackDart>('shizuru_set_audio_level_callback');
    _setTranscriptCallback = _lib.lookupFunction<_SetTranscriptCallbackNative,
        _SetTranscriptCallbackDart>('shizuru_set_transcript_callback');
    _setDiagnosticCallback = _lib.lookupFunction<_SetDiagnosticCallbackNative,
        _SetDiagnosticCallbackDart>('shizuru_set_diagnostic_callback');
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
  int startPlayout() => _startPlayout(_handle);
  int stopPlayout() => _stopPlayout(_handle);
  int setVoiceInput(bool enable) => _setVoiceInput(_handle, enable ? 1 : 0);
  int setVoiceOutput(bool enable) => _setVoiceOutput(_handle, enable ? 1 : 0);

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

  void onTranscript(void Function(String text) callback) {
    _transcriptCallable?.close();
    _transcriptCallable = NativeCallable<_TranscriptCallbackNative>.listener(
      (Pointer<Utf8> textPtr, Pointer _) {
        final text = textPtr.toDartString();
        _lib.lookupFunction<Void Function(Pointer), void Function(Pointer)>(
            'shizuru_free_string')(textPtr);
        callback(text);
      },
    );
    _setTranscriptCallback(
        _handle, _transcriptCallable!.nativeFunction, nullptr);
  }

  void onDiagnostic(void Function(String message) callback) {
    _diagnosticCallable?.close();
    _diagnosticCallable = NativeCallable<_DiagnosticCallbackNative>.listener(
      (Pointer<Utf8> msgPtr, Pointer _) {
        final msg = msgPtr.toDartString();
        _lib.lookupFunction<Void Function(Pointer), void Function(Pointer)>(
            'shizuru_free_string')(msgPtr);
        callback(msg);
      },
    );
    _setDiagnosticCallback(
        _handle, _diagnosticCallable!.nativeFunction, nullptr);
  }
}
