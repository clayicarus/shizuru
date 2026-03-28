import 'dart:convert';

class BridgeConfig {
  final String llmBaseUrl;
  final String llmApiPath;
  final String llmApiKey;
  final String llmModel;
  final String elevenLabsApiKey;
  final String elevenLabsVoiceId;
  final String baiduApiKey;
  final String baiduSecretKey;
  final String systemInstruction;
  final int maxTurns;

  const BridgeConfig({
    required this.llmBaseUrl,
    required this.llmApiPath,
    required this.llmApiKey,
    required this.llmModel,
    required this.elevenLabsApiKey,
    required this.elevenLabsVoiceId,
    required this.baiduApiKey,
    required this.baiduSecretKey,
    required this.systemInstruction,
    required this.maxTurns,
  });

  factory BridgeConfig.defaults() => const BridgeConfig(
    llmBaseUrl: 'https://dashscope.aliyuncs.com',
    llmApiPath: '/compatible-mode/v1/chat/completions',
    llmApiKey: '',
    llmModel: 'qwen3-coder-next',
    elevenLabsApiKey: '',
    elevenLabsVoiceId: '',
    baiduApiKey: '',
    baiduSecretKey: '',
    systemInstruction:
        'You are a helpful voice assistant. Keep responses concise and natural '
        'for speech. Avoid markdown formatting.',
    maxTurns: 100,
  );

  BridgeConfig copyWith({
    String? llmBaseUrl,
    String? llmApiPath,
    String? llmApiKey,
    String? llmModel,
    String? elevenLabsApiKey,
    String? elevenLabsVoiceId,
    String? baiduApiKey,
    String? baiduSecretKey,
    String? systemInstruction,
    int? maxTurns,
  }) {
    return BridgeConfig(
      llmBaseUrl: llmBaseUrl ?? this.llmBaseUrl,
      llmApiPath: llmApiPath ?? this.llmApiPath,
      llmApiKey: llmApiKey ?? this.llmApiKey,
      llmModel: llmModel ?? this.llmModel,
      elevenLabsApiKey: elevenLabsApiKey ?? this.elevenLabsApiKey,
      elevenLabsVoiceId: elevenLabsVoiceId ?? this.elevenLabsVoiceId,
      baiduApiKey: baiduApiKey ?? this.baiduApiKey,
      baiduSecretKey: baiduSecretKey ?? this.baiduSecretKey,
      systemInstruction: systemInstruction ?? this.systemInstruction,
      maxTurns: maxTurns ?? this.maxTurns,
    );
  }

  String toJson() => jsonEncode({
    'llm_base_url':       llmBaseUrl,
    'llm_api_path':       llmApiPath,
    'llm_api_key':        llmApiKey,
    'llm_model':          llmModel,
    'elevenlabs_api_key':  elevenLabsApiKey,
    'elevenlabs_voice_id': elevenLabsVoiceId,
    'baidu_api_key':       baiduApiKey,
    'baidu_secret_key':   baiduSecretKey,
    'system_instruction': systemInstruction,
    'max_turns':          maxTurns,
  });

  factory BridgeConfig.fromJson(Map<String, dynamic> json) => BridgeConfig(
    llmBaseUrl:       json['llm_base_url'] as String? ?? 'https://dashscope.aliyuncs.com',
    llmApiPath:       json['llm_api_path'] as String? ?? '/compatible-mode/v1/chat/completions',
    llmApiKey:        json['llm_api_key'] as String? ?? '',
    llmModel:         json['llm_model'] as String? ?? 'qwen3-coder-next',
    elevenLabsApiKey: json['elevenlabs_api_key'] as String? ?? '',
    elevenLabsVoiceId: json['elevenlabs_voice_id'] as String? ?? '',
    baiduApiKey:      json['baidu_api_key'] as String? ?? '',
    baiduSecretKey:   json['baidu_secret_key'] as String? ?? '',
    systemInstruction: json['system_instruction'] as String? ??
        'You are a helpful voice assistant. Keep responses concise and natural '
        'for speech. Avoid markdown formatting.',
    maxTurns: json['max_turns'] as int? ?? 100,
  );
}
