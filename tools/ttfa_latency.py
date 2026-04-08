#!/usr/bin/env python3
"""
TTFA (Time To First Audio) latency analyzer.

Parses shizuru log output and computes per-turn latency breakdown:
  VAD speech_end → ASR result → ObsFilter → LLM submit → LLM first token
  → TTS segment ready → TTS first audio chunk → First playout

Usage:
  python tools/ttfa_latency.py shizuru.log
  cat shizuru.log | python tools/ttfa_latency.py
"""

import re
import sys
from datetime import datetime
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# Log timestamp format: [2026-04-09 01:35:19.952]
TS_PATTERN = re.compile(r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]")

# Pipeline event patterns
EVENTS = {
    "speech_end":      re.compile(r"vad_in received event 'speech_end'"),
    "asr_result":      re.compile(r"BaiduAsrDevice: ASR result:"),
    "obs_filter_done": re.compile(r"ObservationFilter took \d+ms"),
    "llm_submit":      re.compile(r"LLM submit started"),
    "llm_first_token": re.compile(r"LLM first token received"),
    "tts_segment":     re.compile(r"TTS segment (?:ready|final flush):"),
    "tts_first_chunk": re.compile(r"TTS first audio chunk:"),
    "first_playout":   re.compile(r"first audio playout, buffer latency="),
}

# Also extract the inline filter duration for display
FILTER_MS_PATTERN = re.compile(r"ObservationFilter took (\d+)ms")

STAGE_LABELS = [
    ("speech_end",      "asr_result",      "VAD → ASR"),
    ("asr_result",      "obs_filter_done", "  ASR → ObsFilter done"),
    ("obs_filter_done", "llm_submit",      "  ObsFilter → LLM submit"),
    ("llm_submit",      "llm_first_token", "  LLM submit → 1st token"),
    ("asr_result",      "llm_first_token", "ASR → LLM 1st token (total)"),
    ("llm_first_token", "tts_segment",     "LLM 1st token → TTS segment"),
    ("tts_segment",     "tts_first_chunk", "TTS segment → TTS 1st chunk"),
    ("tts_first_chunk", "first_playout",   "TTS 1st chunk → Playout"),
]

# Events required for a "complete" turn
COMPLETE_EVENTS = [
    "speech_end", "asr_result", "llm_first_token",
    "tts_segment", "tts_first_chunk", "first_playout",
]


@dataclass
class Turn:
    index: int
    timestamps: dict = field(default_factory=dict)
    filter_ms: Optional[float] = None  # inline ObsFilter duration from log

    def is_complete(self) -> bool:
        return all(k in self.timestamps for k in COMPLETE_EVENTS)

    def ttfa_ms(self) -> Optional[float]:
        if "speech_end" in self.timestamps and "first_playout" in self.timestamps:
            return _ms(self.timestamps["speech_end"], self.timestamps["first_playout"])
        return None


def _parse_ts(line: str) -> Optional[datetime]:
    m = TS_PATTERN.search(line)
    if not m:
        return None
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")


def _ms(t0: datetime, t1: datetime) -> float:
    return (t1 - t0).total_seconds() * 1000


def analyze(lines):
    turns = []  # type: List[Turn]
    current = None  # type: Optional[Turn]
    turn_idx = 0

    for line in lines:
        ts = _parse_ts(line)
        if ts is None:
            continue

        # speech_end starts a new turn
        if EVENTS["speech_end"].search(line):
            turn_idx += 1
            current = Turn(index=turn_idx)
            current.timestamps["speech_end"] = ts
            turns.append(current)
            continue

        if current is None:
            continue

        # Match remaining events (only first occurrence per turn)
        for event_name, pattern in EVENTS.items():
            if event_name == "speech_end":
                continue
            if event_name not in current.timestamps and pattern.search(line):
                current.timestamps[event_name] = ts
                # Extract inline filter duration
                if event_name == "obs_filter_done":
                    m = FILTER_MS_PATTERN.search(line)
                    if m:
                        current.filter_ms = float(m.group(1))
                break

    return turns


def print_report(turns):
    # type: (List[Turn]) -> None
    if not turns:
        print("No turns found in log.")
        return

    all_ttfa = []  # type: List[float]
    all_stages = {}  # type: Dict[str, List[float]]
    for _, _, label in STAGE_LABELS:
        all_stages.setdefault(label, [])

    for turn in turns:
        print(f"\n{'='*64}")
        print(f"Turn {turn.index}")
        print(f"{'='*64}")

        for from_evt, to_evt, label in STAGE_LABELS:
            if from_evt in turn.timestamps and to_evt in turn.timestamps:
                dt = _ms(turn.timestamps[from_evt], turn.timestamps[to_evt])
                extra = ""
                # Show inline filter duration next to the ObsFilter line
                if "ObsFilter done" in label and turn.filter_ms is not None:
                    extra = f"  (filter self-reported: {turn.filter_ms:.0f}ms)"
                print(f"  {label:.<44s} {dt:>7.1f} ms{extra}")
                all_stages[label].append(dt)
            else:
                missing = to_evt if from_evt in turn.timestamps else from_evt
                print(f"  {label:.<44s}     --- (missing {missing})")

        ttfa = turn.ttfa_ms()
        if ttfa is not None:
            print(f"  {'TTFA (total)':.<44s} {ttfa:>7.1f} ms")
            all_ttfa.append(ttfa)
        else:
            print(f"  {'TTFA (total)':.<44s}     --- (incomplete)")

    # Summary
    n = len(turns)
    complete = sum(1 for t in turns if t.is_complete())
    print(f"\n{'='*64}")
    print(f"Summary: {n} turn(s), {complete} complete")
    print(f"{'='*64}")

    if all_ttfa:
        avg = sum(all_ttfa) / len(all_ttfa)
        lo, hi = min(all_ttfa), max(all_ttfa)
        print(f"  {'TTFA avg':.<44s} {avg:>7.1f} ms")
        print(f"  {'TTFA min':.<44s} {lo:>7.1f} ms")
        print(f"  {'TTFA max':.<44s} {hi:>7.1f} ms")

    print()
    for _, _, label in STAGE_LABELS:
        vals = all_stages.get(label, [])
        if vals:
            avg = sum(vals) / len(vals)
            print(f"  {label + ' avg':.<44s} {avg:>7.1f} ms")


def main():
    if len(sys.argv) > 1 and sys.argv[1] != "-":
        with open(sys.argv[1], "r", errors="replace") as f:
            lines = f.readlines()
    else:
        sys.stdin.reconfigure(errors="replace")
        lines = sys.stdin.readlines()

    turns = analyze(lines)
    print_report(turns)


if __name__ == "__main__":
    main()
