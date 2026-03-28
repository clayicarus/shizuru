#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------

typedef void* ShizuruHandle;

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------

// Called for every LLM output token/chunk.
// is_partial=1 for streaming token chunks, is_partial=0 for the final response.
typedef void (*ShizuruOutputCallback)(const char* text, int32_t is_partial,
                                      void* user_data);

// Called whenever core::State changes.
// state is the int32_t ordinal of core::State:
//   0=Idle, 1=Listening, 2=Thinking, 3=Routing, 4=Acting, 5=Responding,
//   6=Error, 7=Terminated
typedef void (*ShizuruStateCallback)(int32_t state, void* user_data);

// Called at ~20ms intervals with the current microphone RMS level.
typedef void (*ShizuruAudioLevelCallback)(float rms, void* user_data);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Parse config_json, construct AgentRuntime with full voice device topology.
// Returns a non-null handle on success.
// On failure, returns NULL and writes a human-readable message to error_buf
// (up to error_buf_len-1 bytes, always NUL-terminated if error_buf != NULL).
ShizuruHandle shizuru_create(const char* config_json, char* error_buf,
                             int error_buf_len);

// Call AgentRuntime::StartSession(). Returns 0 on success, negative on error.
int32_t shizuru_start(ShizuruHandle handle);

// Call AgentRuntime::Shutdown() and free all resources.
void shizuru_destroy(ShizuruHandle handle);

// ---------------------------------------------------------------------------
// Messaging and state
// ---------------------------------------------------------------------------

// Call AgentRuntime::SendMessage(text). Returns 0 on success.
int32_t shizuru_send_message(ShizuruHandle handle, const char* text);

// Return current core::State as int32_t. Safe to call from any thread.
int32_t shizuru_get_state(ShizuruHandle handle);

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

// Register output callback. Fired for every RuntimeOutput (partial + final).
void shizuru_set_output_callback(ShizuruHandle handle, ShizuruOutputCallback cb,
                                 void* user_data);

// Register state-change callback. Fired when core::State changes.
void shizuru_set_state_callback(ShizuruHandle handle, ShizuruStateCallback cb,
                                void* user_data);

// ---------------------------------------------------------------------------
// Voice control
// ---------------------------------------------------------------------------

// Start microphone capture. No-op if already running. Returns 0 on success.
int32_t shizuru_start_capture(ShizuruHandle handle);

// Stop microphone capture. No-op if already stopped. Returns 0 on success.
int32_t shizuru_stop_capture(ShizuruHandle handle);

// Register audio level callback. Fired at ~20ms intervals during capture.
void shizuru_set_audio_level_callback(ShizuruHandle handle,
                                      ShizuruAudioLevelCallback cb,
                                      void* user_data);

// Free a string allocated by the bridge (e.g. from output callbacks).
// Must be called by Dart after copying the string content.
void shizuru_free_string(char* str);

#ifdef __cplusplus
}  // extern "C"
#endif
