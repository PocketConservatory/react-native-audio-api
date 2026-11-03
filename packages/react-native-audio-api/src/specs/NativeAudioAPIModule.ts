'use strict';
import type { CodegenTypes, TurboModule } from 'react-native';
import { TurboModuleRegistry } from 'react-native';
import {
  AudioDeviceInfo,
  AudioDevicesInfo,
  PermissionStatus,
} from '../system/types';

type OptionsMap = { [key: string]: string | boolean | number | undefined };
type NotificationOpResponse = { success: boolean; error?: string };
type NotificationType = 'playback' | 'recording' | 'simple';
type AudioFocusType =
  | 'gain'
  | 'gainTransient'
  | 'gainTransientExclusive'
  | 'gainTransientMayDuck';

interface Spec extends TurboModule {
  install(): boolean;
  getDevicePreferredSampleRate(): number;
  isFfmpegEnabled(): boolean;

  // AVAudioSession management
  setAudioSessionActivity(enabled: boolean): Promise<void>;
  setSystemOptions(
    ios: CodegenTypes.UnsafeObject,
    android: CodegenTypes.UnsafeObject
  ): void;
  disableSessionManagement(): void;

  // system events and interruptions
  observeAudioInterruptions(focusType: AudioFocusType, enabled: boolean): void;
  activelyReclaimSession(enabled: boolean): void;
  // Android-only: interruptions of the active recording, without audio focus.
  observeRecordingInterruptions(enabled: boolean): void;
  isRecordingSilenced(): boolean;
  observeVolumeChanges(enabled: boolean): void;
  getSystemVolume(): number;

  // Permissions
  requestRecordingPermissions(): Promise<PermissionStatus>;
  checkRecordingPermissions(): Promise<PermissionStatus>;
  requestNotificationPermissions(): Promise<PermissionStatus>;
  checkNotificationPermissions(): Promise<PermissionStatus>;

  // Audio devices
  getDevicesInfo(): Promise<AudioDevicesInfo>;
  setInputDevice(deviceId: string | null): Promise<void>;
  setCommunicationDevice(device: string): Promise<void>;
  getCommunicationDevice(): Promise<AudioDeviceInfo | null>;

  // Notification system
  showNotification(
    type: NotificationType,
    key: string,
    options: OptionsMap
  ): Promise<NotificationOpResponse>;
  hideNotification(key: string): Promise<NotificationOpResponse>;
  isNotificationActive(key: string): Promise<boolean>;
  // Android-only: whether the foreground service is in the foreground for the notification.
  isNotificationServiceRunning(key: string): Promise<boolean>;
  // Android-only: reads bundled asset bytes and returns a Base64 string.
  readAndroidReleaseAssetBytesAsBase64(assetPath: string): Promise<string>;
}

const NativeAudioAPIModule = TurboModuleRegistry.get<Spec>('AudioAPIModule')!;

export { NativeAudioAPIModule };
