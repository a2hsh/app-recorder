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
 * Everything about the timeline is deliberately absent: a process tap is the
 * reference timeline (design 5.1). No gap synthesis, no drift correction, no
 * discontinuity handling. It arrives perfect; it is treated as perfect.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdlib.h>

#include "capture_internal.h"
#include "com_shim.h"
#include "wasapi_common.h"
#include "clock.h"
#include "log.h"

#define PROC_ACTIVATE_TIMEOUT_MS  5000u
#define PROC_DEATH_POLL_MS         200u
#define PROC_MUTE_POLL_MS          500u
#define PROC_SESSION_SEARCH_MS    2000u

typedef struct ProcImpl {
    AprWasapiStream s;
    AprCapStatus    st;

    uint32_t pid;
    int      exclude;

    HANDLE   hproc;                /* SYNCHRONIZE only; NULL in exclude mode */
    int      death_reported;

    ISimpleAudioVolume *vol;       /* cached session volume for pid */

    uint64_t ticks_per_death_poll;
    uint64_t ticks_per_mute_poll;
    uint64_t ticks_per_search;
    uint64_t next_death_ticks;
    uint64_t next_mute_ticks;
    uint64_t next_search_ticks;

    int      com_owned;            /* this source called CoInitializeEx */
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

/* ---------------------------------------------------------------------------
 * Pump tick
 * ------------------------------------------------------------------------- */
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

    if (!p->exclude && now >= p->next_mute_ticks) {
        p->next_mute_ticks = now + p->ticks_per_mute_poll;
        poll_mute(p, now);
    }
}

/* ---------------------------------------------------------------------------
 * VTable
 * ------------------------------------------------------------------------- */

static AprErr proc_open(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb)
{
    ProcImpl *p;
    IAudioClient *ac = NULL;
    HRESULT hr_com;
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
    p->ticks_per_mute_poll  = ticks_for_ms(PROC_MUTE_POLL_MS);
    p->ticks_per_search     = ticks_for_ms(PROC_SESSION_SEARCH_MS);

    /* Design 4.1 note 7: the activation callback cannot be delivered to an STA,
     * and IAgileObject on the handler does not rescue it. Fail with something
     * readable rather than a bare HRESULT.
     * NOTE: open() and close() must run on the same thread, because this
     * reference is released there. */
    hr_com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr_com == RPC_E_CHANGED_MODE)
        return APR_ERR(APR_E_STATE,
                       L"process loopback requires the MTA; this thread is an STA");
    if (FAILED(hr_com)) return APR_ERR_HR(hr_com, L"CoInitializeEx(MTA)");
    p->com_owned = 1;

    if (p->exclude) {
        APR_WARN(L"process source pid %u opened in EXCLUDE mode: this records "
                 L"EVERYTHING the machine plays except that process tree",
                 p->pid);
    }

    e = apr_com_activate_process_loopback(p->pid, p->exclude,
                                          PROC_ACTIVATE_TIMEOUT_MS, &ac);
    if (apr_failed(&e)) return e;

    apr_wasapi_stream_init(&p->s, rb, &p->st, cfg->sample_rate, cfg->channels,
                           0 /* not device_mode: no drift, no gaps */,
                           proc_tick, p);

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

static void proc_close(AprCapture *c)
{
    ProcImpl *p = (ProcImpl *)c->impl;
    if (!p) return;

    apr_wasapi_close(&p->s);

    if (p->vol)   { ISimpleAudioVolume_Release(p->vol); p->vol = NULL; }
    if (p->hproc) { CloseHandle(p->hproc); p->hproc = NULL; }
    if (p->com_owned) { CoUninitialize(); p->com_owned = 0; }

    c->impl = NULL;
    free(p);
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
