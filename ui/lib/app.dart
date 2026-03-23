import 'package:flutter/material.dart';

import 'pages/main_layout.dart';
import 'pages/settings_page.dart';

class ShizuruApp extends StatelessWidget {
  const ShizuruApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Shizuru',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF6B4EFF),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
        inputDecorationTheme: const InputDecorationTheme(
          border: OutlineInputBorder(),
        ),
      ),
      initialRoute: '/',
      routes: {
        '/': (_) => const MainLayout(),
        '/settings': (_) => const SettingsPage(),
      },
    );
  }
}
