package com.example.shizuru_ui

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.example.shizuru_ui/audio"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "setSpeakerphoneOn" -> {
                        val on = call.argument<Boolean>("on") ?: true
                        val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
                        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION

                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            // Android 12+: use setCommunicationDevice
                            if (on) {
                                val speaker = audioManager.availableCommunicationDevices
                                    .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
                                if (speaker != null) {
                                    audioManager.setCommunicationDevice(speaker)
                                }
                            } else {
                                audioManager.clearCommunicationDevice()
                            }
                        } else {
                            // Android < 12: use deprecated setSpeakerphoneOn
                            @Suppress("DEPRECATION")
                            audioManager.isSpeakerphoneOn = on
                        }
                        result.success(null)
                    }
                    else -> result.notImplemented()
                }
            }
    }
}
