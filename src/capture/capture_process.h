/*
 * capture_process.h -- the test probe for the process tap. Not public.
 *
 * capture.h is deliberately identical for all three source kinds, so it has
 * no way to ask "which thread did that happen on?". This is that door, and it
 * exists for one assertion: THE MUTE POLL MUST NOT RUN ON THE PUMP THREAD.
 *
 * That is not a style preference. Mute detection is a cross-process COM round
 * trip into the audio service with no bound on it, and a process tap is the
 * reference timeline (design 5.1): a pump that stalls inside one drops packets
 * and shears every bus reading that tap against every bus that is not,
 * permanently and silently. The property is invisible from the outside -- the
 * recording still sounds fine -- so it is asserted structurally instead.
 */
#ifndef APPRECORDER_CAPTURE_PROCESS_H
#define APPRECORDER_CAPTURE_PROCESS_H

#include <windows.h>

#include "capture.h"

typedef struct AprProcProbe {
    DWORD pump_thread_id;   /* the thread that drains WASAPI packets */
    DWORD mute_thread_id;   /* 0 when there is no poller (EXCLUDE mode) */
    long  mute_polls;       /* polls attempted since open */
} AprProcProbe;

/* Fills `out` for an open process capture. Returns 0 (and zeroes `out`) for
 * anything else, so a test can tell "not a process tap" from "no polls yet".
 * Reads published counters only: never blocks, never allocates. */
int apr_capture_process_probe(const AprCapture *c, AprProcProbe *out);

#endif /* APPRECORDER_CAPTURE_PROCESS_H */
