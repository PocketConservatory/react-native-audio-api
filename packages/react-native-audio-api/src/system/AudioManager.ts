import { AudioEventEmitter, AudioEventSubscription } from '../events';
import { SystemEventCallback, SystemEventName } from '../events/types';
import { NativeAudioAPIModule } from '../specs';
import { parseNativeError } from './errors';
import {
  AudioDevicesInfo,
  AudioFocusType,
  IAudioManager,
  PermissionStatus,
  SessionOptions,
} from './types';

class AudioManager implements IAudioManager {
  private readonly audioEventEmitter: AudioEventEmitter;
  constructor() {
    this.audioEventEmitter = new AudioEventEmitter(
      globalThis.AudioEventEmitter
    );
  }

  getDevicePreferredSampleRate(): number {
    return NativeAudioAPIModule.getDevicePreferredSampleRate();
  }

  /**
   * Activates or deactivates the audio session.
   *
   * Resolves when the session activity was set successfully. On failure it
   * rejects with a {@link SessionActivationError} carrying the native error
   * details (`nativeErrorInfo`) when available.
   */
  async setAudioSessionActivity(enabled: boolean): Promise<void> {
    try {
      await NativeAudioAPIModule.setAudioSessionActivity(enabled);
    } catch (error) {
      throw parseNativeError(error);
    }
  }

  setAudioSessionOptions(options: SessionOptions) {
    NativeAudioAPIModule.setAudioSessionOptions(
      options.iosCategory ?? '',
      options.iosMode ?? '',
      options.iosOptions ?? [],
      options.iosAllowHaptics ?? false,
      options.iosNotifyOthersOnDeactivation ?? true
    );
  }

  disableSessionManagement() {
    NativeAudioAPIModule.disableSessionManagement();
  }

  observeAudioInterruptions(param: AudioFocusType | boolean | null) {
    if (typeof param === 'string') {
      NativeAudioAPIModule.observeAudioInterruptions(param, true);
    } else {
      // audiofocusgain as default value if not provided
      NativeAudioAPIModule.observeAudioInterruptions('gain', param === true);
    }
  }

  /**
   * @param enabled - Whether to actively reclaim the session or not
   * @experimental more aggressively try to reactivate the audio session during interruptions.
   * It is subject to change in the future and might be removed.
   *
   * In some cases (depends on app session settings and other apps using audio) system may never
   * send the `interruption ended` event. This method will check if any other audio is playing
   * and try to reactivate the audio session, as soon as there is "silence".
   * Although this might change the expected behavior.
   *
   * Internally method uses `AVAudioSessionSilenceSecondaryAudioHintNotification` as well as
   * interval polling to check if other audio is playing.
   */
  activelyReclaimSession(enabled: boolean) {
    NativeAudioAPIModule.activelyReclaimSession(enabled);
  }

  /**
   * Observes interruptions of an active recording without requesting audio
   * focus, so other apps can keep playing audio while the app records. Use it
   * instead of {@link observeAudioInterruptions} for a recording that should
   * keep going alongside other apps' playback.
   *
   * Android 10+ (API 29+) only; a no-op on iOS, web, and older Android
   * versions. Events arrive through the existing `interruption` system event:
   *
   * - `began` (`shouldResume: false`) when the system silences the recorder's
   *   capture (a call, the assistant, another recorder, the mic privacy
   *   toggle), including a recording that starts already silenced.
   * - `ended` (`shouldResume: true`) once the capture is no longer silenced and
   *   the device is no longer in a call.
   *
   * Calls are counted: each `true` needs a matching `false` before observation
   * stops.
   *
   * @param enabled - Whether to observe recording interruptions.
   */
  observeRecordingInterruptions(enabled: boolean) {
    NativeAudioAPIModule.observeRecordingInterruptions(enabled);
  }

  /**
   * Whether the active recording is currently silenced by the system, the
   * device is in a call, or its input stream is waiting to reopen after
   * disconnecting repeatedly (Android 10+). The recording's own silenced state
   * is tracked only while {@link observeRecordingInterruptions} is enabled.
   * Always `false` on iOS, web, and older Android versions.
   *
   * @returns `true` while recording now would capture silence.
   */
  isRecordingSilenced(): boolean {
    return NativeAudioAPIModule.isRecordingSilenced();
  }

  observeVolumeChanges(enabled: boolean) {
    NativeAudioAPIModule.observeVolumeChanges(enabled);
  }

  addSystemEventListener<Name extends SystemEventName>(
    name: Name,
    callback: SystemEventCallback<Name>
  ): AudioEventSubscription {
    return this.audioEventEmitter.addAudioEventListener(name, callback);
  }

  async requestRecordingPermissions(): Promise<PermissionStatus> {
    return NativeAudioAPIModule.requestRecordingPermissions();
  }

  async checkRecordingPermissions(): Promise<PermissionStatus> {
    return NativeAudioAPIModule.checkRecordingPermissions();
  }

  async requestNotificationPermissions(): Promise<PermissionStatus> {
    return NativeAudioAPIModule.requestNotificationPermissions();
  }

  async checkNotificationPermissions(): Promise<PermissionStatus> {
    return NativeAudioAPIModule.checkNotificationPermissions();
  }

  async getDevicesInfo(): Promise<AudioDevicesInfo> {
    return NativeAudioAPIModule.getDevicesInfo();
  }

  /**
   * Selects the given device as the current audio input.
   *
   * Resolves when the input device was set successfully and rejects when the
   * device cannot be found or the system fails to switch to it.
   */
  async setInputDevice(deviceId: string): Promise<void> {
    await NativeAudioAPIModule.setInputDevice(deviceId);
  }
}

export default new AudioManager();
