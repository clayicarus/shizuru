import 'dart:async';
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'bridge/bridge_config.dart';
import 'providers/agent_provider.dart';
import 'providers/conversation_provider.dart';
import 'screens/conversation_screen.dart';
import 'screens/settings_screen.dart';

void main() {
  runZonedGuarded(
    () {
      WidgetsFlutterBinding.ensureInitialized();

      FlutterError.onError = (details) {
        FlutterError.presentError(details);
        Zone.current.handleUncaughtError(
          details.exception,
          details.stack ?? StackTrace.empty,
        );
      };

      PlatformDispatcher.instance.onError = (error, stack) {
        debugPrint('PlatformDispatcher error: $error');
        debugPrint('$stack');
        return true;
      };

      runApp(
        MultiProvider(
          providers: [
            ChangeNotifierProvider(create: (_) => AgentProvider()),
            ChangeNotifierProvider(create: (_) => ConversationProvider()),
          ],
          child: const ShizuruApp(),
        ),
      );
    },
    (error, stack) {
      debugPrint('Uncaught zone error: $error');
      debugPrint('$stack');
    },
  );
}

class ShizuruApp extends StatelessWidget {
  const ShizuruApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Shizuru',
      theme: ThemeData(colorSchemeSeed: Colors.teal, useMaterial3: true),
      home: const _AppStartup(),
    );
  }
}

class _AppStartup extends StatefulWidget {
  const _AppStartup();

  @override
  State<_AppStartup> createState() => _AppStartupState();
}

class _AppStartupState extends State<_AppStartup> {
  static const bool _skipAutoInit = bool.fromEnvironment(
    'SHIZURU_SKIP_AUTO_INIT',
    defaultValue: false,
  );
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _startup();
  }

  Future<void> _startup() async {
    final prefs = await SharedPreferences.getInstance();
    BridgeConfig? savedConfig;

    final llmApiKey = prefs.getString('llm_api_key') ?? '';
    final elevenLabsKey = prefs.getString('elevenlabs_api_key') ?? '';
    final baiduApiKey = prefs.getString('baidu_api_key') ?? '';
    final baiduSecretKey = prefs.getString('baidu_secret_key') ?? '';

    final configComplete =
        llmApiKey.isNotEmpty &&
        elevenLabsKey.isNotEmpty &&
        baiduApiKey.isNotEmpty &&
        baiduSecretKey.isNotEmpty;

    if (configComplete && !_skipAutoInit) {
      savedConfig = BridgeConfig(
        llmBaseUrl:
            prefs.getString('llm_base_url') ??
            BridgeConfig.defaults().llmBaseUrl,
        llmApiPath:
            prefs.getString('llm_api_path') ??
            BridgeConfig.defaults().llmApiPath,
        llmApiKey: llmApiKey,
        llmModel:
            prefs.getString('llm_model') ?? BridgeConfig.defaults().llmModel,
        elevenLabsApiKey: elevenLabsKey,
        elevenLabsVoiceId: prefs.getString('elevenlabs_voice_id') ?? '',
        baiduApiKey: baiduApiKey,
        baiduSecretKey: baiduSecretKey,
        systemInstruction:
            prefs.getString('system_instruction') ??
            BridgeConfig.defaults().systemInstruction,
        maxTurns:
            int.tryParse(prefs.getString('max_turns') ?? '') ??
            BridgeConfig.defaults().maxTurns,
      );

      if (mounted) {
        final agent = context.read<AgentProvider>();
        final conv = context.read<ConversationProvider>();

        // Register output callback BEFORE initialize so the bridge picks it up.
        agent.setOutputCallback((text, isPartial) {
          conv.onOutputChunk(text, isPartial);
        });

        // Register transcript callback so voice input appears in chat.
        agent.setTranscriptCallback((text) {
          conv.addUserMessage(text);
        });
      }
    }

    if (mounted) {
      setState(() => _loading = false);
      if (!configComplete) {
        Navigator.of(context).pushReplacement(
          MaterialPageRoute(
            builder: (_) => const SettingsScreen(isInitialSetup: true),
          ),
        );
      } else if (!_skipAutoInit && savedConfig != null) {
        final config = savedConfig;
        WidgetsBinding.instance.addPostFrameCallback((_) {
          unawaited(_initializeSavedConfig(config));
        });
      }
    }
  }

  Future<void> _initializeSavedConfig(BridgeConfig config) async {
    if (!mounted) return;
    try {
      await context.read<AgentProvider>().initialize(config);
    } catch (e, st) {
      debugPrint('Shizuru startup failed: $e');
      debugPrint('$st');
      if (!mounted) return;
      Navigator.of(context).pushReplacement(
        MaterialPageRoute(
          builder: (_) => const SettingsScreen(isInitialSetup: true),
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Scaffold(body: Center(child: CircularProgressIndicator()));
    }
    return const ConversationScreen();
  }
}
