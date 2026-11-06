package com.swmansion.audioapi.system

import android.media.AudioManager
import android.media.AudioRecordingConfiguration
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.annotation.RequiresApi
import java.lang.ref.WeakReference
import java.util.concurrent.Executor
import java.util.concurrent.Executors

/**
 * Reports when the system silences the active recorder's capture (a call, the assistant, another
 * recorder, the mic privacy toggle) to the recorder as its capture-silenced interruption cause
 * ([NativeRecorderControl.setCaptureSilenced]), without requesting audio focus, so other apps'
 * playback is left alone. The recorder emits the `interruption` event: `began` when its first
 * cause appears and `ended` when the last one clears, so a silenced capture and a stream that
 * keeps disconnecting (its other cause) share one `began`/`ended` pair.
 *
 * The recorder's configuration is the active recording configuration whose client audio session
 * id matches the recorder's current input stream ([NativeRecorderControl.audioSessionId]), read
 * on every callback because a disconnect reopens the stream with a new id. A callback without
 * that configuration leaves the state unchanged: a missing configuration is unknown, not
 * unsilenced.
 *
 * - The cause is set once per interruption, when the configuration becomes silenced, including
 *   one that first appears already silenced.
 * - It is cleared once the configuration is no longer silenced and no call started after it was
 *   set: the audio mode at that point (`began` below) is recorded, and the clear is held while
 *   the current mode is a call mode (in call, in communication, call screening) other than that
 *   one. The check is stateless, so the mode listener (API 31+), the poll (API 29–30), the
 *   recording-configuration callback and [isRecordingSilenced] all apply the same rule and differ
 *   only in when they run. A call mode already set at `began`, such as the host app's own VoIP,
 *   never holds `ended`; a system call silences capture through the ringtone mode first, so its
 *   call mode starts after `began`. Trade-off: a configuration first seen already silenced while
 *   the mode is already a call (a recording started mid-call, or a reclaimed stream) doesn't hold
 *   `ended`, so an unsilence mid-call ends that interruption early.
 * - Each time the app comes to the foreground ([onAppForegrounded]) while a call seen during the
 *   interruption is over and the configuration is still silenced, the recorder's input stream is
 *   reopened once ([NativeRecorderControl.reopenInputStream]). On Android 14 and earlier, a
 *   stream opened while the app had no microphone access (not on top, no microphone-type
 *   foreground service), as the disconnect reclaim can do mid-call, stays silenced for its whole
 *   life: the system checks the RECORD_AUDIO app op when the capture starts and, for an app
 *   targeting API 34+, re-checks it only when the op's setting changes. A stream opened once the
 *   app is in the foreground again is unsilenced, and its configuration then clears the cause
 *   through the rule above. The reopen is a no-op for a capture that isn't stuck, so it also
 *   covers other causes and runs on every API level.
 * - With no active recorder session, or when observation stops, the state resets and the cause is
 *   dropped without emitting.
 *
 * The audio mode is observed from `began` until the interruption ends: through the mode listener
 * on API 31+, and a poll on API 29–30.
 *
 * Requires API 29+; a no-op below. State is written on the main looper only.
 */
class RecordingSilenceListener(
  private val audioManager: WeakReference<AudioManager>,
) {
  private val handler = Handler(Looper.getMainLooper())
  private val mainExecutor = Executor { handler.post(it) }

  private var enabledCount = 0
  private var released = false
  private var recordingCallback: AudioManager.AudioRecordingCallback? = null
  private var observingMode = false

  /** Whether a call mode was current at some point during this interruption. */
  private var callModeSeen = false

  // The reopen waits on the recorder locks and the old stream's stop, so it runs off the main
  // looper, on a thread created only when a reopen is first needed.
  private val reopenExecutorDelegate = lazy { Executors.newSingleThreadExecutor() }
  private val reopenExecutor by reopenExecutorDelegate

  // AudioManager.OnModeChangedListener (API 31+), held untyped so older runtimes never resolve it.
  private var modeChangedListener: Any? = null
  private val modePoll =
    object : Runnable {
      override fun run() {
        evaluate()
        if (observingMode) {
          handler.postDelayed(this, MODE_POLL_INTERVAL_MS)
        }
      }
    }

  @Volatile
  private var captureSilenced = false

  /** The audio mode when the cause was set; null while no interruption is in progress. */
  @Volatile
  private var modeAtBegan: Int? = null

  /**
   * Enables or disables observation. Calls are counted, so a late `false` from an earlier owner
   * does not stop observation a newer owner enabled. Callable from any thread.
   */
  fun setEnabled(enabled: Boolean) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      return
    }
    handler.post {
      if (released) {
        return@post
      }
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
   * Stops observation for good, whatever the enable count, without emitting. Called when the
   * owning module is invalidated. Callable from any thread.
   */
  fun release() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      return
    }
    handler.post {
      released = true
      enabledCount = 0
      unregister()
      if (reopenExecutorDelegate.isInitialized()) {
        reopenExecutor.shutdown()
      }
    }
  }

  /**
   * Reopens the recorder's input stream when a call seen during the interruption is over and the
   * capture is still silenced (see the class rules). Called each time the app's activity comes to
   * the foreground, so a reopen that still lands silenced is retried on the next one. Callable
   * from any thread.
   */
  fun onAppForegrounded() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      return
    }
    handler.post {
      val sessionId = NativeRecorderControl.audioSessionId()
      if (released || sessionId == 0 || !needsReopen(currentMode())) {
        return@post
      }
      // The id ties the reopen to this session: a stop or new start in between makes it a no-op.
      reopenExecutor.execute { NativeRecorderControl.reopenInputStream(sessionId) }
    }
  }

  /**
   * True while the active recorder's capture is silenced, its interruption's `ended` is still
   * held by a call that started after `began`, or its input stream waits to reopen after
   * disconnecting repeatedly ([NativeRecorderControl.isReclaimBackingOff]). The silenced state is
   * tracked only while observation is enabled. Below API 29 only the reopen wait counts. Callable
   * from any thread.
   */
  fun isRecordingSilenced(): Boolean {
    if (NativeRecorderControl.audioSessionId() == 0) return false
    if (NativeRecorderControl.isReclaimBackingOff()) return true
    return Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && (captureSilenced || isEndedHeld())
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
    if (captureSilenced && modeAtBegan == null) {
      modeAtBegan = currentMode()
      NativeRecorderControl.setCaptureSilenced(true)
      startObservingMode()
    }
    evaluate()
  }

  /** Applies the class rules to the current configuration state and audio mode: clears the cause. */
  private fun evaluate() {
    if (modeAtBegan == null) {
      stopObservingMode()
      return
    }
    if (isCallMode(currentMode())) {
      callModeSeen = true
    }
    if (captureSilenced || isEndedHeld()) {
      return
    }
    modeAtBegan = null
    callModeSeen = false
    stopObservingMode()
    NativeRecorderControl.setCaptureSilenced(false)
  }

  /** Whether a call seen during this interruption is over and the capture is still silenced. */
  private fun needsReopen(mode: Int): Boolean = modeAtBegan != null && captureSilenced && callModeSeen && !isCallMode(mode)

  private fun reset() {
    stopObservingMode()
    if (modeAtBegan != null) {
      NativeRecorderControl.dropCaptureSilenced()
    }
    modeAtBegan = null
    callModeSeen = false
    captureSilenced = false
  }

  private fun startObservingMode() {
    if (observingMode) {
      return
    }
    observingMode = true
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      addModeChangedListener()
      // A mode change between the last read and the registration would otherwise be lost.
      handler.post { evaluate() }
    } else {
      handler.postDelayed(modePoll, MODE_POLL_INTERVAL_MS)
    }
  }

  private fun stopObservingMode() {
    if (!observingMode) {
      return
    }
    observingMode = false
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      removeModeChangedListener()
    } else {
      handler.removeCallbacks(modePoll)
    }
  }

  @RequiresApi(Build.VERSION_CODES.S)
  private fun addModeChangedListener() {
    val listener = AudioManager.OnModeChangedListener { evaluate() }
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

  /** Whether a call mode that started after `began` holds the interruption's `ended`. */
  private fun isEndedHeld(): Boolean {
    val began = modeAtBegan ?: return false
    val mode = currentMode()
    return isCallMode(mode) && mode != began
  }

  private fun isCallMode(mode: Int): Boolean =
    mode == AudioManager.MODE_IN_CALL ||
      mode == AudioManager.MODE_IN_COMMUNICATION ||
      (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && mode == AudioManager.MODE_CALL_SCREENING)

  companion object {
    private const val MODE_POLL_INTERVAL_MS = 1_000L
  }
}
