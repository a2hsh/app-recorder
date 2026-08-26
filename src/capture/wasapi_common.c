/*
 * wasapi_common.c -- shared open / format / event / pump plumbing.
 *
 * Both real capture kinds run this loop. The differences between them are
 * confined to `device_mode` and the tick callback, so a fix to the drain path
 * lands on both at once.
 *
 * It also owns the capture's apartment -- see the long note in the header for
 * why that is here and not in the caller.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#include "wasapi_common.h"
#include "join.h"
#include "log.h"

/* How long a close waits for the pump before concluding it is wedged inside
 * WASAPI and abandoning it. A pump whose longest wait is 200 ms and that has
 * not left the drain loop in 5 s is not merely slow. */
#define APR_WASAPI_JOIN_MS 5000u

/* ksmedia.h speaker positions, spelled out rather than pulled in. */
#define APR_SPEAKER_FRONT_LEFT   0x1u
#define APR_SPEAKER_FRONT_RIGHT  0x2u
#define APR_SPEAKER_FRONT_CENTER 0x4u

const GUID apr_clsid_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E } };
const GUID apr_iid_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6 } };
const GUID apr_iid_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2 } };
const GUID apr_iid_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17 } };
const GUID apr_iid_IAudioSessionManager2 =
    { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B,0xC7,0x2C,0x65,0x4C,0x9A,0x9B,0x6F } };
const GUID apr_iid_IAudioSessionControl2 =
    { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F,0xA2,0x07,0xC9,0x50,0xBE,0x9C,0x6D } };
const GUID apr_iid_ISimpleAudioVolume =
    { 0x87CE5498, 0x68D6, 0x44E5, { 0x92,0x15,0x6D,0xA4,0x7E,0xF8,0x83,0xD8 } };
const GUID apr_ksdataformat_subtype_ieee_float =
    { 0x00000003, 0x0000, 0x0010, { 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 } };

void apr_wasapi_format_f32(WAVEFORMATEXTENSIBLE *wfx,
                           uint32_t sample_rate, uint16_t channels)
{
    DWORD mask;
    WORD  block = (WORD)(channels * 4u);

    ZeroMemory(wfx, sizeof(*wfx));
    wfx->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx->Format.nChannels       = channels;
    wfx->Format.nSamplesPerSec  = sample_rate;
    wfx->Format.wBitsPerSample  = 32;
    wfx->Format.nBlockAlign     = block;
    wfx->Format.nAvgBytesPerSec = sample_rate * block;
    wfx->Format.cbSize          =
        (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wfx->Samples.wValidBitsPerSample = 32;

    if (channels == 1)      mask = APR_SPEAKER_FRONT_CENTER;
    else if (channels == 2) mask = APR_SPEAKER_FRONT_LEFT | APR_SPEAKER_FRONT_RIGHT;
    else                    mask = (DWORD)((1u << channels) - 1u);
    wfx->dwChannelMask = mask;
    wfx->SubFormat     = apr_ksdataformat_subtype_ieee_float;
}

void apr_wasapi_stream_init(AprWasapiStream *s, RingBuf *rb, AprCapStatus *st,
                            uint32_t sample_rate, uint16_t channels,
                            int device_mode,
                            AprWasapiTick tick, void *tick_user)
{
    ZeroMemory(s, sizeof(*s));
    s->rb          = rb;
    s->st          = st;
    s->sample_rate = sample_rate;
    s->channels    = channels;
    s->frame_bytes = (uint16_t)(channels * 4u);
    s->device_mode = device_mode;
    s->tick        = tick;
    s->tick_user   = tick_user;
    apr_clock_init(&s->clock, apr_qpc_freq(), sample_rate);
}

AprErr apr_wasapi_prepare(AprWasapiStream *s, IAudioClient *ac,
                          DWORD extra_flags)
{
    WAVEFORMATEXTENSIBLE wfx;
    HRESULT hr;

    s->ac = ac;

    apr_wasapi_format_f32(&wfx, s->sample_rate, s->channels);

    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extra_flags,
                                 0 /* hnsBufferDuration: see header */,
                                 0 /* hnsPeriodicity */,
                                 &wfx.Format, NULL);
    if (FAILED(hr)) {
        AprErr e = APR_ERR_HR(hr, L"IAudioClient::Initialize %u Hz / %u ch / f32",
                              s->sample_rate, (unsigned)s->channels);
        IAudioClient_Release(ac);
        s->ac = NULL;
        return e;
    }

    s->ev = CreateEventW(NULL, FALSE /* auto-reset */, FALSE, NULL);
    if (!s->ev) return APR_ERR_LAST(L"CreateEvent for the WASAPI stream event");

    hr = IAudioClient_SetEventHandle(ac, s->ev);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"IAudioClient::SetEventHandle");

    hr = IAudioClient_GetService(ac, &apr_iid_IAudioCaptureClient,
                                 (void **)&s->cc);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"GetService(IAudioCaptureClient)");

    {
        UINT32 bufsize = 0;
        if (SUCCEEDED(IAudioClient_GetBufferSize(ac, &bufsize)) && bufsize) {
            APR_DEBUG(L"capture buffer %u frames (%.2f ms) at %u Hz",
                      bufsize, (double)bufsize * 1000.0 / (double)s->sample_rate,
                      s->sample_rate);
        }
    }
    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * The packet timeline. See wasapi_common.h for why this is a pure function
 * with its own tests rather than four lines inside drain().
 * ------------------------------------------------------------------------- */

uint64_t apr_wasapi_packet_start(uint64_t arrival_ticks, uint32_t frames,
                                 uint64_t qpc_freq, uint32_t sample_rate)
{
    uint64_t span;

    if (frames == 0 || qpc_freq == 0 || sample_rate == 0) return arrival_ticks;
    span = apr_frames_to_ticks(frames, qpc_freq, sample_rate);
    return span >= arrival_ticks ? 0 : arrival_ticks - span;
}

uint64_t apr_wasapi_packet_gap(AprClock *clock, uint64_t delivered,
                               uint64_t arrival_ticks, uint32_t frames,
                               DWORD flags)
{
    uint64_t start;
    AprDrift d;

    if (!clock || frames == 0) return 0;

    /* When this packet's first frame was captured -- NOT when we saw it. */
    start = apr_wasapi_packet_start(arrival_ticks, frames,
                                    clock->qpc_freq, clock->sample_rate);

    if (!apr_clock_anchored(clock)) {
        uint64_t back;

        /* A buffer with no usable timestamp must not set the anchor (design
         * 5.2 step 4), so by the time one may, frames can already have been
         * delivered. Walk back over them: the anchor is frame 0's capture
         * time, not this packet's. */
        if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) return 0;
        back = apr_frames_to_ticks(delivered, clock->qpc_freq,
                                   clock->sample_rate);
        apr_clock_anchor(clock, back >= start ? 0 : start - back);
        return 0;   /* nothing can be missing before frame 0 */
    }

    if (!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) return 0;
    if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)       return 0;

    /* `delivered` counts everything up to but NOT including this packet, and
     * `start` is when this packet's first frame was captured, so the two are
     * measured at the same instant on the same timeline. Their difference is
     * exactly what the engine failed to hand over. */
    d = apr_clock_drift(clock, start, delivered);
    return d.delta_frames > 0 ? (uint64_t)d.delta_frames : 0;
}

/* ---------------------------------------------------------------------------
 * The pump
 * ------------------------------------------------------------------------- */

/* Death signals. WASAPI reports these for a device whose endpoint went away;
 * it never reports anything at all for a process tap (design 4.1 note 6),
 * which is why capture_process.c has its own detector. */
static int hresult_is_fatal(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_NOT_INITIALIZED;
}

static void pump_fail(AprWasapiStream *s, const AprErr *e, HRESULT hr)
{
    apr_capstat_set_error(s->st, e);
    APR_LOG_ERR(APR_LOG_ERROR, e);
    if (hresult_is_fatal(hr)) apr_capstat_set_alive(s->st, 0);
}

/* Drain every packet WASAPI currently holds. Returns 0 when the stream has
 * failed fatally and the pump should exit. */
static int drain(AprWasapiStream *s)
{
    UINT32 packet = 0;
    HRESULT hr;

    hr = IAudioCaptureClient_GetNextPacketSize(s->cc, &packet);
    if (FAILED(hr)) {
        AprErr e = APR_ERR_HR(hr, L"GetNextPacketSize");
        pump_fail(s, &e, hr);
        return !hresult_is_fatal(hr);
    }

    while (packet > 0) {
        BYTE  *data   = NULL;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        UINT64 devpos = 0, qpcpos = 0;  /* both deliberately ignored, see below */
        uint64_t now, fill;

        hr = IAudioCaptureClient_GetBuffer(s->cc, &data, &frames, &flags,
                                           &devpos, &qpcpos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) {
            AprErr e = APR_ERR_HR(hr, L"IAudioCaptureClient::GetBuffer");
            pump_fail(s, &e, hr);
            return !hresult_is_fatal(hr);
        }

        /* THE anchor. Not pu64QPCPosition -- for a process tap that field is
         * the frame counter rescaled and carries no clock information at all,
         * and pu64DevicePosition is permanently 0 (design 4.1 notes 3 and 4).
         * QPC read here, at arrival, is the only honest timestamp available. */
        now = apr_qpc_now();

        /* Anchoring and gap measurement, in one place and on one timeline:
         * the capture time of this packet's FIRST frame, which is `now` less
         * the packet's own span. Reading the anchor off arrival instead put
         * every source one packet late and made the two numbers below
         * disagree whenever packet sizes differed. */
        fill = apr_wasapi_packet_gap(&s->clock, s->frames, now, frames, flags);
        if (apr_clock_anchored(&s->clock) && s->st->anchor_ticks == 0)
            s->st->anchor_ticks = (LONG64)s->clock.anchor_ticks;

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            s->discont++;
            s->st->discontinuities = (LONG64)s->discont;

            if (!s->device_mode) {
                /* The spike saw this exactly never in ~190 s of process
                 * loopback, and design 5.1 says it cannot happen. It is still
                 * filled -- see apr_wasapi_packet_gap: "arrives perfect" is a
                 * measurement of an engine keeping up, not a promise about one
                 * that has just reported dropping audio, and a process tap is
                 * the REFERENCE timeline, so a silent shear there desyncs
                 * every bus that reads it against every bus that does not. */
                APR_WARN(L"DATA_DISCONTINUITY on a process tap: design 5.1 "
                         L"says this cannot happen; filling %llu frames to "
                         L"keep the reference timeline aligned",
                         (unsigned long long)fill);
            }
            if (fill == 0) {
                APR_WARN(L"discontinuity with no usable timestamp or no "
                         L"measurable gap; not filling");
            }
        }

        if (fill > 0) {
            uint64_t cap = (uint64_t)rb_capacity_frames(s->rb);
            if (fill > cap) fill = cap;
            rb_write_silence(s->rb, (size_t)fill);
            s->frames += fill;
            s->st->frames_written = (LONG64)s->frames;
            APR_WARN(L"capture gap: filled %llu frames of silence",
                     (unsigned long long)fill);
        }

        if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
            /* Design 5.2 step 4: fall back to frame counting for this buffer
             * and do not let it near the anchor. Nothing else to do -- the
             * frames themselves are fine. */
            APR_WARN(L"TIMESTAMP_ERROR on %u frames; counting frames only",
                     frames);
        }

        if (frames > 0) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                /* SILENT means pData is undefined and must not be copied. */
                rb_write_silence(s->rb, frames);
            } else if (data) {
                rb_write(s->rb, data, frames);
            } else {
                rb_write_silence(s->rb, frames);
            }
            s->frames += frames;
            s->st->frames_written = (LONG64)s->frames;
        }

        hr = IAudioCaptureClient_ReleaseBuffer(s->cc, frames);
        if (FAILED(hr)) {
            AprErr e = APR_ERR_HR(hr, L"IAudioCaptureClient::ReleaseBuffer");
            pump_fail(s, &e, hr);
            return !hresult_is_fatal(hr);
        }

        hr = IAudioCaptureClient_GetNextPacketSize(s->cc, &packet);
        if (FAILED(hr)) {
            AprErr e = APR_ERR_HR(hr, L"GetNextPacketSize");
            pump_fail(s, &e, hr);
            return !hresult_is_fatal(hr);
        }
    }
    return 1;
}

/* The drain loop. Runs on the capture thread between start() and stop(); the
 * same thread services jobs the rest of the time (see the header). */
static void pump(AprWasapiStream *s)
{
    HANDLE waits[2];
    HANDLE mmcss;
    DWORD  task = 0;

    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);

    waits[0] = s->stop_ev;
    waits[1] = s->ev;

    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;             /* stop requested */
        if (w == WAIT_FAILED) {
            AprErr e = APR_ERR_LAST(L"WaitForMultipleObjects in the capture pump");
            apr_capstat_set_error(s->st, &e);
            break;
        }
        if (!drain(s)) break;
        if (s->tick) s->tick(s->tick_user);
    }

    /* One last sweep so frames already handed to us are not thrown away. */
    (void)drain(s);

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    /* Stopping the client is a COM call and belongs on this thread with every
     * other one. */
    if (s->ac) IAudioClient_Stop(s->ac);
}

/* ---------------------------------------------------------------------------
 * The apartment
 * ------------------------------------------------------------------------- */

enum { APR_WJ_NONE = 0, APR_WJ_CALL, APR_WJ_RUN, APR_WJ_QUIT };

static AprErr client_start(AprWasapiStream *s)
{
    HRESULT hr;
    if (!s->ac || !s->cc) return APR_ERR(APR_E_STATE, L"stream is not prepared");
    hr = IAudioClient_Start(s->ac);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"IAudioClient::Start");
    return apr_ok();
}

static DWORD WINAPI capture_thread(LPVOID param)
{
    AprWasapiStream *s = (AprWasapiStream *)param;

    /* The one CoInitializeEx in the capture layer. Everything COM the capture
     * ever does happens on this thread, so no caller has to be in any
     * particular apartment and nothing has to be marshalled between them. */
    s->com_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetEvent(s->ready_ev);
    if (FAILED(s->com_hr)) return 0;

    for (;;) {
        if (WaitForSingleObject(s->job_ev, INFINITE) != WAIT_OBJECT_0) break;

        if (s->job == APR_WJ_QUIT) { SetEvent(s->done_ev); break; }

        if (s->job == APR_WJ_CALL) {
            s->job_err = s->job_fn ? s->job_fn(s->job_user) : apr_ok();
            SetEvent(s->done_ev);
            continue;
        }

        /* APR_WJ_RUN: start the client, answer start() -- which must not block
         * for the length of the recording -- and then BE the pump until stop.
         * idle_ev is what stop() waits on, so it is set on both paths. */
        s->job_err = client_start(s);
        if (apr_failed(&s->job_err)) {
            SetEvent(s->idle_ev);
            SetEvent(s->done_ev);
        } else {
            SetEvent(s->done_ev);
            pump(s);
            SetEvent(s->idle_ev);
        }
    }

    CoUninitialize();
    return 0;
}

/* Post a job and wait. The caller holds s->lock. Waiting on the thread handle
 * as well means a capture thread that died (it cannot, but) surfaces as an
 * error rather than as a hang. */
static AprErr post(AprWasapiStream *s, int job)
{
    HANDLE w[2];

    s->job = job;
    w[0] = s->done_ev;
    w[1] = s->thread;
    SetEvent(s->job_ev);

    if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
        return APR_ERR(APR_E_STATE,
                       L"the capture thread exited without answering");
    }
    return s->job_err;
}

static void close_handle(HANDLE *h)
{
    if (*h) { CloseHandle(*h); *h = NULL; }
}

AprErr apr_wasapi_thread_start(AprWasapiStream *s)
{
    s->ready_ev = CreateEventW(NULL, TRUE  /* manual */, FALSE, NULL);
    s->job_ev   = CreateEventW(NULL, FALSE /* auto   */, FALSE, NULL);
    s->done_ev  = CreateEventW(NULL, FALSE /* auto   */, FALSE, NULL);
    s->idle_ev  = CreateEventW(NULL, TRUE  /* manual */, TRUE,  NULL);
    s->stop_ev  = CreateEventW(NULL, TRUE  /* manual */, FALSE, NULL);
    if (!s->ready_ev || !s->job_ev || !s->done_ev || !s->idle_ev || !s->stop_ev)
        return APR_ERR_LAST(L"CreateEvent for the capture thread");

    InitializeCriticalSection(&s->lock);
    s->lock_init = 1;

    s->thread = CreateThread(NULL, 0, capture_thread, s, 0, &s->thread_id);
    if (!s->thread) return APR_ERR_LAST(L"CreateThread for the capture thread");

    if (WaitForSingleObject(s->ready_ev, 10000) != WAIT_OBJECT_0)
        return APR_ERR(APR_E_TIMEOUT, L"the capture thread did not start");
    if (FAILED(s->com_hr))
        return APR_ERR_HR(s->com_hr,
                          L"CoInitializeEx(MTA) on the capture thread");
    return apr_ok();
}

/* Test seam; see wasapi_common.h. A file-static rather than a stream field so
 * that a production build's AprWasapiStream is exactly what it was. */
static AprWasapiRaceHook g_race_hook;
static void             *g_race_user;

void apr_wasapi_test_set_race_hook(AprWasapiRaceHook hook, void *user)
{
    g_race_hook = hook;
    g_race_user = user;
}

AprErr apr_wasapi_call(AprWasapiStream *s, AprWasapiJob job, void *user)
{
    AprErr e;

    if (!s->thread || !s->lock_init)
        return APR_ERR(APR_E_STATE, L"the capture has no thread");

    /* Already inside the apartment -- a tick callback, most likely. The thread
     * cannot post to itself, and it does not need to. */
    if (GetCurrentThreadId() == s->thread_id)
        return job ? job(user) : apr_ok();

    /* Between start() and stop() the thread is the pump and is not reading the
     * queue. Say so instead of blocking forever.
     *
     * THE TEST IS INSIDE THE LOCK, and that is the whole of it. Read outside,
     * it was a TOCTOU against apr_wasapi_start(): both would find `running`
     * clear, start would win the lock, post APR_WJ_RUN and turn the thread
     * into the pump, and this call would then post a job into a queue nobody
     * was serving and wait on it forever -- post() waits INFINITE by design,
     * because a job that has been accepted always completes. start() raises
     * `running` BEFORE it takes the lock, so whoever gets the lock second sees
     * the truth. */
    if (g_race_hook) g_race_hook(s, g_race_user);

    EnterCriticalSection(&s->lock);
    if (InterlockedCompareExchange(&s->running, 0, 0) != 0) {
        LeaveCriticalSection(&s->lock);
        return APR_ERR(APR_E_STATE, L"the capture thread is pumping");
    }
    s->job_fn   = job;
    s->job_user = user;
    e = post(s, APR_WJ_CALL);
    LeaveCriticalSection(&s->lock);
    return e;
}

AprErr apr_wasapi_start(AprWasapiStream *s)
{
    AprErr e;

    if (!s->thread) return APR_ERR(APR_E_STATE, L"stream is not prepared");
    if (!s->ac || !s->cc) return APR_ERR(APR_E_STATE, L"stream is not prepared");
    if (InterlockedCompareExchange(&s->running, 1, 0) != 0) return apr_ok();

    EnterCriticalSection(&s->lock);
    ResetEvent(s->stop_ev);
    ResetEvent(s->idle_ev);
    e = post(s, APR_WJ_RUN);
    LeaveCriticalSection(&s->lock);

    if (apr_failed(&e)) {
        InterlockedExchange(&s->running, 0);
        SetEvent(s->idle_ev);      /* harmless if the thread already did */
    }
    return e;
}

void apr_wasapi_stop(AprWasapiStream *s)
{
    InterlockedExchange(&s->running, 0);
    if (s->stop_ev) SetEvent(s->stop_ev);

    if (!s->thread) return;

    /* stop() is documented safe from any thread, and the tick callback runs on
     * the capture thread. Waiting for ourselves would deadlock, so the pump
     * just unwinds. */
    if (GetCurrentThreadId() == s->thread_id) return;

    EnterCriticalSection(&s->lock);
    if (apr_join_wait2(s->idle_ev, s->thread, APR_WASAPI_JOIN_MS) ==
        APR_JOIN_ABANDONED)
    {
        /* A pump whose longest wait is 200 ms and that has not left the drain
         * loop in 5 s is wedged inside WASAPI. Killing it mid-write would
         * corrupt the ring, so mark the stream unfreeable instead: leaking a
         * COM reference and a thread is survivable, a use-after-free under an
         * audio thread is not. close() turns this into an AprErr the layer
         * that owns the ring is handed -- see join.h. */
        s->pump_stuck = 1;
        APR_ERROR(L"capture pump did not exit within 5 s; leaking the "
                  L"stream rather than freeing it under a live thread");
    }
    LeaveCriticalSection(&s->lock);
}

/* Everything close() has to do inside the apartment, in one job: the
 * implementation's own COM objects first, then this layer's. */
static AprErr close_job(void *user)
{
    AprWasapiStream *s = (AprWasapiStream *)user;

    if (s->teardown) {
        (void)s->teardown(s->teardown_user);
        s->teardown = NULL;
    }
    if (s->cc) { IAudioCaptureClient_Release(s->cc); s->cc = NULL; }
    if (s->ac) { IAudioClient_Release(s->ac); s->ac = NULL; }
    return apr_ok();
}

AprErr apr_wasapi_close(AprWasapiStream *s, AprWasapiJob teardown, void *user)
{
    apr_wasapi_stop(s);

    /* see apr_wasapi_stop; nothing may be freed */
    if (s->pump_stuck) return APR_ERR_ABANDONED(L"the capture pump");

    /* A capture cannot retire the thread it is running on. Nothing does this,
     * but hanging is a worse way to find out than a log line is. */
    if (s->thread && GetCurrentThreadId() == s->thread_id) {
        s->pump_stuck = 1;
        APR_ERROR(L"close() called from the capture's own thread; leaking the "
                  L"stream rather than joining a thread with itself");
        return APR_ERR(APR_E_STATE,
                       L"close() was called from the capture's own thread; "
                       L"the stream is leaked rather than joined with itself");
    }

    if (s->thread) {
        s->teardown      = teardown;
        s->teardown_user = user;

        EnterCriticalSection(&s->lock);
        s->job_fn   = close_job;
        s->job_user = s;
        (void)post(s, APR_WJ_CALL);
        (void)post(s, APR_WJ_QUIT);
        LeaveCriticalSection(&s->lock);

        if (apr_join_wait(s->thread, APR_WASAPI_JOIN_MS) == APR_JOIN_ABANDONED) {
            s->pump_stuck = 1;
            APR_ERROR(L"capture thread did not exit within 5 s; leaking the "
                      L"stream rather than freeing it under a live thread");
            return APR_ERR_ABANDONED(L"the capture thread");
        }
        close_handle(&s->thread);
        s->thread_id = 0;
    } else if (teardown) {
        /* The thread was never created, so open() failed before it could make
         * any COM object -- there is nothing to marshal and nothing to be in
         * the wrong apartment for. */
        (void)teardown(user);
    }

    if (s->lock_init) { DeleteCriticalSection(&s->lock); s->lock_init = 0; }

    close_handle(&s->ev);
    close_handle(&s->stop_ev);
    close_handle(&s->ready_ev);
    close_handle(&s->job_ev);
    close_handle(&s->done_ev);
    close_handle(&s->idle_ev);
    return apr_ok();
}
