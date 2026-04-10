import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../bridge/bridge_config.dart';
import '../providers/agent_provider.dart';
import '../providers/conversation_provider.dart';

class SettingsScreen extends StatefulWidget {
  final bool isInitialSetup;
  const SettingsScreen({super.key, this.isInitialSetup = false});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  final _formKey = GlobalKey<FormState>();

  late final TextEditingController _llmBaseUrl;
  late final TextEditingController _llmApiPath;
  late final TextEditingController _llmApiKey;
  late final TextEditingController _llmModel;
  late final TextEditingController _elevenLabsApiKey;
  late final TextEditingController _elevenLabsVoiceId;
  late final TextEditingController _baiduApiKey;
  late final TextEditingController _baiduSecretKey;
  late final TextEditingController _systemInstruction;
  late final TextEditingController _maxTurns;

  bool _saving = false;

  @override
  void initState() {
    super.initState();
    final d = BridgeConfig.defaults();
    _llmBaseUrl        = TextEditingController(text: d.llmBaseUrl);
    _llmApiPath        = TextEditingController(text: d.llmApiPath);
    _llmApiKey         = TextEditingController();
    _llmModel          = TextEditingController(text: d.llmModel);
    _elevenLabsApiKey  = TextEditingController();
    _elevenLabsVoiceId = TextEditingController();
    _baiduApiKey       = TextEditingController();
    _baiduSecretKey    = TextEditingController();
    _systemInstruction = TextEditingController(text: d.systemInstruction);
    _maxTurns          = TextEditingController(text: d.maxTurns.toString());
    _loadSaved();
  }

  Future<void> _loadSaved() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      _llmBaseUrl.text        = prefs.getString('llm_base_url')        ?? _llmBaseUrl.text;
      _llmApiPath.text        = prefs.getString('llm_api_path')        ?? _llmApiPath.text;
      _llmApiKey.text         = prefs.getString('llm_api_key')         ?? '';
      _llmModel.text          = prefs.getString('llm_model')           ?? _llmModel.text;
      _elevenLabsApiKey.text  = prefs.getString('elevenlabs_api_key')  ?? '';
      _elevenLabsVoiceId.text = prefs.getString('elevenlabs_voice_id') ?? '';
      _baiduApiKey.text       = prefs.getString('baidu_api_key')       ?? '';
      _baiduSecretKey.text    = prefs.getString('baidu_secret_key')    ?? '';
      _systemInstruction.text = prefs.getString('system_instruction')  ?? _systemInstruction.text;
      _maxTurns.text          = prefs.getString('max_turns')           ?? _maxTurns.text;
    });
  }

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;
    setState(() => _saving = true);

    final prefs = await SharedPreferences.getInstance();
    await prefs.setString('llm_base_url',       _llmBaseUrl.text.trim());
    await prefs.setString('llm_api_path',        _llmApiPath.text.trim());
    await prefs.setString('llm_api_key',         _llmApiKey.text.trim());
    await prefs.setString('llm_model',           _llmModel.text.trim());
    await prefs.setString('elevenlabs_api_key',  _elevenLabsApiKey.text.trim());
    await prefs.setString('elevenlabs_voice_id', _elevenLabsVoiceId.text.trim());
    await prefs.setString('baidu_api_key',        _baiduApiKey.text.trim());
    await prefs.setString('baidu_secret_key',     _baiduSecretKey.text.trim());
    await prefs.setString('system_instruction',   _systemInstruction.text.trim());
    await prefs.setString('max_turns',            _maxTurns.text.trim());

    final config = BridgeConfig(
      llmBaseUrl:        _llmBaseUrl.text.trim(),
      llmApiPath:        _llmApiPath.text.trim(),
      llmApiKey:         _llmApiKey.text.trim(),
      llmModel:          _llmModel.text.trim(),
      elevenLabsApiKey:  _elevenLabsApiKey.text.trim(),
      elevenLabsVoiceId: _elevenLabsVoiceId.text.trim(),
      baiduApiKey:       _baiduApiKey.text.trim(),
      baiduSecretKey:    _baiduSecretKey.text.trim(),
      systemInstruction: _systemInstruction.text.trim(),
      maxTurns:          int.tryParse(_maxTurns.text.trim()) ?? 100,
    );

    if (mounted) {
      final agent = context.read<AgentProvider>();
      final conv  = context.read<ConversationProvider>();

      // Register output callback before (re-)initializing the bridge.
      agent.setOutputCallback((text, isPartial) {
        conv.onOutputChunk(text, isPartial);
      });

      // Register transcript callback so voice input appears in chat.
      agent.setTranscriptCallback((text) {
        conv.addUserMessage(text);
      });

      await agent.initialize(config);
      setState(() => _saving = false);
      if (mounted) Navigator.of(context).pop();
    }
  }

  @override
  void dispose() {
    _llmBaseUrl.dispose();
    _llmApiPath.dispose();
    _llmApiKey.dispose();
    _llmModel.dispose();
    _elevenLabsApiKey.dispose();
    _elevenLabsVoiceId.dispose();
    _baiduApiKey.dispose();
    _baiduSecretKey.dispose();
    _systemInstruction.dispose();
    _maxTurns.dispose();
    super.dispose();
  }

  Widget _field(
    String label,
    TextEditingController ctrl, {
    bool obscure = false,
    bool multiline = false,
    bool required = false,
    TextInputType? keyboardType,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8),
      child: TextFormField(
        controller: ctrl,
        obscureText: obscure,
        maxLines: multiline ? 4 : 1,
        keyboardType: keyboardType,
        decoration: InputDecoration(
          labelText: label,
          border: const OutlineInputBorder(),
        ),
        validator: required
            ? (v) => (v == null || v.trim().isEmpty) ? '$label is required' : null
            : null,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.isInitialSetup ? 'Setup' : 'Settings'),
        automaticallyImplyLeading: !widget.isInitialSetup,
      ),
      body: Form(
        key: _formKey,
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _field('LLM Base URL', _llmBaseUrl),
            _field('LLM API Path', _llmApiPath),
            _field('LLM API Key', _llmApiKey, obscure: true, required: true),
            _field('LLM Model', _llmModel),
            _field('ElevenLabs API Key', _elevenLabsApiKey, obscure: true, required: true),
            _field('ElevenLabs Voice ID', _elevenLabsVoiceId),
            _field('Baidu API Key', _baiduApiKey, obscure: true, required: true),
            _field('Baidu Secret Key', _baiduSecretKey, obscure: true, required: true),
            _field('System Instruction', _systemInstruction, multiline: true),
            _field('Max Turns', _maxTurns, keyboardType: TextInputType.number),
            const SizedBox(height: 16),
            ElevatedButton(
              onPressed: _saving ? null : _save,
              child: _saving
                  ? const SizedBox(
                      height: 20,
                      width: 20,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Text('Save & Connect'),
            ),
          ],
        ),
      ),
    );
  }
}
