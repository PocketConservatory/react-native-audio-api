package com.swmansion.audioapi.system

import android.media.AudioManager
import android.media.AudioRecordingConfiguration
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.annotation.RequiresApi
import com.swmansion.audioapi.AudioAPIModule
import java.lang.ref.WeakReference
import java.util.HashMap
import java.util.concurrent.Executor

/**
 * Reports when the system silences the active recorder's capture (a call, the assistant, another
 * recorder, the mic privacy toggle) as the `interruption` event, without requesting audio focus,
 * so other apps' playback is left alone.
 *
 * The recorder's configuration is the active recording configuration whose client audio session
 * id matches the recorder's current input stream ([NativeRecorderControl.audioSessionId]), read
 * on every callback because a disconnect reopens the stream with a new id. A callback without
 * that configuration leaves the state unchanged: a missing configuration is unknown, not
 * unsilenced.
 *
 * - `began` (`shouldResume: false`) once per interruption, when the configuration becomes
 *   silenced, including one that first appears already silenced.
 * - `ended` (`shouldResume: true`) once per `began`, when the configuration is no longer silenced
 *   and the audio mode is not a call. While the mode is still a call, `ended` is held until the
 *   mode returns to normal (a mode listener on API 31+, polling on API 29–30).
 * - With no active recorder session the state resets without emitting.
 *
 * Requires API 29+; a no-op below. All state lives on the main looper.
 */
class RecordingSilenceListener(
  private val audioManager: WeakReference<AudioManager>,
  private val audioAPIModule: WeakReference<AudioAPIModule>,
) {
  private val handler = Handler(Looper.getMainLooper())
  private val mainExecutor = Executor { handler.post(it) }

  private var enabledCount = 0
  private var recordingCallback: AudioManager.AudioRecordingCallback? = null
  private var interrupted = false
  private var awaitingCallEnd = false

  // AudioManager.OnModeChangedListener (API 31+), held untyped so older runtimes never resolve it.
  private var modeChangedListener: Any? = null
  private val modePoll =
    object : Runnable {
      override fun run() {
        tryEmitEnded()
        if (awaitingCallEnd) {
          handler.postDelayed(this, MODE_POLL_INTERVAL_MS)
        }
      }
    }

  @Volatile
  private var captureSilenced = false

  /**
   * Enables or disables observation. Calls are counted, so a late `false` from an earlier owner
   * does not stop observation a newer owner enabled. Callable from any thread.
   */
  fun setEnabled(enabled: Boolean) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      return
    }
    handler.post {
      if (enabled) {
        enabledCount += 1
        if (enabledCount == 1) register()
      } else if (enabledCount > 0) {
        enabledCount -= 1
        if (enabledCount == 0) unregister()
      }
    }
  }

  /**
   * True while the active recorder's capture is silenced (tracked while observation is enabled)
   * or the device is in a call. Always false below API 29. Callable from any thread.
   */
  fun isRecordingSilenced(): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      return false
    }
    val trackedSilenced = captureSilenced && NativeRecorderControl.audioSessionId() != 0
    return trackedSilenced || isInCall()
  }

  @RequiresApi(Build.VERSION_CODES.Q)
  private fun register() {
    val manager = audioManager.get() ?: return
    val callback =
      object : AudioManager.AudioRecordingCallback() {
        override fun onRecordingConfigChanged(configs: MutableList<AudioRecordingConfiguration>) {
          onConfigs(configs)
        }
      }
    recordingCallback = callback
    manager.registerAudioRecordingCallback(callback, handler)
    onConfigs(manager.activeRecordingConfigurations)
  }

  @RequiresApi(Build.VERSION_CODES.Q)
  private fun unregister() {
    recordingCallback?.let { audioManager.get()?.unregisterAudioRecordingCallback(it) }
    recordingCallback = null
    reset()
  }

  @RequiresApi(Build.VERSION_CODES.Q)
  private fun onConfigs(configs: List<AudioRecordingConfiguration>) {
    val sessionId = NativeRecorderControl.audioSessionId()
    if (sessionId == 0) {
      reset()
      return
    }
    val config = configs.firstOrNull { it.clientAudioSessionId == sessionId } ?: return

    captureSilenced = config.isClientSilenced
    if (captureSilenced) {
      stopAwaitingCallEnd()
      if (!interrupted) {
        interrupted = true
        emit(began = true)
      }
    } else {
      tryEmitEnded()
    }
  }

  private fun tryEmitEnded() {
    if (!interrupted || captureSilenced) {
      stopAwaitingCallEnd()
      return
    }
    if (isInCall()) {
      startAwaitingCallEnd()
      return
    }
    stopAwaitingCallEnd()
    interrupted = false
    emit(began = false)
  }

  private fun reset() {
    stopAwaitingCallEnd()
    interrupted = false
    captureSilenced = false
  }

  private fun startAwaitingCallEnd() {
    if (awaitingCallEnd) {
      return
    }
    awaitingCallEnd = true
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      addModeChangedListener()
    } else {
      handler.postDelayed(modePoll, MODE_POLL_INTERVAL_MS)
    }
  }

  private fun stopAwaitingCallEnd() {
    if (!awaitingCallEnd) {
      return
    }
    awaitingCallEnd = false
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      removeModeChangedListener()
    } else {
      handler.removeCallbacks(modePoll)
    }
  }

  @RequiresApi(Build.VERSION_CODES.S)
  private fun addModeChangedListener() {
    val listener = AudioManager.OnModeChangedListener { tryEmitEnded() }
    modeChangedListener = listener
    audioManager.get()?.addOnModeChangedListener(mainExecutor, listener)
  }

  @RequiresApi(Build.VERSION_CODES.S)
  private fun removeModeChangedListener() {
    (modeChangedListener as? AudioManager.OnModeChangedListener)?.let {
      audioManager.get()?.removeOnModeChangedListener(it)
    }
    modeChangedListener = null
  }

  private fun isInCall(): Boolean {
    val mode = audioManager.get()?.mode ?: return false
    return mode == AudioManager.MODE_IN_CALL ||
      mode == AudioManager.MODE_IN_COMMUNICATION ||
      (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && mode == AudioManager.MODE_CALL_SCREENING)
  }

  private fun emit(began: Boolean) {
    val body =
      HashMap<String, Any>().apply {
        put("type", if (began) "began" else "ended")
        put("shouldResume", !began)
      }
    audioAPIModule.get()?.invokeHandlerWithEventNameAndEventBody(AudioEvent.INTERRUPTION.ordinal, body)
  }

  companion object {
    private const val MODE_POLL_INTERVAL_MS = 1_000L
  }
}
