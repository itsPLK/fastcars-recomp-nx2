// fastcars - unhandled host exception reporter (Windows); debugger attach opt-in (Linux).
//
// Writes the exception, registers and a symbolized stack of the faulting thread to
// crash.txt next to the log. Recompiled functions are named sub_XXXXXXXX, so frames map
// straight back to guest addresses.


// Android: debuggerd already writes a tombstone with the native stack (adb logcat / bugreport).
