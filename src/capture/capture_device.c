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
    int                  com_owned;
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

static AprErr dev_open(AprCapture *c, const AprCaptureConfig *cfg, RingBuf *rb)
{
    DevImpl *d;
    IAudioClient *ac = NULL;
    HRESULT hr_com, hr;
    AprErr e;

    e = apr_capture_check_common(cfg, rb);
    if (apr_failed(&e)) return e;
    if (cfg->kind != APR_SRC_DEVICE)
        return APR_ERR(APR_E_INVALID_ARG, L"not a device source");

    d = (DevImpl *)calloc(1, sizeof(*d));
    if (!d) return APR_ERR(APR_E_NO_MEMORY, L"device capture");
    c->impl = d;
    apr_capstat_init(&d->st);

    /* MTA, for the same reason as the process tap: the pump thread and the
     * opener must agree, and everything in this codebase is free-threaded.
     * open() and close() must run on the same thread. */
    hr_com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr_com == RPC_E_CHANGED_MODE)
        return APR_ERR(APR_E_STATE,
                       L"device capture requires the MTA; this thread is an STA");
    if (FAILED(hr_com)) return APR_ERR_HR(hr_com, L"CoInitializeEx(MTA)");
    d->com_owned = 1;

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

    apr_wasapi_stream_init(&d->s, rb, &d->st, cfg->sample_rate, cfg->channels,
                           1 /* device_mode: real drift, real dropouts */,
                           NULL, NULL);

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

static void dev_close(AprCapture *c)
{
    DevImpl *d = (DevImpl *)c->impl;
    if (!d) return;

    apr_wasapi_close(&d->s);

    if (d->dev) { IMMDevice_Release(d->dev); d->dev = NULL; }
    if (d->enu) { IMMDeviceEnumerator_Release(d->enu); d->enu = NULL; }
    if (d->com_owned) { CoUninitialize(); d->com_owned = 0; }

    c->impl = NULL;
    free(d);
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
