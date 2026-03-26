// Public C API for the Shizuru agent runtime.
//
// Provides a stable ABI for embedding the C++ agent runtime in host
// applications (Flutter via dart:ffi, Python via ctypes, etc.).
//
// Thread-safety:
//   - All functions are safe to call from any thread.
//   - Callbacks (output, state) are invoked on the Controller's worker thread.
//     Host code must handle cross-thread dispatch (e.g. Dart NativeCallable.listener).
//
// String ownership:
//   - Strings returned by the API are allocated with malloc().
//     The caller MUST free them with shizuru_free_string().
//   - Strings passed as callback arguments are allocated with strdup().
//     The callback recipient MUST free them with shizuru_free_string().

#ifndef SHIZURU_API_H
#define SHIZURU_API_H

#ifdef _WIN32
  #ifdef SHIZURU_API_EXPORTS
    #define SHIZURU_API __declspec(dllexport)
  #else
    #define SHIZURU_API __declspec(dllimport)
  #endif
#else
  #define SHIZURU_API __attribute__((visibility("default")))
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------

typedef struct ShizuruRuntime ShizuruRuntime;

// ---------------------------------------------------------------------------
// Agent state (mirrors core::State)
// ---------------------------------------------------------------------------

typedef enum {
  SHIZURU_STATE_IDLE        = 0,
  SHIZURU_STATE_LISTENING   = 1,
  SHIZURU_STATE_THINKING    = 2,
  SHIZURU_STATE_ROUTING     = 3,
  SHIZURU_STATE_ACTING      = 4,
  SHIZURU_STATE_RESPONDING  = 5,
  SHIZURU_STATE_ERROR       = 6,
  SHIZURU_STATE_TERMINATED  = 7,
} ShizuruState;

// ---------------------------------------------------------------------------
// Callback types
//
// text / message pointers are allocated with strdup(). The callback recipient
// MUST call shizuru_free_string() on them after use.
// ---------------------------------------------------------------------------

typedef void (*ShizuruOutputCallback)(const char* text, void* user_data);
typedef void (*ShizuruStateCallback)(int32_t state, void* user_data);

// Tool call notification: name and arguments are strdup-allocated.
// The callback MUST call shizuru_free_string() on both.
typedef void (*ShizuruToolCallCallback)(const char* tool_name,
                                         const char* arguments,
                                         void* user_data);

// Tool handler: synchronous C function that executes a tool.
// Must return a malloc-allocated JSON string: {"success":true,"output":"..."}
// or {"success":false,"error":"..."}. Caller frees the returned string.
typedef const char* (*ShizuruToolHandler)(const char* arguments,
                                           void* user_data);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create a runtime instance.  config_json is a UTF-8 JSON string:
//
//   {
//     "llm_base_url":    "https://dashscope.aliyuncs.com",
//     "llm_api_path":    "/compatible-mode/v1/chat/completions",
//     "llm_api_key":     "sk-...",
//     "llm_model":       "qwen3-coder-next",
//     "system_prompt":   "You are Shizuru, a helpful AI assistant."
//   }
//
// Returns NULL on failure.
SHIZURU_API ShizuruRuntime* shizuru_create(const char* config_json);

// Destroy the runtime and free all resources.  Safe to call with NULL.
SHIZURU_API void shizuru_destroy(ShizuruRuntime* rt);

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// Start a new session.  Returns a session-id string (caller must free with
// shizuru_free_string).  Returns NULL on failure.
SHIZURU_API const char* shizuru_start_session(ShizuruRuntime* rt);

// Shut down the active session.
SHIZURU_API void shizuru_shutdown(ShizuruRuntime* rt);

// Check if a session is active.  Returns 1 (true) or 0 (false).
SHIZURU_API int32_t shizuru_has_active_session(ShizuruRuntime* rt);

// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

// Send a user text message.  Non-blocking: enqueues the message to the
// Controller's observation queue and returns immediately.
SHIZURU_API void shizuru_send_message(ShizuruRuntime* rt, const char* content);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Get the current agent state (ShizuruState value).
SHIZURU_API int32_t shizuru_get_state(ShizuruRuntime* rt);

// ---------------------------------------------------------------------------
// Callbacks  (call BEFORE shizuru_start_session)
// ---------------------------------------------------------------------------

// Register a callback invoked when the agent produces a final text response.
// text is strdup-allocated; the callback MUST call shizuru_free_string(text).
SHIZURU_API void shizuru_set_output_callback(ShizuruRuntime* rt,
                                              ShizuruOutputCallback cb,
                                              void* user_data);

// Register a callback invoked on every agent state transition.
SHIZURU_API void shizuru_set_state_callback(ShizuruRuntime* rt,
                                             ShizuruStateCallback cb,
                                             void* user_data);

// Register a callback invoked when the agent dispatches a tool call.
// tool_name and arguments are strdup-allocated; callback MUST free both.
SHIZURU_API void shizuru_set_tool_call_callback(ShizuruRuntime* rt,
                                                 ShizuruToolCallCallback cb,
                                                 void* user_data);

// ---------------------------------------------------------------------------
// Tool registration  (call BEFORE shizuru_start_session)
// ---------------------------------------------------------------------------

// Register a tool that the LLM can invoke.
//   name:        tool name (e.g. "get_weather")
//   description: human-readable description for the LLM
//   params_json: JSON array of parameter objects:
//                [{"name":"city","type":"string","description":"...","required":true}]
//   handler:     C function executed when the tool is called (may be NULL for
//                notification-only tools)
//   user_data:   passed to handler
//
// Automatically adds a PolicyRule allowing this tool and grants the capability.
SHIZURU_API void shizuru_register_tool(ShizuruRuntime* rt,
                                        const char* name,
                                        const char* description,
                                        const char* params_json,
                                        ShizuruToolHandler handler,
                                        void* user_data);

// ---------------------------------------------------------------------------
// Voice pipeline  (call setup BEFORE shizuru_start_session)
// ---------------------------------------------------------------------------

// Configure and create the voice pipeline (capture → VAD → ASR, TTS → playout).
// voice_config_json:
//   {
//     "asr_api_key":     "...",
//     "asr_secret_key":  "...",
//     "tts_provider":    "baidu" | "elevenlabs",
//     "tts_api_key":     "...",
//     "tts_secret_key":  "...",   // baidu only
//     "tts_voice_id":    "...",   // elevenlabs only
//     "sample_rate":     16000     // optional, default 16000
//   }
// Returns 1 on success, 0 on failure.
SHIZURU_API int32_t shizuru_setup_voice(ShizuruRuntime* rt,
                                         const char* voice_config_json);

// Feed text into the TTS device for synthesis and playout.
SHIZURU_API void shizuru_speak(ShizuruRuntime* rt, const char* text);

// Cancel in-progress TTS and audio playout.
SHIZURU_API void shizuru_stop_speaking(ShizuruRuntime* rt);

// ---------------------------------------------------------------------------
// ASR recording (click-to-talk; call setup_voice with asr_api_key first)
// ---------------------------------------------------------------------------

// Callback invoked when transcription completes (on a C++ worker thread).
// transcript is a malloc-allocated UTF-8 string; caller MUST free it with
// shizuru_free_string().  May be NULL if transcription failed or was empty.
typedef void (*ShizuruTranscriptionCallback)(const char* transcript,
                                              void* user_data);

// Register transcription callback (call before shizuru_start_session).
SHIZURU_API void shizuru_set_transcription_callback(
    ShizuruRuntime* rt, ShizuruTranscriptionCallback cb, void* user_data);

// Start microphone recording.
// Returns 1 on success, 0 on failure (ASR not configured, device error).
SHIZURU_API int32_t shizuru_start_recording(ShizuruRuntime* rt);

// Stop recording and asynchronously transcribe the captured audio.
// The transcript is delivered via ShizuruTranscriptionCallback (may be NULL).
SHIZURU_API void shizuru_stop_recording(ShizuruRuntime* rt);

// ---------------------------------------------------------------------------
// TTS audio output (Dart-side polling bridge)
// ---------------------------------------------------------------------------

// Returns the pending audio byte count (> 0 if ready), or 0 if none.
SHIZURU_API int64_t shizuru_peek_audio_size(ShizuruRuntime* rt);

// Copies audio into caller-allocated buffer (size >= shizuru_peek_audio_size).
// Returns bytes copied, or -1 on error. Clears the pending audio.
SHIZURU_API int64_t shizuru_take_audio_into(ShizuruRuntime* rt,
                                              uint8_t* buf,
                                              int64_t  buf_size);

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------

// Free a string returned by or passed through the API.
SHIZURU_API void shizuru_free_string(const char* str);

#ifdef __cplusplus
}
#endif

#endif  // SHIZURU_API_H
