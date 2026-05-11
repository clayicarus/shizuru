#pragma once

// io/onebot/onebot_tags.h — OneBot-specific structured tags for LLM context.
//
// Tags representing QQ/OneBot message segment types.  These are used by
// OneBotDevice when constructing text content for the agent pipeline.

#include <string>

namespace shizuru::io::onebot::tags {

// Simple self-closing XML tag helper: <tag attr="value"/>
inline std::string SelfClosingTag(const char* tag, const char* attr,
                                  const std::string& value) {
  return std::string("<") + tag + " " + attr + "=\"" + value + "\"/>";
}

// ── At mention (inline, self-closing) ───────────────────────────────────────
// <at id="self"/>  or  <at id="12345"/>
inline constexpr char kAt[]      = "at";
inline constexpr char kAttrId[]  = "id";
inline constexpr char kAtSelf[]  = "self";

// ── Face emoji (inline, self-closing) ───────────────────────────────────────
// <face desc="微笑"/>
inline constexpr char kFace[]     = "face";
inline constexpr char kAttrDesc[] = "desc";

// ── Media stub (inline, self-closing) — unsupported media types ─────────────
// <media type="record"/>  <media type="video"/>  <media type="forward"/>
inline constexpr char kMedia[]    = "media";
inline constexpr char kAttrType[] = "type";

}  // namespace shizuru::io::onebot::tags
