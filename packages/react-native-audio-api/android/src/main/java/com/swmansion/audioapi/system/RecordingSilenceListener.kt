package com.swmansion.audioapi.system

import android.media.AudioManager
import android.media.AudioRecordingConfiguration
import android.os.Build
import android.os.Handler
import androidx.annotation.RequiresApi
import com.swmansion.audioapi.AudioAPIModule
import java.lang.ref.WeakReference
import java.util.concurrent.Executor

/**
 * Reports the system silencing the active recorder's capture (a call, the assistant, another
 * recorder, the microphone privacy toggle) as the `interruption` event, without requesting audio
 * focus, so other apps' playback is left alone.
 *
 * The recorder's configuration is the active recording configuration whose client audio session
 * id matches [NativeRecorderControl.audioSessionId], read on every callback because a restart
 * reopens the stream with a new id. A callback without that configuration leaves the state
 * unchanged: a missing configuration is unknown, not unsilenced.
 *
 * - `began` (`shouldResume: false`) once per interruption, when the configuration becomes
 *   silenced, including one that first appears already silenced.
 * - `ended` (`shouldResume: true`) once per `began`, when the configuration is no longer silenced
 *   and no call started after `began`: the audio mode at `began` is recorded, and `ended` is held
 *   while the current mode is a call mode other than that one. A call mode already set at
 *   `began`, such as the app's own communication session, never holds `ended`; a system call
 *   silences capture while ringing, so its call mode starts after `began`. The mode listener
 *   (API 31+), the poll (API 29–30), the configuration callback and [isRecordingSilenced] all
 *   apply this one rule.
 * - With no active recorder session the state resets without emitting.
 *
 * Requires API 29+; a no-op below. [setEnabled] and [release] run on the main thread, where all
 * state is written.
 */
class RecordingSilenceListener(
  private val audioManager: WeakReference<AudioManager>,
  private val audioAPIModule: WeakReference<AudioAPIModule>,
  private val mainHandler: Handler,
) {
  private val mainExecutor = Executor { mainHandler.post(it) }

  private var enabledCount = 0
  private var released = false
  private var recordingCallback: AudioManager.AudioRecordingCallback? = null
  private var awaitingCallEnd = false

  // AudioManager.OnModeChangedListener (API 31+), held untyped so older runtimes never resolve it.
  private var modeChangedListener: Any? = null
  private val modePoll =
    object : Runnable {
      override fun run() {
        tryEmitEnded()
        if (awaitingCallEnd) {
          mainHandler.postDelayed(this, MODE_POLL_INTERVAL_MS)
        }
      }
    }

  @Volatile
  private var captureSilenced = false

  /** The audio mode when `began` was emitted; null while no interruption is in progress. */
  @Volatile
  private var modeAtBegan: Int? = null

  /**
   * Enables or disables observation. Calls are counted, so a late `false` from an earlier owner
   * does not stop observation a newer owner enabled.
   */
  fun setEnabled(enabled: Boolean) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || released) {
      return
    }
    if (enabled) {
      enabledCount += 1
      if (enabledCount == 1) register()
    } else if (enabledCount > 0) {
      enabledCount -= 1
      if (enabledCount == 0) unregister()
    }
  }

  /** Stops observation for good, whatever the enable count, without emitting. */
  fun release() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || released) {
      return
    }
    released = true
    enabledCount = 0
    unregister()
  }

  /**
   * True while the active recorder's capture is silenced, or its `ended` is still held by a call
   * that started after `began`. Tracked only while observation is enabled; always false below
   * API 29. Callable from any thread.
   */
  fun isRecordingSilenced(): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || NativeRecorderControl.audioSessionId() == 0) {
      return false
    }
    return captureSilenced || isEndedHeld()
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
    manager.registerAudioRecordingCallback(callback, mainHandler)
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
      if (modeAtBegan == null) {
        modeAtBegan = currentMode()
        emit(began = true)
      }
    } else {
      tryEmitEnded()
    }
  }

  private fun tryEmitEnded() {
    if (modeAtBegan == null || captureSilenced) {
      stopAwaitingCallEnd()
      return
    }
    if (isEndedHeld()) {
      startAwaitingCallEnd()
      return
    }
    stopAwaitingCallEnd()
    modeAtBegan = null
    emit(began = false)
  }

  private fun reset() {
    stopAwaitingCallEnd()
    modeAtBegan = null
    captureSilenced = false
  }

  private fun startAwaitingCallEnd() {
    if (awaitingCallEnd) {
      return
    }
    awaitingCallEnd = true
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      addModeChangedListener()
      // A mode change between the last read and the registration would otherwise be lost.
      mainHandler.post { tryEmitEnded() }
    } else {
      mainHandler.postDelayed(modePoll, MODE_POLL_INTERVAL_MS)
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
      mainHandler.removeCallbacks(modePoll)
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

  private fun currentMode(): Int = audioManager.get()?.mode ?: AudioManager.MODE_NORMAL

  private fun isEndedHeld(): Boolean {
    val began = modeAtBegan ?: return false
    val mode = currentMode()
    return isCallMode(mode) && mode != began
  }

  private fun isCallMode(mode: Int): Boolean =
    mode == AudioManager.MODE_IN_CALL ||
      mode == AudioManager.MODE_IN_COMMUNICATION ||
      (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && mode == AudioManager.MODE_CALL_SCREENING)

  private fun emit(began: Boolean) {
    audioAPIModule.get()?.invokeHandlerWithEventNameAndEventBody(
      AudioEvent.INTERRUPTION.ordinal,
      mapOf("type" to if (began) "began" else "ended", "shouldResume" to !began),
    )
  }

  companion object {
    private const val MODE_POLL_INTERVAL_MS = 1_000L
  }
}
