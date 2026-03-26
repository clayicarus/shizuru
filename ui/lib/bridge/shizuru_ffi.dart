// Dart FFI bindings for the shizuru_api shared library (shizuru_api.dll).
//
// This file defines the native function signatures and loads the dynamic
// library.  It is consumed by _FfiBridge in shizuru_bridge.dart.

import 'dart:ffi';
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

// ---------------------------------------------------------------------------
// Native type definitions (match shizuru_api.h)
// ---------------------------------------------------------------------------

// Opaque handle
typedef ShizuruRuntime = Void;

// Callback types (C side)
typedef NativeOutputCallback = Void Function(Pointer<Utf8> text, Pointer<Void> userData);
typedef NativeStateCallback = Void Function(Int32 state, Pointer<Void> userData);
typedef NativeToolCallCallback = Void Function(
    Pointer<Utf8> toolName, Pointer<Utf8> arguments, Pointer<Void> userData);
// TODO: add NativeTranscriptionCallback once ASR pipeline is wired in shizuru_api.cpp

// ---------------------------------------------------------------------------
// Native function signatures (C)
// ---------------------------------------------------------------------------

// ShizuruRuntime* shizuru_create(const char* config_json);
typedef ShizuruCreateNative = Pointer<ShizuruRuntime> Function(Pointer<Utf8> configJson);
typedef ShizuruCreateDart = Pointer<ShizuruRuntime> Function(Pointer<Utf8> configJson);

// void shizuru_destroy(ShizuruRuntime* rt);
typedef ShizuruDestroyNative = Void Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruDestroyDart = void Function(Pointer<ShizuruRuntime> rt);

// const char* shizuru_start_session(ShizuruRuntime* rt);
typedef ShizuruStartSessionNative = Pointer<Utf8> Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruStartSessionDart = Pointer<Utf8> Function(Pointer<ShizuruRuntime> rt);

// void shizuru_shutdown(ShizuruRuntime* rt);
typedef ShizuruShutdownNative = Void Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruShutdownDart = void Function(Pointer<ShizuruRuntime> rt);

// int32_t shizuru_has_active_session(ShizuruRuntime* rt);
typedef ShizuruHasActiveSessionNative = Int32 Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruHasActiveSessionDart = int Function(Pointer<ShizuruRuntime> rt);

// void shizuru_send_message(ShizuruRuntime* rt, const char* content);
typedef ShizuruSendMessageNative = Void Function(Pointer<ShizuruRuntime> rt, Pointer<Utf8> content);
typedef ShizuruSendMessageDart = void Function(Pointer<ShizuruRuntime> rt, Pointer<Utf8> content);

// int32_t shizuru_get_state(ShizuruRuntime* rt);
typedef ShizuruGetStateNative = Int32 Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruGetStateDart = int Function(Pointer<ShizuruRuntime> rt);

// void shizuru_set_output_callback(ShizuruRuntime* rt, callback, void* user_data);
typedef ShizuruSetOutputCallbackNative = Void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeOutputCallback>> cb,
    Pointer<Void> userData);
typedef ShizuruSetOutputCallbackDart = void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeOutputCallback>> cb,
    Pointer<Void> userData);

// void shizuru_set_state_callback(ShizuruRuntime* rt, callback, void* user_data);
typedef ShizuruSetStateCallbackNative = Void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeStateCallback>> cb,
    Pointer<Void> userData);
typedef ShizuruSetStateCallbackDart = void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeStateCallback>> cb,
    Pointer<Void> userData);

// void shizuru_set_tool_call_callback(rt, cb, user_data);
typedef ShizuruSetToolCallCallbackNative = Void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeToolCallCallback>> cb,
    Pointer<Void> userData);
typedef ShizuruSetToolCallCallbackDart = void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeToolCallCallback>> cb,
    Pointer<Void> userData);

// void shizuru_register_tool(rt, name, description, params_json, handler, user_data);
// handler can be nullptr — we pass nullptr from Dart (tools run in C++).
typedef ShizuruRegisterToolNative = Void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<Utf8> name,
    Pointer<Utf8> description,
    Pointer<Utf8> paramsJson,
    Pointer<Void> handler,  // ShizuruToolHandler — nullptr from Dart
    Pointer<Void> userData);
typedef ShizuruRegisterToolDart = void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<Utf8> name,
    Pointer<Utf8> description,
    Pointer<Utf8> paramsJson,
    Pointer<Void> handler,
    Pointer<Void> userData);

// int32_t shizuru_setup_voice(rt, voice_config_json);
typedef ShizuruSetupVoiceNative = Int32 Function(
    Pointer<ShizuruRuntime> rt, Pointer<Utf8> voiceConfigJson);
typedef ShizuruSetupVoiceDart = int Function(
    Pointer<ShizuruRuntime> rt, Pointer<Utf8> voiceConfigJson);

// void shizuru_speak(rt, text);
typedef ShizuruSpeakNative = Void Function(
    Pointer<ShizuruRuntime> rt, Pointer<Utf8> text);
typedef ShizuruSpeakDart = void Function(
    Pointer<ShizuruRuntime> rt, Pointer<Utf8> text);

// void shizuru_stop_speaking(rt);
typedef ShizuruStopSpeakingNative = Void Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruStopSpeakingDart = void Function(Pointer<ShizuruRuntime> rt);

// void (*ShizuruTranscriptionCallback)(const char* transcript, void* user_data);
typedef NativeTranscriptionCallback = Void Function(
    Pointer<Utf8> transcript, Pointer<Void> userData);

// void shizuru_set_transcription_callback(rt, cb, user_data);
typedef ShizuruSetTranscriptionCallbackNative = Void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeTranscriptionCallback>> cb,
    Pointer<Void> userData);
typedef ShizuruSetTranscriptionCallbackDart = void Function(
    Pointer<ShizuruRuntime> rt,
    Pointer<NativeFunction<NativeTranscriptionCallback>> cb,
    Pointer<Void> userData);

// int32_t shizuru_start_recording(rt);
typedef ShizuruStartRecordingNative = Int32 Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruStartRecordingDart = int Function(Pointer<ShizuruRuntime> rt);

// void shizuru_stop_recording(rt);
typedef ShizuruStopRecordingNative = Void Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruStopRecordingDart = void Function(Pointer<ShizuruRuntime> rt);

// int64_t shizuru_peek_audio_size(rt);
typedef ShizuruPeekAudioSizeNative = Int64 Function(Pointer<ShizuruRuntime> rt);
typedef ShizuruPeekAudioSizeDart = int Function(Pointer<ShizuruRuntime> rt);

// int64_t shizuru_take_audio_into(rt, buf, buf_size);
typedef ShizuruTakeAudioIntoNative = Int64 Function(
    Pointer<ShizuruRuntime> rt, Pointer<Uint8> buf, Int64 bufSize);
typedef ShizuruTakeAudioIntoDart = int Function(
    Pointer<ShizuruRuntime> rt, Pointer<Uint8> buf, int bufSize);

// void shizuru_free_string(const char* str);
typedef ShizuruFreeStringNative = Void Function(Pointer<Utf8> str);
typedef ShizuruFreeStringDart = void Function(Pointer<Utf8> str);

// ---------------------------------------------------------------------------
// Bindings class
// ---------------------------------------------------------------------------

class ShizuruNativeBindings {
  late final DynamicLibrary _lib;

  late final ShizuruCreateDart create;
  late final ShizuruDestroyDart destroy;
  late final ShizuruStartSessionDart startSession;
  late final ShizuruShutdownDart shutdown;
  late final ShizuruHasActiveSessionDart hasActiveSession;
  late final ShizuruSendMessageDart sendMessage;
  late final ShizuruGetStateDart getState;
  late final ShizuruSetOutputCallbackDart setOutputCallback;
  late final ShizuruSetStateCallbackDart setStateCallback;
  late final ShizuruSetToolCallCallbackDart setToolCallCallback;
  late final ShizuruRegisterToolDart registerTool;
  late final ShizuruSetupVoiceDart setupVoice;
  late final ShizuruSpeakDart speak;
  late final ShizuruStopSpeakingDart stopSpeaking;
  late final ShizuruSetTranscriptionCallbackDart setTranscriptionCallback;
  late final ShizuruStartRecordingDart startRecording;
  late final ShizuruStopRecordingDart stopRecording;
  late final ShizuruPeekAudioSizeDart peekAudioSize;
  late final ShizuruTakeAudioIntoDart takeAudioInto;
  late final ShizuruFreeStringDart freeString;

  ShizuruNativeBindings() {
    _lib = _loadLibrary();
    _bindAll();
  }

  ShizuruNativeBindings.fromPath(String path) {
    _lib = DynamicLibrary.open(path);
    _bindAll();
  }

  void _bindAll() {
    create = _lib.lookupFunction<ShizuruCreateNative, ShizuruCreateDart>(
        'shizuru_create');
    destroy = _lib.lookupFunction<ShizuruDestroyNative, ShizuruDestroyDart>(
        'shizuru_destroy');
    startSession = _lib.lookupFunction<ShizuruStartSessionNative, ShizuruStartSessionDart>(
        'shizuru_start_session');
    shutdown = _lib.lookupFunction<ShizuruShutdownNative, ShizuruShutdownDart>(
        'shizuru_shutdown');
    hasActiveSession = _lib.lookupFunction<ShizuruHasActiveSessionNative, ShizuruHasActiveSessionDart>(
        'shizuru_has_active_session');
    sendMessage = _lib.lookupFunction<ShizuruSendMessageNative, ShizuruSendMessageDart>(
        'shizuru_send_message');
    getState = _lib.lookupFunction<ShizuruGetStateNative, ShizuruGetStateDart>(
        'shizuru_get_state');
    setOutputCallback = _lib.lookupFunction<ShizuruSetOutputCallbackNative, ShizuruSetOutputCallbackDart>(
        'shizuru_set_output_callback');
    setStateCallback = _lib.lookupFunction<ShizuruSetStateCallbackNative, ShizuruSetStateCallbackDart>(
        'shizuru_set_state_callback');
    setToolCallCallback = _lib.lookupFunction<ShizuruSetToolCallCallbackNative, ShizuruSetToolCallCallbackDart>(
        'shizuru_set_tool_call_callback');
    registerTool = _lib.lookupFunction<ShizuruRegisterToolNative, ShizuruRegisterToolDart>(
        'shizuru_register_tool');
    setupVoice = _lib.lookupFunction<ShizuruSetupVoiceNative, ShizuruSetupVoiceDart>(
        'shizuru_setup_voice');
    speak = _lib.lookupFunction<ShizuruSpeakNative, ShizuruSpeakDart>(
        'shizuru_speak');
    stopSpeaking = _lib.lookupFunction<ShizuruStopSpeakingNative, ShizuruStopSpeakingDart>(
        'shizuru_stop_speaking');
    setTranscriptionCallback = _lib.lookupFunction<
            ShizuruSetTranscriptionCallbackNative,
            ShizuruSetTranscriptionCallbackDart>(
        'shizuru_set_transcription_callback');
    startRecording = _lib.lookupFunction<ShizuruStartRecordingNative,
            ShizuruStartRecordingDart>(
        'shizuru_start_recording');
    stopRecording = _lib.lookupFunction<ShizuruStopRecordingNative,
            ShizuruStopRecordingDart>(
        'shizuru_stop_recording');
    peekAudioSize = _lib.lookupFunction<ShizuruPeekAudioSizeNative, ShizuruPeekAudioSizeDart>(
        'shizuru_peek_audio_size');
    takeAudioInto = _lib.lookupFunction<ShizuruTakeAudioIntoNative, ShizuruTakeAudioIntoDart>(
        'shizuru_take_audio_into');
    freeString = _lib.lookupFunction<ShizuruFreeStringNative, ShizuruFreeStringDart>(
        'shizuru_free_string');
  }

  static DynamicLibrary _loadLibrary() {
    if (Platform.isWindows) {
      return DynamicLibrary.open('shizuru_api.dll');
    } else if (Platform.isLinux) {
      return DynamicLibrary.open('libshizuru_api.so');
    } else if (Platform.isMacOS) {
      return DynamicLibrary.open('libshizuru_api.dylib');
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }
}
