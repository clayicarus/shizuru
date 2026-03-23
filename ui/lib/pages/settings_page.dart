import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/app_provider.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final TextEditingController _llmUrlCtrl;
  late final TextEditingController _llmKeyCtrl;
  late final TextEditingController _llmModelCtrl;
  late final TextEditingController _llmPathCtrl;
  late final TextEditingController _asrKeyCtrl;
  late final TextEditingController _asrSecretCtrl;
  late final TextEditingController _ttsKeyCtrl;
  late final TextEditingController _ttsVoiceCtrl;
  late final TextEditingController _promptCtrl;

  @override
  void initState() {
    super.initState();
    final s = context.read<AppProvider>().settings;
    _llmUrlCtrl    = TextEditingController(text: s.llmBaseUrl);
    _llmKeyCtrl    = TextEditingController(text: s.llmApiKey);
    _llmModelCtrl  = TextEditingController(text: s.llmModel);
    _llmPathCtrl   = TextEditingController(text: s.llmApiPath);
    _asrKeyCtrl    = TextEditingController(text: s.asrApiKey);
    _asrSecretCtrl = TextEditingController(text: s.asrSecretKey);
    _ttsKeyCtrl    = TextEditingController(text: s.ttsApiKey);
    _ttsVoiceCtrl  = TextEditingController(text: s.ttsVoiceId);
    _promptCtrl    = TextEditingController(text: s.systemPrompt);
  }

  @override
  void dispose() {
    for (final c in [
      _llmUrlCtrl, _llmKeyCtrl, _llmModelCtrl, _llmPathCtrl,
      _asrKeyCtrl, _asrSecretCtrl, _ttsKeyCtrl, _ttsVoiceCtrl, _promptCtrl,
    ]) {
      c.dispose();
    }
    super.dispose();
  }

  void _save() {
    final s = context.read<AppProvider>().settings;
    s
      ..llmBaseUrl   = _llmUrlCtrl.text.trim()
      ..llmApiKey    = _llmKeyCtrl.text.trim()
      ..llmModel     = _llmModelCtrl.text.trim()
      ..llmApiPath   = _llmPathCtrl.text.trim()
      ..asrApiKey    = _asrKeyCtrl.text.trim()
      ..asrSecretKey = _asrSecretCtrl.text.trim()
      ..ttsApiKey    = _ttsKeyCtrl.text.trim()
      ..ttsVoiceId   = _ttsVoiceCtrl.text.trim()
      ..systemPrompt = _promptCtrl.text;

    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(
        content: Text('设置已保存。重启会话后生效。'),
        duration: Duration(seconds: 2),
      ),
    );
    Navigator.pop(context);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('设置'),
        actions: [
          FilledButton(onPressed: _save, child: const Text('保存')),
          const SizedBox(width: 12),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.symmetric(horizontal: 56, vertical: 28),
        children: [
          // ── LLM ────────────────────────────────────────────────────
          _Section(
            title: 'LLM',
            icon: Icons.psychology_rounded,
            children: [
              _Field('Base URL', _llmUrlCtrl,
                  hint: 'https://dashscope.aliyuncs.com'),
              _Field('API Key', _llmKeyCtrl,
                  hint: 'sk-…', obscure: true),
              _Field('模型', _llmModelCtrl,
                  hint: 'qwen3-coder-next'),
              _Field('API Path', _llmPathCtrl,
                  hint: '/compatible-mode/v1/chat/completions'),
            ],
          ),
          const SizedBox(height: 28),

          // ── ASR ─────────────────────────────────────────────────────
          _Section(
            title: 'ASR 语音识别',
            icon: Icons.mic_rounded,
            subtitle: '使用 Baidu ASR，留空则禁用语音输入',
            children: [
              _Field('API Key', _asrKeyCtrl, hint: 'Baidu API Key', obscure: true),
              _Field('Secret Key', _asrSecretCtrl,
                  hint: 'Baidu Secret Key', obscure: true),
            ],
          ),
          const SizedBox(height: 28),

          // ── TTS ─────────────────────────────────────────────────────
          _Section(
            title: 'TTS 语音合成',
            icon: Icons.volume_up_rounded,
            subtitle: '使用 ElevenLabs TTS，留空则禁用语音输出',
            children: [
              _Field('API Key', _ttsKeyCtrl,
                  hint: 'ElevenLabs API Key', obscure: true),
              _Field('Voice ID', _ttsVoiceCtrl,
                  hint: '留空使用默认 Voice (Rachel)'),
            ],
          ),
          const SizedBox(height: 28),

          // ── System Prompt ───────────────────────────────────────────
          _Section(
            title: 'System Prompt',
            icon: Icons.edit_note_rounded,
            children: [
              TextField(
                controller: _promptCtrl,
                maxLines: 6,
                minLines: 3,
                style: const TextStyle(fontSize: 13, height: 1.6),
                decoration: const InputDecoration(
                  hintText:
                      'You are a helpful voice assistant. Keep responses concise and natural.',
                  contentPadding:
                      EdgeInsets.symmetric(horizontal: 14, vertical: 12),
                ),
              ),
            ],
          ),
          const SizedBox(height: 40),

          // Save button
          Center(
            child: FilledButton.icon(
              icon: const Icon(Icons.save_rounded),
              label: const Text('保存设置'),
              onPressed: _save,
              style: FilledButton.styleFrom(
                minimumSize: const Size(200, 48),
              ),
            ),
          ),
          const SizedBox(height: 20),

          // Version / info
          Center(
            child: Text(
              'Shizuru v0.1.0 — Flutter UI Prototype',
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: Theme.of(context)
                        .colorScheme
                        .onSurface
                        .withOpacity(0.25),
                  ),
            ),
          ),
        ],
      ),
    );
  }
}

// ─── Section container ────────────────────────────────────────────────────────

class _Section extends StatelessWidget {
  final String title;
  final IconData icon;
  final String? subtitle;
  final List<Widget> children;

  const _Section({
    required this.title,
    required this.icon,
    this.subtitle,
    required this.children,
  });

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Icon(icon, size: 18, color: cs.primary),
            const SizedBox(width: 8),
            Text(
              title,
              style: tt.titleSmall?.copyWith(
                color: cs.primary,
                fontWeight: FontWeight.w700,
                letterSpacing: 0.3,
              ),
            ),
          ],
        ),
        if (subtitle != null) ...[
          const SizedBox(height: 4),
          Text(
            subtitle!,
            style: tt.bodySmall
                ?.copyWith(color: cs.onSurface.withOpacity(0.45)),
          ),
        ],
        const SizedBox(height: 14),
        ...children,
      ],
    );
  }
}

// ─── Single form field with optional password toggle ─────────────────────────

class _Field extends StatefulWidget {
  final String label;
  final TextEditingController ctrl;
  final String? hint;
  final bool obscure;

  const _Field(
    this.label,
    this.ctrl, {
    this.hint,
    this.obscure = false,
  });

  @override
  State<_Field> createState() => _FieldState();
}

class _FieldState extends State<_Field> {
  bool _visible = false;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 14),
      child: TextField(
        controller: widget.ctrl,
        obscureText: widget.obscure && !_visible,
        style: const TextStyle(fontSize: 13),
        decoration: InputDecoration(
          labelText: widget.label,
          hintText: widget.hint,
          hintStyle: TextStyle(
            color: Theme.of(context)
                .colorScheme
                .onSurface
                .withOpacity(0.3),
            fontSize: 12,
          ),
          contentPadding:
              const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
          suffixIcon: widget.obscure
              ? IconButton(
                  icon: Icon(
                    _visible
                        ? Icons.visibility_off_outlined
                        : Icons.visibility_outlined,
                    size: 18,
                  ),
                  onPressed: () => setState(() => _visible = !_visible),
                )
              : null,
        ),
      ),
    );
  }
}
