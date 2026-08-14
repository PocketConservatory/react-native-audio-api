package com.swmansion.audioapi.system

/**
 * Stops and finalizes any active native recordings without an AudioAPIModule instance.
 * The native side is registered from JNI_OnLoad, so it survives React module invalidation.
 */
object RecorderEmergencyStop {
  @JvmStatic
  external fun stopActiveRecordings()
}
