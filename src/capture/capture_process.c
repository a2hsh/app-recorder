/*
 * capture_process.c -- WASAPI process loopback (design section 4.1).
 *
 * Two things WASAPI will not do for us, and this file therefore must:
 *
 *   DEATH. Process loopback keeps delivering perfect, gapless silence forever
 *   after the target exits, with no error and no flag (spike, measured). So a
 *   SYNCHRONIZE handle from OpenProcess is the only honest death signal there
 *   is (design section 10). Without it, closing Teams mid-session yields hours
 *   of silence that looks like a successful recording.
 *
 *   MUTE. Loopback sits post-session-volume: an app muted in the Windows mixer
 *   records as pure digital silence while the engine cheerfully reports it
 *   rendering (spike: session volume 1e-4 gave captured peak 0.000025, exactly
 *   0.25 x 1e-4; volume 0 gave 0 of 1,534,080 samples non-zero). That is
 *   indistinguishable from death and from a quiet app, so ISimpleAudioVolume is
 *   polled and the answer published.
 *
 * Everything about the timeline is deliberately absent from THIS file: a
 * process tap is the reference timeline (design 5.1). No drift correction and
 * no discontinuity handling here. (The shared pump does fill a gap the engine
 * explicitly reports on a process tap -- see apr_wasapi_packet_gap -- but that
 * is a loss the engine announced, not synthesis.)
 *
 * WHY MUTE POLLING HAS ITS OWN THREAD
 *
 *   It used to run inline on the pump, and that made the pump's worst case a
 *   COM round trip: an ISimpleAudioVolume RPC every 500 ms, and -- whenever
 *   the target has no session yet, which is the ordinary idle case -- a full
 *   every-render-endpoint x every-session enumeration every 2 s. Both are
 *   cross-process RPC into the audio service, and neither has a bound.
 *
 *   The cost of that landing late is not a dropped mute warning. It is that
 *   WASAPI drops packets while the pump is not draining and sets
 *   DATA_DISCONTINUITY -- on the source that IS the reference timeline. A
 *   shear there moves every bus reading this tap against every bus that is
 *   not, permanently and silently, which is the single failure the whole clock
 *   design exists to prevent.
 *
 *   So the poll moved off the pump entirely rather than being bounded: there
 *   is no timeout knob on a COM call, and "bounding" it would have meant
 *   another thread anyway. Death detection stays on the pump because it is a
 *   zero-timeout WaitForSingleObject on a handle -- no RPC, no enumeration,
 *   no unbounded anything.
 *
 *   This does not weaken design 4.2.1. The rule there is that a capture owns
 *   its apartment so its CALLER never has to think about one; the mute thread
 *   enters its own MTA and creates, uses and releases its own COM objects on
 *   it, touching nothing the pump touches. Two threads, two apartments, no
 *   object crossing between them.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdlib.h>

#include "capture_internal.h"
#include "capture_process.h"
#include "com_shim.h"
#include "wasapi_common.h"
#include "clock.h"
#include "join.h"
#include "log.h"

#define PROC_ACTIVATE_TIMEOUT_MS  5000u
#define PROC_DEATH_POLL_MS         200u
#define PROC_MUTE_POLL_MS          500u
#define PROC_SESSION_SEARCH_MS    2000u

/* How long close() waits for the mute thread. It only ever sleeps or sits in
 * a COM call, so this is generous; a thread still inside a wedged RPC after it
 * is abandoned rather than freed under, exactly like the pump. */
#define PROC_MUTE_JOIN_MS         5000u

typedef struct ProcImpl {
    AprWasapiStream s;
    AprCapStatus    st;

    uint32_t pid;
    int      exclude;

    HANDLE   hproc;                /* SYNCHRONIZE only; NULL in exclude mode */
    int      death_reported;

    /* The mute poller. Its own thread, its own MTA, its own COM objects --
     * see the header comment for why this is not on the pump. `vol` is
     * touched ONLY by that thread. */
    HANDLE   mute_thread;
    HANDLE   mute_stop_ev;
    DWORD    mute_thread_id;
    ISimpleAudioVolume *vol;       /* mute thread only */
    volatile LONG mute_polls;      /* diagnostics and the test probe */

    uint64_t ticks_per_death_poll;
    uint64_t ticks_per_search;
    uint64_t next_death_ticks;
    uint64_t next_search_ticks;
} ProcImpl;

static uint64_t ticks_for_ms(uint64_t ms)
{
    return apr_mul_div_u64(ms, apr_qpc_freq(), 1000u, NULL);
}

/* ---------------------------------------------------------------------------
 * Mute detection
 *
 * Every active render endpoint is searched, not just the default one: an app
 * can be playing to any of them, and on this author's rig several are live at
 * once. The result is cached, because enumerating sessions is far too
 * expensive to do on an audio thread every tick.
 *
 * Limitation worth knowing: INCLUDE_TARGET_PROCESS_TREE captures children too,
 * but only the named PID has its session inspected. A muted child inside an
 * unmuted parent tree will not raise the flag.
 * ------------------------------------------------------------------------- */
static ISimpleAudioVolume *find_session_volume(uint32_t pid)
{
    IMMDeviceEnumerator *enu = NULL;
    IMMDeviceCollection *coll = NULL;
    ISimpleAudioVolume  *found = NULL;
    UINT count = 0, i;
    HRESULT hr;

    hr = CoCreateInstance(&apr_clsid_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &apr_iid_IMMDeviceEnumerator, (void **)&enu);
    if (FAILED(hr)) return NULL;

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enu, eRender, DEVICE_STATE_ACTIVE,
                                                &coll);
    if (FAILED(hr)) { IMMDeviceEnumerator_Release(enu); return NULL; }

    if (FAILED(IMMDeviceCollection_GetCount(coll, &count))) count = 0;

    for (i = 0; i < count && !found; i++) {
        IMMDevice               *dev = NULL;
        IAudioSessionManager2   *mgr = NULL;
        IAudioSessionEnumerator *sen = NULL;
        int n = 0, k;

        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev))) continue;

        if (SUCCEEDED(IMMDevice_Activate(dev, &apr_iid_IAudioSessionManager2,
                                         CLSCTX_ALL, NULL, (void **)&mgr)) &&
            SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &sen)) &&
            SUCCEEDED(IAudioSessionEnumerator_GetCount(sen, &n)))
        {
            for (k = 0; k < n && !found; k++) {
                IAudioSessionControl  *ctl  = NULL;
                IAudioSessionControl2 *ctl2 = NULL;
                DWORD spid = 0;

                if (FAILED(IAudioSessionEnumerator_GetSession(sen, k, &ctl)))
                    continue;

                if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                        ctl, &apr_iid_IAudioSessionControl2, (void **)&ctl2)))
                {
                    if (SUCCEEDED(IAudioSessionControl2_GetProcessId(ctl2, &spid)) &&
                        spid == pid)
                    {
                        (void)IAudioSessionControl2_QueryInterface(
                            ctl2, &apr_iid_ISimpleAudioVolume, (void **)&found);
                    }
                    IAudioSessionControl2_Release(ctl2);
                }
                IAudioSessionControl_Release(ctl);
            }
        }

        if (sen) IAudioSessionEnumerator_Release(sen);
        if (mgr) IAudioSessionManager2_Release(mgr);
        IMMDevice_Release(dev);
    }

    IMMDeviceCollection_Release(coll);
    IMMDeviceEnumerator_Release(enu);
    return found;
}

static void poll_mute(ProcImpl *p, uint64_t now)
{
    BOOL  mute = FALSE;
    float v = 1.0f;
    HRESULT hr;

    InterlockedIncrement(&p->mute_polls);

    if (!p->vol) {
        if (now < p->next_search_ticks) return;
        p->next_search_ticks = now + p->ticks_per_search;
        p->vol = find_session_volume(p->pid);
        /* No session at all means the app has never rendered, which is not the
         * same thing as muted. Leave the flag alone. */
        if (!p->vol) return;
    }

    hr = ISimpleAudioVolume_GetMute(p->vol, &mute);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_GetMasterVolume(p->vol, &v);
    if (FAILED(hr)) {
        ISimpleAudioVolume_Release(p->vol);
        p->vol = NULL;
        return;
    }

    /* Both record as pure silence, so both raise the same flag. */
    apr_capstat_set_muted(&p->st, (mute || v == 0.0f) ? 1 : 0);
}

/* The mute poller's thread. Everything COM it touches is created and released
 * here, in this thread's own MTA -- nothing crosses to the pump. It runs for
 * the whole life of the capture rather than only between start() and stop(),
 * because "the app you picked is muted" is worth saying BEFORE a recording
 * starts, and it costs one sleeping thread to say it. */
static DWORD WINAPI mute_thread(LPVOID param)
{
    ProcImpl *p  = (ProcImpl *)param;
    HRESULT   hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (FAILED(hr)) {
        APR_WARN(L"mute poller could not enter the MTA (0x%08lx); this source "
                 L"cannot report a muted target", (unsigned long)hr);
        return 0;
    }

    for (;;) {
        poll_mute(p, apr_qpc_now());
        if (WaitForSingleObject(p->mute_stop_ev, PROC_MUTE_POLL_MS) ==
            WAIT_OBJECT_0)
            break;
    }

    if (p->vol) { ISimpleAudioVolume_Release(p->vol); p->vol = NULL; }
    CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------------------------
 * Pump tick
 * ------------------------------------------------------------------------- */
/* Runs on the PUMP thread, between audio packets. Nothing here may make a COM
 * call, take a lock the audio service holds, or block for an unbounded time --
 * see the header comment. Death detection qualifies: it is a zero-timeout wait
 * on a handle we already hold. */
static void proc_tick(void *user)
{
    ProcImpl *p = (ProcImpl *)user;
    uint64_t now = apr_qpc_now();

    if (p->hproc && !p->death_reported && now >= p->next_death_ticks) {
        p->next_death_ticks = now + p->ticks_per_death_poll;
        if (WaitForSingleObject(p->hproc, 0) == WAIT_OBJECT_0) {
            AprErr e = APR_ERR(APR_E_STATE,
                               L"target process %u exited; loopback will keep "
                               L"delivering silence", p->pid);
            p->death_reported = 1;
            apr_capstat_set_error(&p->st, &e);
            apr_capstat_set_alive(&p->st, 0);
            APR_LOG_ERR(APR_LOG_WARN, &e);
            /* Deliberately does NOT stop the pump: design section 10 says one
             * source dying must not take the session down, and the owner
             * decides what to do about it. */
        }
    }
}

/* ---------------------------------------------------------------------------
 * VTable
 * ------------------------------------------------------------------------- */

/* The half of open() that touches COM. Runs on the capture's own thread, in
 * the MTA it owns -- design 4.1 note 7 requires the MTA for the activation
 * callback, and requiring it of the CALLER is what stopped the windowed front
 * end (an STA, necessarily) from adding a single source. See wasapi_common.h. */
static AprErr proc_open_com(void *user)
{
    ProcImpl *p = (ProcImpl *)user;
    IAudioClient *ac = NULL;
    AprErr e;

    if (p->exclude) {
        APR_WARN(L"process source pid %u opened in EXCLUDE mode: this records "
                 L"EVERYTHING the machine plays except that process tree",
                 p->pid);
    }

    e = apr_com_activate_process_loopback(p->pid, p->exclude,
                                          PROC_ACTIVATE_TIMEOUT_MS, &ac);
    if (apr_failed(&e)) return e;

    /* GetMixFormat and GetDevicePeriod are both E_NOTIMPL here (design 4.1
     * note 1). There is nothing to negotiate: we supply the session format and
     * hnsBufferDuration 0. */
    e = apr_wasapi_prepare(&p->s, ac, AUDCLNT_STREAMFLAGS_LOOPBACK);
    if (apr_failed(&e)) return e;

    if (!p->exclude) {
        /* The only reliable death signal there is. */
        p->hproc = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)p->pid);
        if (!p->hproc) {
            DWORD gle = GetLastError();
            if (gle == ERROR_INVALID_PARAMETER) {
                return APR_ERR_WIN32(gle, L"no process with pid %u", p->pid);
            }
            /* Elevated or protected target: capture still works, we just
             * cannot see it die. Say so rather than pretending. */
            APR_WARN(L"OpenProcess(SYNCHRONIZE, %u) failed (%lu); this source "
                     L"cannot detect the target exiting", p->pid, gle);
        }
    } else {
        /* In EXCLUDE mode the named process is the one thing NOT being
         * captured, so its death is irrelevant -- the capture stays valid and
         * simply starts including it. Same for its session volume. */
        APR_DEBUG(L"EXCLUDE mode: no death detection, no mute polling");
    }

    return apr_ok();
}

/* Callable from ANY apartment. Nothing here touches COM: it validates, builds
 * the stream, gives the capture its own MTA thread, and then does the real work
 * over there. capture.h states this as a guarantee. */
static AprErr proc_open(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb)
{
    ProcImpl *p;
    AprErr e;

    e = apr_capture_check_common(cfg, rb);
    if (apr_failed(&e)) return e;
    if (cfg->kind != APR_SRC_PROCESS)
        return APR_ERR(APR_E_INVALID_ARG, L"not a process source");
    if (cfg->process.pid == 0)
        return APR_ERR(APR_E_INVALID_ARG, L"pid 0 cannot be captured");

    p = (ProcImpl *)calloc(1, sizeof(*p));
    if (!p) return APR_ERR(APR_E_NO_MEMORY, L"process capture");
    c->impl = p;

    apr_capstat_init(&p->st);
    p->pid     = cfg->process.pid;
    p->exclude = cfg->process.exclude ? 1 : 0;
    p->ticks_per_death_poll = ticks_for_ms(PROC_DEATH_POLL_MS);
    p->ticks_per_search     = ticks_for_ms(PROC_SESSION_SEARCH_MS);

    /* Must precede the thread: it zeroes the stream, handles and all. */
    apr_wasapi_stream_init(&p->s, rb, &p->st, cfg->sample_rate, cfg->channels,
                           0 /* not device_mode: no drift correction here */,
                           proc_tick, p);
    /* AFTER stream_init, which zeroes the stream. A non-zero resume anchor
     * means this tap is replacing one whose target exited: its first frame
     * belongs at the absolute index that anchor implies, not at the ring's
     * current write position (capture.h). */
    apr_capresume_init(&p->s.resume, cfg);

    e = apr_wasapi_thread_start(&p->s);
    if (apr_failed(&e)) return e;

    e = apr_wasapi_call(&p->s, proc_open_com, p);
    if (apr_failed(&e)) return e;

    /* In EXCLUDE mode the named process is the one thing NOT being captured,
     * so its session volume is irrelevant and there is nothing to poll. */
    if (!p->exclude) {
        p->mute_stop_ev = CreateEventW(NULL, TRUE /* manual */, FALSE, NULL);
        if (!p->mute_stop_ev)
            return APR_ERR_LAST(L"CreateEvent for the mute poller");
        p->mute_thread = CreateThread(NULL, 0, mute_thread, p, 0,
                                      &p->mute_thread_id);
        if (!p->mute_thread) {
            /* Capture is fine without it; the user just loses the "this app is
             * muted" warning. Losing the recording over that would be worse. */
            APR_WARN(L"could not start the mute poller (%lu); this source "
                     L"cannot report a muted target", GetLastError());
        }
    }
    return apr_ok();
}

static AprErr proc_start(AprCapture *c)
{
    ProcImpl *p = (ProcImpl *)c->impl;
    if (!p) return APR_ERR(APR_E_STATE, L"process capture is not open");
    return apr_wasapi_start(&p->s);
}

static void proc_stop(AprCapture *c)
{
    ProcImpl *p = (ProcImpl *)c->impl;
    if (p) apr_wasapi_stop(&p->s);
}

static void proc_status(const AprCapture *c, AprCaptureStatus *out)
{
    const ProcImpl *p = (const ProcImpl *)c->impl;
    if (!out) return;
    if (!p) { ZeroMemory(out, sizeof(*out)); out->last_error = apr_ok(); return; }
    apr_capstat_read(&p->st, out);
}

/* Retire the mute poller. Its ISimpleAudioVolume is released on its own
 * thread, in the apartment it was created in, as that thread unwinds. */
static AprErr proc_close_mute(ProcImpl *p)
{
    if (!p->mute_thread) {
        if (p->mute_stop_ev) { CloseHandle(p->mute_stop_ev); p->mute_stop_ev = NULL; }
        return apr_ok();
    }
    SetEvent(p->mute_stop_ev);
    if (apr_join_wait(p->mute_thread, PROC_MUTE_JOIN_MS) == APR_JOIN_ABANDONED) {
        APR_ERROR(L"the mute poller did not exit within 5 s; leaking the "
                  L"source rather than freeing it under a live thread");
        return APR_ERR_ABANDONED(L"the mute poller");
    }
    CloseHandle(p->mute_thread);
    p->mute_thread    = NULL;
    p->mute_thread_id = 0;
    CloseHandle(p->mute_stop_ev);
    p->mute_stop_ev = NULL;
    return apr_ok();
}

/* No teardown job is passed to apr_wasapi_close below: this file's only COM
 * object outside the shared stream was the ISimpleAudioVolume, and that now
 * belongs to the mute poller's apartment and is released as its thread
 * unwinds. Releasing it from the pump's apartment would be the mirror image
 * of the bug this file used to have on open(). */
static AprErr proc_close(AprCapture *c)
{
    ProcImpl *p = (ProcImpl *)c->impl;
    AprErr    e, me;

    if (!p) return apr_ok();

    /* Both threads get a chance to leave before anything is judged, so that a
     * wedged pump does not hide a wedged poller (or the reverse) from the log.
     * Either one still running means NOTHING here may be freed: both hold
     * pointers into this allocation, and the pump holds the caller's ring. */
    e  = apr_wasapi_close(&p->s, NULL, NULL);
    me = proc_close_mute(p);
    if (!apr_failed(&e)) e = me;
    if (apr_failed(&e)) return e;

    if (p->hproc) { CloseHandle(p->hproc); p->hproc = NULL; }

    c->impl = NULL;
    free(p);
    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * Test probe -- see capture_process.h
 * ------------------------------------------------------------------------- */
int apr_capture_process_probe(const AprCapture *c, AprProcProbe *out)
{
    const ProcImpl *p;

    if (!out) return 0;
    ZeroMemory(out, sizeof(*out));
    if (!c || c->vt != apr_capture_process_vtable() || !c->impl) return 0;

    p = (const ProcImpl *)c->impl;
    out->pump_thread_id = p->s.thread_id;
    out->mute_thread_id = p->mute_thread_id;
    out->mute_polls     = (long)InterlockedCompareExchange(
                              (volatile LONG *)&p->mute_polls, 0, 0);
    return 1;
}

static const AprCaptureVTable g_process_vtable = {
    "process-loopback",
    proc_open,
    proc_start,
    proc_stop,
    proc_status,
    proc_close
};

const AprCaptureVTable *apr_capture_process_vtable(void)
{
    return &g_process_vtable;
}
