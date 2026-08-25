/*
 * com_shim.h -- the hand-written COM vtable boilerplate (design section 7:
 * capture/com_shim.c is its sole owner).
 *
 * There is exactly one COM callback interface in apprecorder --
 * IActivateAudioInterfaceCompletionHandler, needed by
 * ActivateAudioInterfaceAsync -- and it lives here. If another one is ever
 * required, it goes in this file too, not next to its caller.
 */
#ifndef APPRECORDER_CAPTURE_COM_SHIM_H
#define APPRECORDER_CAPTURE_COM_SHIM_H

#include <windows.h>
#include <audioclient.h>
#include <stdint.h>

#include "err.h"

/* Activate a process-loopback IAudioClient for `pid` and block until the async
 * activation completes or `timeout_ms` elapses.
 *
 *   exclude == 0 -> PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
 *                   (capture this app and its children)
 *   exclude != 0 -> PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
 *                   (capture everything the machine plays EXCEPT this app --
 *                    privacy-sensitive, see capture.h)
 *
 * The caller MUST already be in the MTA. Under an STA the activation callback
 * cannot be delivered and IAgileObject on the handler does not save you.
 *
 * On success *out_client holds a reference the caller releases. On failure
 * *out_client is NULL.
 */
AprErr apr_com_activate_process_loopback(uint32_t pid, int exclude,
                                         DWORD timeout_ms,
                                         IAudioClient **out_client);

#endif /* APPRECORDER_CAPTURE_COM_SHIM_H */
