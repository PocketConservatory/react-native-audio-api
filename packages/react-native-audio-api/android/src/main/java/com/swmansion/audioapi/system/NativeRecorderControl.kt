package com.swmansion.audioapi.system

/**
 * Direct access to the active C++ recorder, independent of the JS runtime.
 * This is what allows the recording-notification stop action to end a recording
 * after the app task has been removed.
 */
object NativeRecorderControl {
  init {
    System.loadLibrary("react-native-audio-api")
  }

  /**
   * The file info is stashed natively for
   * `AudioRecorder.consumeLastRecordingResult()` on the JS side.
   */
  fun stop(): RecorderState = RecorderState.fromOrdinal(stopActiveRecording())

  /** Pauses an actively recording session; a no-op in any other state. */
  fun pause(): RecorderState = RecorderState.fromOrdinal(pauseActiveRecording())

  /** Resumes a paused session; a no-op in any other state. */
  fun resume(): RecorderState = RecorderState.fromOrdinal(resumeActiveRecording())

  /** Non-blocking read of the recorder state; [RecorderState.IDLE] when there is none. */
  fun currentState(): RecorderState = RecorderState.fromOrdinal(currentRecorderState())

  /**
   * Audio session id of the active recorder's input stream, or 0 when no recording session
   * holds a stream. Matches `AudioRecordingConfiguration.getClientAudioSessionId()`.
   */
  fun audioSessionId(): Int = activeAudioSessionId()

  /**
   * Replaces the active recorder's input stream with a freshly opened one, keeping the session
   * state. A no-op unless the active session's stream still has [expectedSessionId] (see
   * [audioSessionId]); when the new stream can't be opened, the current one keeps running and the
   * recorder's error event is dispatched. Blocks on the recorder, so call it off the main thread.
   */
  fun reopenInputStream(expectedSessionId: Int) = reopenActiveInputStream(expectedSessionId)

  /**
   * Reports whether the system silences the active session's capture. The recorder emits the
   * `interruption` event when this begins or ends its interruption, which also has causes of its
   * own (see [isReclaimBackingOff]). A no-op without a session. Never blocks on the recorder.
   */
  fun setCaptureSilenced(silenced: Boolean) = setActiveCaptureSilenced(silenced)

  /** Clears the silenced report without emitting, for an observer that stops tracking it. */
  fun dropCaptureSilenced() = dropActiveCaptureSilenced()

  /**
   * True while the active session waits to reopen an input stream that keeps disconnecting right
   * after it opens; the recorder reports this as an interruption. Never blocks on the recorder.
   */
  fun isReclaimBackingOff(): Boolean = isActiveReclaimBackingOff()

  @JvmStatic
  private external fun activeAudioSessionId(): Int

  @JvmStatic
  private external fun reopenActiveInputStream(expectedSessionId: Int)

  @JvmStatic
  private external fun setActiveCaptureSilenced(silenced: Boolean)

  @JvmStatic
  private external fun dropActiveCaptureSilenced()

  @JvmStatic
  private external fun isActiveReclaimBackingOff(): Boolean

  @JvmStatic
  private external fun stopActiveRecording(): Int

  @JvmStatic
  private external fun pauseActiveRecording(): Int

  @JvmStatic
  private external fun resumeActiveRecording(): Int

  @JvmStatic
  private external fun currentRecorderState(): Int
}
