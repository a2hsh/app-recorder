/*
 * capture_device.c -- a real capture endpoint (design section 4.2).
 *
 * This is the source kind that actually has a clock problem. A device rides a
 * hardware crystal that is not the machine QPC and not the audio engine, so
 * over a three-hour session it genuinely drifts, and it genuinely drops
 * samples. Design 5.1: process taps are the reference timeline, device
 * captures are what gets corrected onto it. Everything the shared pump does
 * with DATA_DISCONTINUITY and TIMESTAMP_ERROR exists for this file.
 *
 * The drift *correction* is not here. Capture measures (an anchor from
 * apr_qpc_now() at buffer arrival, plus an exact frame count) and fills real
 * gaps; the PI controller and the resampler live in core/ and are shared by
 * every path (design 5.2 step 5, and the DRY table in section 7).
 *
 * Format: unlike a process tap, a device has a mix format and will refuse one
 * it does not like. apprecorder still imposes the session format, because
 * capture.h promises every source delivers the same thing -- so on refusal the
 * client is re-activated with AUTOCONVERTPCM and the engine is asked to do the
 * conversion. That is not a second resampler in this codebase; it is the OS
 * one, and it does not hide drift, which still shows up as delivered frames
 * against elapsed QPC.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <stdlib.h>

#include "capture_internal.h"
#include "wasapi_common.h"
#include "log.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

typedef struct DevImpl {
    AprWasapiStream s;
    AprCapStatus    st;

    IMMDeviceEnumerator *enu;
    IMMDevice           *dev;

    /* Borrowed for the length of open() only, which is exactly how long the
     * endpoint id inside it is valid (capture.h). The COM half of open runs on
     * the capture thread while this call is still on the stack, so passing the
     * pointer across is safe -- and nothing keeps it afterwards. */
    const AprCaptureConfig *cfg;
} DevImpl;

/* Fetch a fresh IAudioClient. A failed Initialize leaves the old one unusable,
 * so the retry path needs a new one rather than another go at the same
 * pointer. */
static AprErr activate_client(DevImpl *d, IAudioClient **out)
{
    HRESULT hr = IMMDevice_Activate(d->dev, &apr_iid_IAudioClient, CLSCTX_ALL,
                                    NULL, (void **)out);
    if (FAILED(hr)) {
        *out = NULL;
        return APR_ERR_HR(hr, L"IMMDevice::Activate(IAudioClient)");
    }
    return apr_ok();
}

/* The half of open() that touches COM. Runs on the capture's own thread, in the
 * MTA it owns. This path did NOT need the MTA as badly as the process tap does
 * -- IMMDeviceEnumerator is happy in an STA -- but it demanded it all the same,
 * with the identical RPC_E_CHANGED_MODE refusal, so "Add Source > a microphone"
 * failed from the windowed front end for exactly the same reason. It is fixed
 * the same way rather than by relaxing the check, because the objects created
 * here are used by the pump and belong in the pump's apartment. */
static AprErr dev_open_com(void *user)
{
    DevImpl *d = (DevImpl *)user;
    const AprCaptureConfig *cfg = d->cfg;
    IAudioClient *ac = NULL;
    HRESULT hr;
    AprErr e;

    hr = CoCreateInstance(&apr_clsid_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &apr_iid_IMMDeviceEnumerator, (void **)&d->enu);
    if (FAILED(hr)) return APR_ERR_HR(hr, L"CoCreateInstance(MMDeviceEnumerator)");

    if (cfg->device.endpoint_id && cfg->device.endpoint_id[0]) {
        hr = IMMDeviceEnumerator_GetDevice(d->enu, cfg->device.endpoint_id,
                                           &d->dev);
        if (FAILED(hr))
            return APR_ERR_HR(hr, L"no capture endpoint with id %s",
                              cfg->device.endpoint_id);
    } else {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(d->enu, eCapture,
                                                         eConsole, &d->dev);
        if (FAILED(hr))
            return APR_ERR_HR(hr, L"no default capture endpoint");
    }

    e = activate_client(d, &ac);
    if (apr_failed(&e)) return e;

    e = apr_wasapi_prepare(&d->s, ac, 0);
    if (apr_failed(&e)) {
        /* AUDCLNT_E_UNSUPPORTED_FORMAT means the endpoint will not give us the
         * session rate or channel count directly. Ask the engine to convert
         * rather than changing what this source promises to deliver. */
        if (e.kind != APR_E_HRESULT ||
            (HRESULT)e.code != AUDCLNT_E_UNSUPPORTED_FORMAT)
            return e;

        APR_WARN(L"endpoint refused %u Hz / %u ch float32; retrying with "
                 L"AUTOCONVERTPCM", cfg->sample_rate, (unsigned)cfg->channels);

        ac = NULL;
        e = activate_client(d, &ac);
        if (apr_failed(&e)) return e;

        e = apr_wasapi_prepare(&d->s, ac,
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
        if (apr_failed(&e)) return e;
    }

    return apr_ok();
}

/* Callable from ANY apartment -- see capture.h and wasapi_common.h. */
static AprErr dev_open(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb)
{
    DevImpl *d;
    AprErr e;

    e = apr_capture_check_common(cfg, rb);
    if (apr_failed(&e)) return e;
    if (cfg->kind != APR_SRC_DEVICE)
        return APR_ERR(APR_E_INVALID_ARG, L"not a device source");

    d = (DevImpl *)calloc(1, sizeof(*d));
    if (!d) return APR_ERR(APR_E_NO_MEMORY, L"device capture");
    c->impl = d;
    apr_capstat_init(&d->st);
    d->cfg = cfg;

    /* Must precede the thread: it zeroes the stream, handles and all. */
    apr_wasapi_stream_init(&d->s, rb, &d->st, cfg->sample_rate, cfg->channels,
                           1 /* device_mode: real drift, real dropouts */,
                           NULL, NULL);
    /* AFTER stream_init, which zeroes the stream. A non-zero resume anchor
     * means this capture is replacing one whose endpoint was invalidated --
     * unplugged, or a driver restart -- and its first frame belongs at the
     * absolute index that anchor implies (capture.h). */
    apr_capresume_init(&d->s.resume, cfg);

    e = apr_wasapi_thread_start(&d->s);
    if (apr_failed(&e)) return e;

    e = apr_wasapi_call(&d->s, dev_open_com, d);
    d->cfg = NULL;   /* borrowed only for the length of this call */
    return e;
}

static AprErr dev_start(AprCapture *c)
{
    DevImpl *d = (DevImpl *)c->impl;
    if (!d) return APR_ERR(APR_E_STATE, L"device capture is not open");
    return apr_wasapi_start(&d->s);
}

static void dev_stop(AprCapture *c)
{
    DevImpl *d = (DevImpl *)c->impl;
    if (d) apr_wasapi_stop(&d->s);
}

static void dev_status(const AprCapture *c, AprCaptureStatus *out)
{
    const DevImpl *d = (const DevImpl *)c->impl;
    if (!out) return;
    if (!d) { ZeroMemory(out, sizeof(*out)); out->last_error = apr_ok(); return; }
    apr_capstat_read(&d->st, out);
}

/* The half of close() that touches COM: released on the capture thread, in the
 * apartment these objects were created in. */
static AprErr dev_close_com(void *user)
{
    DevImpl *d = (DevImpl *)user;
    if (d->dev) { IMMDevice_Release(d->dev); d->dev = NULL; }
    if (d->enu) { IMMDeviceEnumerator_Release(d->enu); d->enu = NULL; }
    return apr_ok();
}

static AprErr dev_close(AprCapture *c)
{
    DevImpl *d = (DevImpl *)c->impl;
    AprErr   e;

    if (!d) return apr_ok();

    e = apr_wasapi_close(&d->s, dev_close_com, d);
    if (apr_failed(&e)) {
        /* wasapi_common.h: the pump is still running on &d->s, which lives
         * inside this allocation and points at the caller's ring. c->impl is
         * deliberately LEFT SET so a caller that keeps the capture can retry;
         * the error is what stops the ring being freed above us. */
        return e;
    }

    c->impl = NULL;
    free(d);
    return apr_ok();
}

static const AprCaptureVTable g_device_vtable = {
    "device-capture",
    dev_open,
    dev_start,
    dev_stop,
    dev_status,
    dev_close
};

const AprCaptureVTable *apr_capture_device_vtable(void)
{
    return &g_device_vtable;
}
