import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'app.dart';
import 'bridge/shizuru_bridge.dart';
import 'providers/app_provider.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();

  // Catch ALL unhandled Dart errors before the VM calls exit(1).
  PlatformDispatcher.instance.onError = (error, stack) {
    stderr.writeln('[DART FATAL] $error');
    stderr.writeln('[DART FATAL STACK] $stack');
    return true; // mark as handled — prevents exit(1)
  };
  FlutterError.onError = (details) {
    stderr.writeln('[FLUTTER ERROR] ${details.exceptionAsString()}');
    stderr.writeln('[FLUTTER ERROR STACK] ${details.stack}');
  };

  // Try to use the C++ FFI bridge; fall back to pure-Dart HTTP bridge.
  _initBridge();

  runApp(
    ChangeNotifierProvider(
      create: (_) => AppProvider(),
      child: const ShizuruApp(),
    ),
  );
}

void _initBridge() {
  // Check if shizuru_api.dll exists next to the executable.
  final exeDir = File(Platform.resolvedExecutable).parent.path;
  final dllPath = '$exeDir/shizuru_api.dll';

  if (File(dllPath).existsSync()) {
    try {
      ShizuruBridge.useInstance(FfiBridge(libraryPath: dllPath));
      stderr.writeln('[main] Using FfiBridge (C++ core)');
      return;
    } catch (e) {
      stderr.writeln('[main] Failed to load FfiBridge: $e');
    }
  }

  // Fall back to pure-Dart HTTP bridge (default singleton).
  stderr.writeln('[main] Using HttpBridge (pure Dart fallback)');
}
