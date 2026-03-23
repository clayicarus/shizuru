import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../models/agent_state.dart';
import '../providers/app_provider.dart';

class InputBar extends StatefulWidget {
  const InputBar({super.key});

  @override
  State<InputBar> createState() => _InputBarState();
}

class _InputBarState extends State<InputBar> {
  final _textCtrl = TextEditingController();
  final _focusNode = FocusNode();

  @override
  void dispose() {
    _textCtrl.dispose();
    _focusNode.dispose();
    super.dispose();
  }

  void _send() {
    final prov = context.read<AppProvider>();
    final text = _textCtrl.text.trim();
    if (text.isEmpty) return;
    prov.sendTextMessage(text);
    _textCtrl.clear();
    _focusNode.requestFocus();
  }

  bool get _isBusy {
    final s = context.read<AppProvider>().agentState;
    return s == AgentState.thinking ||
        s == AgentState.routing ||
        s == AgentState.acting ||
        s == AgentState.responding;
  }

  @override
  Widget build(BuildContext context) {
    final prov = context.watch<AppProvider>();
    final theme = Theme.of(context);
    final isVoice = prov.inputMode == InputMode.voice;
    final busy = prov.agentState != AgentState.idle &&
        prov.agentState != AgentState.error;

    return Container(
      padding: const EdgeInsets.fromLTRB(20, 10, 20, 16),
      decoration: BoxDecoration(
        border: Border(top: BorderSide(color: theme.dividerColor)),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          // Mode toggle
          Tooltip(
            message: isVoice ? '切换到文字输入' : '切换到语音输入',
            child: IconButton.outlined(
              icon: Icon(
                isVoice ? Icons.keyboard_rounded : Icons.mic_rounded,
                size: 19,
              ),
              onPressed: () => prov.setInputMode(
                isVoice ? InputMode.text : InputMode.voice,
              ),
            ),
          ),
          const SizedBox(width: 8),
          // Input area
          Expanded(
            child: isVoice ? const _VoiceInputArea() : _buildTextField(busy),
          ),
          const SizedBox(width: 8),
          // Send / stop button
          if (!isVoice)
            busy
                ? _StopButton(onTap: () {})
                : _SendButton(onTap: _send),
        ],
      ),
    );
  }

  Widget _buildTextField(bool busy) {
    return CallbackShortcuts(
      bindings: {
        const SingleActivator(LogicalKeyboardKey.enter): () {
          if (!HardwareKeyboard.instance.isShiftPressed) _send();
        },
      },
      child: Focus(
        child: TextField(
          controller: _textCtrl,
          focusNode: _focusNode,
          enabled: !busy,
          maxLines: 5,
          minLines: 1,
          autofocus: true,
          style: const TextStyle(fontSize: 14),
          decoration: InputDecoration(
            hintText: busy
                ? 'Agent 正在处理中…'
                : '输入消息… (Enter 发送，Shift+Enter 换行)',
            hintStyle: TextStyle(
              color: Theme.of(context).colorScheme.onSurface.withOpacity(0.35),
              fontSize: 13,
            ),
            contentPadding:
                const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          ),
        ),
      ),
    );
  }
}

// ─── Voice input area ─────────────────────────────────────────────────────────

class _VoiceInputArea extends StatelessWidget {
  const _VoiceInputArea();

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final prov = context.watch<AppProvider>();
    final recording = prov.isRecording;
    final busy = prov.agentState == AgentState.thinking ||
        prov.agentState == AgentState.responding;

    return Row(
      children: [
        // Record button
        Expanded(
          child: GestureDetector(
            onTap: busy ? null : () => prov.toggleRecording(),
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 250),
              height: 52,
              decoration: BoxDecoration(
                color: recording
                    ? cs.error.withOpacity(0.15)
                    : cs.surfaceContainerHigh,
                borderRadius: BorderRadius.circular(8),
                border: Border.all(
                  color: recording
                      ? cs.error.withOpacity(0.6)
                      : cs.outline.withOpacity(0.3),
                ),
              ),
              alignment: Alignment.center,
              child: Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(
                    recording
                        ? Icons.stop_circle_rounded
                        : Icons.mic_rounded,
                    size: 22,
                    color: recording
                        ? cs.error
                        : busy
                            ? cs.onSurface.withOpacity(0.25)
                            : cs.primary,
                  ),
                  const SizedBox(width: 10),
                  Text(
                    recording
                        ? '录音中… 点击停止并发送'
                        : busy
                            ? '处理中…'
                            : '点击开始录音',
                    style: TextStyle(
                      fontSize: 13,
                      color: recording
                          ? cs.error
                          : busy
                              ? cs.onSurface.withOpacity(0.3)
                              : cs.onSurface.withOpacity(0.5),
                      fontWeight:
                          recording ? FontWeight.w600 : FontWeight.normal,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
        const SizedBox(width: 8),
        // TTS toggle
        Tooltip(
          message: prov.enableTts ? '关闭语音回复' : '开启语音回复',
          child: IconButton.outlined(
            icon: Icon(
              prov.enableTts
                  ? Icons.volume_up_rounded
                  : Icons.volume_off_rounded,
              size: 19,
              color: prov.enableTts ? cs.primary : cs.onSurface.withOpacity(0.4),
            ),
            onPressed: prov.toggleTts,
          ),
        ),
      ],
    );
  }
}

// ─── Send button ──────────────────────────────────────────────────────────────

class _SendButton extends StatelessWidget {
  final VoidCallback onTap;
  const _SendButton({required this.onTap});

  @override
  Widget build(BuildContext context) {
    return FilledButton(
      onPressed: onTap,
      style: FilledButton.styleFrom(
        minimumSize: const Size(46, 46),
        padding: EdgeInsets.zero,
        shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.all(Radius.circular(10)),
        ),
      ),
      child: const Icon(Icons.send_rounded, size: 19),
    );
  }
}

// ─── Stop button (while agent is busy) ───────────────────────────────────────

class _StopButton extends StatelessWidget {
  final VoidCallback onTap;
  const _StopButton({required this.onTap});

  @override
  Widget build(BuildContext context) {
    return OutlinedButton(
      onPressed: onTap,
      style: OutlinedButton.styleFrom(
        minimumSize: const Size(46, 46),
        padding: EdgeInsets.zero,
        shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.all(Radius.circular(10)),
        ),
      ),
      child: const Icon(Icons.stop_rounded, size: 19),
    );
  }
}
