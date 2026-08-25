/*
 * wasapi_common.c -- shared open / format / event / pump plumbing.
 *
 * Both real capture kinds run this loop. The differences between them are
 * confined to `device_mode` and the tick callback, so a fix to the drain path
 * lands on both at once.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#include "wasapi_common.h"
#include "log.h"

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
        uint64_t now;

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

        if (frames > 0 && !apr_clock_anchored(&s->clock) &&
            !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))
        {
            apr_clock_anchor(&s->clock, now);
            s->st->anchor_ticks = (LONG64)now;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            s->discont++;
            s->st->discontinuities = (LONG64)s->discont;

            if (s->device_mode) {
                /* Design 5.2 step 4: a real gap on a device capture. Everything
                 * QPC says should have arrived and did not is filled with
                 * silence, so the timeline stays sample-accurate and the core
                 * resampler only has to deal with steady drift. */
                if (apr_clock_anchored(&s->clock) &&
                    !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))
                {
                    AprDrift d = apr_clock_drift(&s->clock, now, s->frames);
                    if (d.delta_frames > 0) {
                        uint64_t fill = (uint64_t)d.delta_frames;
                        uint64_t cap  = (uint64_t)rb_capacity_frames(s->rb);
                        if (fill > cap) fill = cap;
                        rb_write_silence(s->rb, (size_t)fill);
                        s->frames += fill;
                        s->st->frames_written = (LONG64)s->frames;
                        APR_WARN(L"device gap: filled %llu frames of silence",
                                 (unsigned long long)fill);
                    }
                } else {
                    APR_WARN(L"device discontinuity with no usable timestamp; "
                             L"not filling");
                }
            } else {
                /* The spike saw this exactly never in ~190 s. If it ever fires
                 * on a process tap, design 5.1 needs revisiting -- say so. */
                APR_WARN(L"DATA_DISCONTINUITY on a process tap: design 5.1 says "
                         L"this cannot happen");
            }
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

static DWORD WINAPI pump_thread(LPVOID param)
{
    AprWasapiStream *s = (AprWasapiStream *)param;
    HANDLE  waits[2];
    HANDLE  mmcss;
    DWORD   task = 0;
    HRESULT hr_com;

    /* The tick callback resolves audio sessions through COM (design section 10
     * mute polling), so this thread needs an apartment of its own. MTA, for the
     * same reason the opener needs one. */
    hr_com = CoInitializeEx(NULL, COINIT_MULTITHREADED);

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
    if (SUCCEEDED(hr_com)) CoUninitialize();
    return 0;
}

AprErr apr_wasapi_start(AprWasapiStream *s)
{
    HRESULT hr;

    if (!s->ac || !s->cc) return APR_ERR(APR_E_STATE, L"stream is not prepared");
    if (InterlockedCompareExchange(&s->running, 1, 0) != 0) return apr_ok();

    s->stop_ev = CreateEventW(NULL, TRUE /* manual reset */, FALSE, NULL);
    if (!s->stop_ev) {
        InterlockedExchange(&s->running, 0);
        return APR_ERR_LAST(L"CreateEvent for the capture stop signal");
    }

    hr = IAudioClient_Start(s->ac);
    if (FAILED(hr)) {
        CloseHandle(s->stop_ev);
        s->stop_ev = NULL;
        InterlockedExchange(&s->running, 0);
        return APR_ERR_HR(hr, L"IAudioClient::Start");
    }

    s->thread = CreateThread(NULL, 0, pump_thread, s, 0, &s->thread_id);
    if (!s->thread) {
        AprErr e = APR_ERR_LAST(L"CreateThread for the capture pump");
        IAudioClient_Stop(s->ac);
        CloseHandle(s->stop_ev);
        s->stop_ev = NULL;
        InterlockedExchange(&s->running, 0);
        return e;
    }
    return apr_ok();
}

void apr_wasapi_stop(AprWasapiStream *s)
{
    HANDLE th;

    InterlockedExchange(&s->running, 0);
    if (s->stop_ev) SetEvent(s->stop_ev);

    /* stop() is documented safe from any thread, and the tick callback runs on
     * the pump. Joining ourselves would deadlock, so the pump just unwinds. */
    if (GetCurrentThreadId() == s->thread_id) {
        if (s->ac) IAudioClient_Stop(s->ac);
        return;
    }

    /* Exactly one caller gets the handle, so two concurrent stops cannot both
     * join and close it. */
    th = InterlockedExchangePointer((PVOID volatile *)&s->thread, NULL);
    if (th) {
        if (WaitForSingleObject(th, 5000) == WAIT_OBJECT_0) {
            CloseHandle(th);
            s->thread_id = 0;
        } else {
            /* A pump whose longest wait is 200 ms and that has not exited in
             * 5 s is wedged inside WASAPI. Killing it mid-write would corrupt
             * the ring, so mark the stream unfreeable instead: leaking a COM
             * reference is survivable, a use-after-free under an audio thread
             * is not. */
            s->pump_stuck = 1;
            APR_ERROR(L"capture pump did not exit within 5 s; leaking the "
                      L"stream rather than freeing it under a live thread");
        }
    }

    if (s->ac) IAudioClient_Stop(s->ac);
}

void apr_wasapi_close(AprWasapiStream *s)
{
    apr_wasapi_stop(s);

    if (s->pump_stuck || s->thread) return;   /* see apr_wasapi_stop */

    if (s->cc)      { IAudioCaptureClient_Release(s->cc); s->cc = NULL; }
    if (s->ac)      { IAudioClient_Release(s->ac); s->ac = NULL; }
    if (s->ev)      { CloseHandle(s->ev); s->ev = NULL; }
    if (s->stop_ev) { CloseHandle(s->stop_ev); s->stop_ev = NULL; }
}
