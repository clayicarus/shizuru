// Audio recording and playback service for Windows desktop.
// Uses Windows native SoundPlayer for playback (no plugin dependency).
// Uses `record` package for microphone recording.

import 'dart:io';
import 'dart:typed_data';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:record/record.dart';

class AudioService {
  AudioRecorder? _recorder;
  String? _tempDir;
  bool _isRecording = false;
  bool _isPlaying = false;
  Process? _playbackProcess;
  bool _available = false;

  bool get isRecording => _isRecording;
  bool get isPlaying => _isPlaying;
  bool get available => _available;

  Future<void> init() async {
    try {
      final dir = await getTemporaryDirectory();
      _tempDir = dir.path;
      _recorder = AudioRecorder();
      _available = true;
    } catch (e) {
      _available = false;
    }
  }

  /// Start recording audio from microphone.
  Future<bool> startRecording() async {
    if (!_available || _recorder == null) return false;
    if (_isRecording) return false;
    if (!await _recorder!.hasPermission()) return false;

    _tempDir ??= (await getTemporaryDirectory()).path;
    final recPath =
        p.join(_tempDir!, 'shizuru_rec_${DateTime.now().millisecondsSinceEpoch}.wav');

    try {
      await _recorder!.start(
        const RecordConfig(
          encoder: AudioEncoder.wav,
          numChannels: 1,
          sampleRate: 16000,
          bitRate: 256000,
        ),
        path: recPath,
      );
      _isRecording = true;
      return true;
    } catch (e) {
      _isRecording = false;
      return false;
    }
  }

  /// Stop recording and return the WAV audio bytes.
  Future<Uint8List?> stopRecording() async {
    if (!_isRecording) return null;
    _isRecording = false;

    try {
      final path = await _recorder!.stop();
      if (path == null || path.isEmpty) return null;

      final file = File(path);
      if (!await file.exists()) return null;

      final bytes = await file.readAsBytes();
      await file.delete().catchError((_) {});
      return bytes;
    } catch (e) {
      return null;
    }
  }

  /// Play audio bytes using Windows native SoundPlayer (powershell).
  /// Supports WAV format. Saves to temp file, plays synchronously, cleans up.
  Future<void> playAudioBytes(Uint8List audioBytes, String mimeType) async {
    _tempDir ??= (await getTemporaryDirectory()).path;

    final ext = _extFromMime(mimeType);
    final filePath =
        p.join(_tempDir!, 'shizuru_tts_${DateTime.now().millisecondsSinceEpoch}.$ext');
    final file = File(filePath);
    await file.writeAsBytes(audioBytes);

    _isPlaying = true;
    try {
      // Use PowerShell's SoundPlayer for WAV playback (guaranteed on Windows).
      final escaped = filePath.replaceAll("'", "''");
      _playbackProcess = await Process.start(
        'powershell',
        [
          '-NoProfile',
          '-Command',
          '\$p = New-Object System.Media.SoundPlayer \'$escaped\'; \$p.PlaySync(); \$p.Dispose()',
        ],
        mode: ProcessStartMode.normal,
      );
      await _playbackProcess!.exitCode;
    } catch (_) {
      // Playback failure is non-fatal.
    } finally {
      _isPlaying = false;
      _playbackProcess = null;
      await file.delete().catchError((_) {});
    }
  }

  /// Stop any playing audio.
  Future<void> stopPlayback() async {
    _playbackProcess?.kill();
    _playbackProcess = null;
    _isPlaying = false;
  }

  String _extFromMime(String mime) {
    if (mime.contains('wav')) return 'wav';
    if (mime.contains('mp3')) return 'mp3';
    if (mime.contains('pcm')) return 'pcm';
    return 'wav';
  }

  Future<void> dispose() async {
    if (_isRecording) await _recorder?.stop();
    await _recorder?.dispose();
    await stopPlayback();
  }
}
