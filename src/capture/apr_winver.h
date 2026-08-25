/*
 * apr_winver.h -- Windows API version floor for the capture layer.
 *
 * MUST be the FIRST include in every src/capture/*.c translation unit, before
 * anything that can pull in <windows.h> (err.h does, and capture.h includes
 * err.h).
 *
 * Why: audioclientactivationparams.h is wrapped in
 *   #if (NTDDI_VERSION >= NTDDI_WIN10_FE)
 * and compiles to *nothing at all* below that -- no AUDIOCLIENT_ACTIVATION_PARAMS,
 * no VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, and a pile of confusing "undeclared
 * identifier" errors that look nothing like a version problem. The spike hit
 * this; this header exists so nobody hits it twice.
 *
 * The #error below turns "included too late" into an immediate, honest message
 * instead of the same confusing pile.
 */
#ifndef APPRECORDER_CAPTURE_WINVER_H
#define APPRECORDER_CAPTURE_WINVER_H

#ifdef _WINDOWS_
#  error "apr_winver.h must be included before <windows.h> (and before err.h / capture.h)"
#endif

#undef  _WIN32_WINNT
#define _WIN32_WINNT   0x0A00
#undef  WINVER
#define WINVER         0x0A00
#undef  NTDDI_VERSION
#define NTDDI_VERSION  0x0A00000A   /* NTDDI_WIN10_FE -- process loopback */

#endif /* APPRECORDER_CAPTURE_WINVER_H */
