/*
 * spike_loopback.c -- feasibility probe for WASAPI *process loopback* in plain C.
 *
 * Build:   build.cmd Debug spikes
 * Run:     build\Debug\spike_loopback.exe <pid> [--seconds N] [--out FILE]
 *                                        [--wait-ms N] [--trace]
 *
 * What it proves / measures:
 *   1. That IActivateAudioInterfaceCompletionHandler -- a COM *callback*
 *      interface -- can be hand-vtabled in C.  See section "COM SHIM" below;
 *      that block is the part destined for src/capture/com_shim.c.
 *   2. That a process-loopback IAudioClient can be Initialize()d with a
 *      caller-supplied format (GetMixFormat is E_NOTIMPL -- probed and
 *      reported, not assumed).
 *   3. *** What actually happens when the captured app goes quiet. ***
 *      This is the measurement the drift-correction design hangs on.
 *
 * Everything is throwaway except the COM shim.  Diagnostics are verbose on
 * purpose: the numbers are the deliverable.
 */

/* NTDDI must be >= NTDDI_WIN10_FE (0x0A00000A) or audioclientactivationparams.h
 * compiles to nothing at all -- no AUDIOCLIENT_ACTIVATION_PARAMS, no
 * VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK.  This must precede every include. */
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  WINVER
#define WINVER 0x0A00
#undef  NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000A

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* GUIDs.  Defined locally rather than pulled from uuid.lib/ksmedia.h so the
 * spike has no link-order surprises and so the exact bytes are visible. */

static const GUID kIID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
/* IAgileObject: 94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90 */
static const GUID kIID_IAgileObject =
    { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90 } };
/* IActivateAudioInterfaceCompletionHandler: 41d949ab-9862-444a-80f6-c261334da5eb */
static const GUID kIID_ICompletionHandler =
    { 0x41d949ab, 0x9862, 0x444a, { 0x80,0xf6,0xc2,0x61,0x33,0x4d,0xa5,0xeb } };
/* IAudioClient: 1cb9ad4c-dbfa-4c32-b178-c2f568a703b2 */
static const GUID kIID_IAudioClient =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2 } };
/* IAudioCaptureClient: c8adbd64-e71e-48a0-a4de-185c395cd317 */
static const GUID kIID_IAudioCaptureClient =
    { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17 } };
/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: 00000003-0000-0010-8000-00aa00389b71 */
static const GUID kSubtypeIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } };

#define SPK_STEREO 0x3u   /* SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT (ksmedia.h) */

/* ================================================================== */
/* COM SHIM -- graduates verbatim into src/capture/com_shim.c          */
/* ================================================================== */

typedef struct ActivationHandler {
    /* MUST be first: makes &self layout-compatible with the interface ptr.
     * The SDK's C view of the interface is exactly { CONST_VTBL Vtbl *lpVtbl; },
     * so embedding it by value is the same thing as declaring our own lpVtbl,
     * but it type-checks against the SDK's macros. */
    IActivateAudioInterfaceCompletionHandler base;

    LONG     ref;
    HANDLE   done;          /* signalled from the callback thread            */
    HRESULT  hr_getresult;  /* HRESULT of GetActivateResult() itself         */
    HRESULT  hr_activate;   /* HRESULT the activation itself produced        */
    IUnknown *punk;         /* activated interface, AddRef'd by the callee   */
    DWORD    callback_tid;  /* proof it lands on another thread              */
} ActivationHandler;

static HRESULT STDMETHODCALLTYPE
AH_QueryInterface(IActivateAudioInterfaceCompletionHandler *This,
                  REFIID riid, void **ppv)
{
    if (ppv == NULL) return E_POINTER;
    if (IsEqualGUID(riid, &kIID_IUnknown) ||
        IsEqualGUID(riid, &kIID_ICompletionHandler) ||
        /* IAgileObject is not optional in practice: without it the activation
         * can be marshalled to another apartment and fail. */
        IsEqualGUID(riid, &kIID_IAgileObject))
    {
        *ppv = This;
        IActivateAudioInterfaceCompletionHandler_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
AH_AddRef(IActivateAudioInterfaceCompletionHandler *This)
{
    ActivationHandler *self = (ActivationHandler *)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE
AH_Release(IActivateAudioInterfaceCompletionHandler *This)
{
    ActivationHandler *self = (ActivationHandler *)This;
    LONG n = InterlockedDecrement(&self->ref);
    if (n == 0) {
        if (self->done) CloseHandle(self->done);
        free(self);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE
AH_ActivateCompleted(IActivateAudioInterfaceCompletionHandler *This,
                     IActivateAudioInterfaceAsyncOperation *op)
{
    ActivationHandler *self = (ActivationHandler *)This;
    self->callback_tid = GetCurrentThreadId();
    self->hr_getresult = IActivateAudioInterfaceAsyncOperation_GetActivateResult(
                             op, &self->hr_activate, &self->punk);
    SetEvent(self->done);
    return S_OK;   /* returning a failure here does NOT propagate anywhere useful */
}

static const IActivateAudioInterfaceCompletionHandlerVtbl g_ah_vtbl = {
    AH_QueryInterface,
    AH_AddRef,
    AH_Release,
    AH_ActivateCompleted
};

static ActivationHandler *ah_create(void)
{
    ActivationHandler *self = (ActivationHandler *)calloc(1, sizeof(*self));
    if (!self) return NULL;
    /* CONST_VTBL is empty in C, so lpVtbl is non-const; cast is required. */
    self->base.lpVtbl = (IActivateAudioInterfaceCompletionHandlerVtbl *)&g_ah_vtbl;
    self->ref  = 1;
    self->done = CreateEventW(NULL, TRUE /*manual reset*/, FALSE, NULL);
    if (!self->done) { free(self); return NULL; }
    return self;
}

/* Open a process-loopback IAudioClient.  Blocks until the async activation
 * completes.  Caller must be in the MTA. */
static HRESULT open_process_loopback_client(DWORD pid, BOOL include_tree,
                                            DWORD timeout_ms,
                                            IAudioClient **out_client,
                                            DWORD *out_callback_tid)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params;
    PROPVARIANT pv;
    ActivationHandler *h;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    HRESULT hr;

    *out_client = NULL;

    h = ah_create();
    if (!h) return E_OUTOFMEMORY;

    ZeroMemory(&params, sizeof(params));
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = include_tree
        ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PropVariantInit(&pv);
    pv.vt              = VT_BLOB;
    pv.blob.cbSize     = (ULONG)sizeof(params);
    pv.blob.pBlobData  = (BYTE *)&params;

    hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                     &kIID_IAudioClient, &pv,
                                     (IActivateAudioInterfaceCompletionHandler *)h,
                                     &op);
    if (FAILED(hr)) {
        IActivateAudioInterfaceCompletionHandler_Release((IActivateAudioInterfaceCompletionHandler *)h);
        return hr;
    }

    if (WaitForSingleObject(h->done, timeout_ms) != WAIT_OBJECT_0) {
        if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
        IActivateAudioInterfaceCompletionHandler_Release((IActivateAudioInterfaceCompletionHandler *)h);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    *out_callback_tid = h->callback_tid;

    hr = h->hr_getresult;
    if (SUCCEEDED(hr)) hr = h->hr_activate;
    if (SUCCEEDED(hr) && h->punk) {
        hr = IUnknown_QueryInterface(h->punk, &kIID_IAudioClient, (void **)out_client);
    } else if (SUCCEEDED(hr)) {
        hr = E_NOINTERFACE;
    }

    if (h->punk) IUnknown_Release(h->punk);
    if (op)      IActivateAudioInterfaceAsyncOperation_Release(op);
    IActivateAudioInterfaceCompletionHandler_Release((IActivateAudioInterfaceCompletionHandler *)h);
    return hr;
}

/* ================================================================== */
/* end COM SHIM                                                       */
/* ================================================================== */

#define SAMPLE_RATE   48000u
#define CHANNELS      2u
#define BYTES_PER_FR  (CHANNELS * 4u)

/* --------------------------- WAV writer --------------------------- */

typedef struct WavWriter {
    FILE  *f;
    UINT64 data_bytes;
} WavWriter;

static int wav_open(WavWriter *w, const wchar_t *path)
{
    unsigned char hdr[44];
    unsigned char *p = hdr;
    UINT32 u32; UINT16 u16;

    w->f = _wfopen(path, L"wb");
    if (!w->f) return 0;
    w->data_bytes = 0;

#define PUT32(v) do { u32 = (UINT32)(v); memcpy(p, &u32, 4); p += 4; } while (0)
#define PUT16(v) do { u16 = (UINT16)(v); memcpy(p, &u16, 2); p += 2; } while (0)
    memcpy(p, "RIFF", 4); p += 4;  PUT32(0);
    memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4;  PUT32(16);
    PUT16(3 /*WAVE_FORMAT_IEEE_FLOAT*/);
    PUT16(CHANNELS);
    PUT32(SAMPLE_RATE);
    PUT32(SAMPLE_RATE * BYTES_PER_FR);
    PUT16(BYTES_PER_FR);
    PUT16(32);
    memcpy(p, "data", 4); p += 4;  PUT32(0);
#undef PUT32
#undef PUT16
    fwrite(hdr, 1, sizeof(hdr), w->f);
    return 1;
}

static void wav_write(WavWriter *w, const void *pcm, size_t bytes)
{
    if (!w->f || bytes == 0) return;
    fwrite(pcm, 1, bytes, w->f);
    w->data_bytes += bytes;
}

static void wav_write_silence(WavWriter *w, UINT32 frames)
{
    static const float zero[512 * CHANNELS] = { 0 };
    UINT32 left = frames;
    while (left) {
        UINT32 chunk = left > 512 ? 512 : left;
        wav_write(w, zero, (size_t)chunk * BYTES_PER_FR);
        left -= chunk;
    }
}

static void wav_close(WavWriter *w)
{
    UINT32 v;
    if (!w->f) return;
    v = (UINT32)(w->data_bytes + 36);
    fseek(w->f, 4, SEEK_SET);  fwrite(&v, 4, 1, w->f);
    v = (UINT32)w->data_bytes;
    fseek(w->f, 40, SEEK_SET); fwrite(&v, 4, 1, w->f);
    fclose(w->f);
    w->f = NULL;
}

/* ------------------------- diagnostics ---------------------------- */

#define MAX_EVENTS 200000

typedef struct PacketRec {
    double  t_ms;          /* wall clock ms since capture start           */
    UINT32  frames;
    DWORD   flags;
    UINT64  qpc;           /* pu64QPCPosition, raw                        */
    UINT64  devpos;        /* pu64DevicePosition, raw                     */
    float   peak;
} PacketRec;

static PacketRec  g_pkt[MAX_EVENTS];
static size_t     g_pkt_n;
static size_t     g_allzero_pkts;

static LARGE_INTEGER g_qpf;

static double qpc_to_ms(UINT64 ticks) { return (double)ticks * 1000.0 / (double)g_qpf.QuadPart; }
static UINT64 qpc_now(void) { LARGE_INTEGER li; QueryPerformanceCounter(&li); return (UINT64)li.QuadPart; }

static const char *flag_str(DWORD f, char *buf, size_t n)
{
    buf[0] = 0;
    if (f & AUDCLNT_BUFFERFLAGS_SILENT)               strncat_s(buf, n, "SILENT ", _TRUNCATE);
    if (f & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)   strncat_s(buf, n, "DISCONT ", _TRUNCATE);
    if (f & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)      strncat_s(buf, n, "TSERR ", _TRUNCATE);
    if (buf[0] == 0) strncat_s(buf, n, "-", _TRUNCATE);
    return buf;
}

static void proc_name(DWORD pid, wchar_t *out, DWORD cch)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    DWORD n = cch;
    out[0] = 0;
    if (p) {
        if (!QueryFullProcessImageNameW(p, 0, out, &n)) out[0] = 0;
        CloseHandle(p);
    }
    if (out[0] == 0) wcscpy_s(out, cch, L"<unknown>");
}

/* ------------------------------ main ------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    DWORD pid = 0;
    double seconds = 10.0;
    DWORD wait_ms = 100;
    UINT32 bucket_ms = 250;
    BOOL trace = FALSE;
    BOOL exclude = FALSE;
    wchar_t out_path[MAX_PATH];
    wchar_t exe[MAX_PATH];

    HRESULT hr;
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    HANDLE hev = NULL;
    WAVEFORMATEXTENSIBLE wfx;
    WAVEFORMATEX *probe_fmt = NULL;
    WavWriter wav = { 0 };
    DWORD cb_tid = 0;
    HANDLE mmcss = NULL;
    DWORD mmcss_task = 0;

    UINT64 t0, t_end, qpc_at_start_100ns;
    UINT32 bufsize = 0;
    REFERENCE_TIME defp = 0, minp = 0;

    size_t waits = 0, timeouts = 0, wake_empty = 0;
    UINT64 total_frames = 0, silent_frames = 0;
    size_t n_silent_pkts = 0, n_discont = 0, n_tserr = 0;
    int i;

    wcscpy_s(out_path, MAX_PATH, L"spike_loopback.wav");

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--seconds") == 0 && i + 1 < argc)      seconds = _wtof(argv[++i]);
        else if (_wcsicmp(argv[i], L"--out") == 0 && i + 1 < argc)     wcscpy_s(out_path, MAX_PATH, argv[++i]);
        else if (_wcsicmp(argv[i], L"--wait-ms") == 0 && i + 1 < argc) wait_ms = (DWORD)_wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--bucket-ms") == 0 && i + 1 < argc) bucket_ms = (UINT32)_wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--trace") == 0)                   trace = TRUE;
        else if (_wcsicmp(argv[i], L"--exclude") == 0)                 exclude = TRUE;
        else if (pid == 0)                                             pid = (DWORD)_wtoi(argv[i]);
    }

    if (pid == 0) {
        wprintf(L"usage: spike_loopback <pid> [--seconds N] [--out FILE] "
                L"[--wait-ms N] [--trace] [--exclude]\n");
        return 2;
    }

    QueryPerformanceFrequency(&g_qpf);
    proc_name(pid, exe, MAX_PATH);

    wprintf(L"=== spike_loopback ===\n");
    wprintf(L"pid            : %lu  (%s)\n", pid, exe);
    wprintf(L"mode           : %s\n", exclude ? L"EXCLUDE_TARGET_PROCESS_TREE"
                                              : L"INCLUDE_TARGET_PROCESS_TREE");
    wprintf(L"duration       : %.1f s\n", seconds);
    wprintf(L"out            : %s\n", out_path);
    wprintf(L"QPF            : %lld Hz\n", (long long)g_qpf.QuadPart);
    wprintf(L"main thread id : %lu\n", GetCurrentThreadId());

    /* MTA is mandatory.  Under an STA the activation callback deadlocks/marshal-
     * fails and IAgileObject alone will not save you. */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed hr=0x%08lX\n", (unsigned long)hr); return 1; }

    hr = open_process_loopback_client(pid, !exclude, 5000, &ac, &cb_tid);
    wprintf(L"ActivateAudioInterfaceAsync -> hr=0x%08lX  (callback thread id %lu)\n",
            (unsigned long)hr, cb_tid);
    if (FAILED(hr)) goto done;

    /* Probe, purely to document the failure the design predicts. */
    hr = IAudioClient_GetMixFormat(ac, &probe_fmt);
    wprintf(L"GetMixFormat (probe) -> hr=0x%08lX%s\n", (unsigned long)hr,
            hr == E_NOTIMPL ? L"  (E_NOTIMPL, as designed for)" : L"");
    if (SUCCEEDED(hr) && probe_fmt) {
        wprintf(L"   ... it actually returned %u Hz / %u ch / %u bits\n",
                probe_fmt->nSamplesPerSec, probe_fmt->nChannels, probe_fmt->wBitsPerSample);
        CoTaskMemFree(probe_fmt);
        probe_fmt = NULL;
    }

    hr = IAudioClient_GetDevicePeriod(ac, &defp, &minp);
    wprintf(L"GetDevicePeriod (probe) -> hr=0x%08lX  default=%lld  min=%lld (100ns)\n",
            (unsigned long)hr, (long long)defp, (long long)minp);

    /* Supply the format; do not negotiate. */
    ZeroMemory(&wfx, sizeof(wfx));
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = (WORD)CHANNELS;
    wfx.Format.nSamplesPerSec  = SAMPLE_RATE;
    wfx.Format.wBitsPerSample  = 32;
    wfx.Format.nBlockAlign     = (WORD)BYTES_PER_FR;
    wfx.Format.nAvgBytesPerSec = SAMPLE_RATE * BYTES_PER_FR;
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask          = SPK_STEREO;
    wfx.SubFormat              = kSubtypeIeeeFloat;

    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK |
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 0 /*hnsBufferDuration*/, 0 /*hnsPeriodicity*/,
                                 &wfx.Format, NULL);
    wprintf(L"Initialize -> hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) goto done;

    hev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_SetEventHandle(ac, hev);
    wprintf(L"SetEventHandle -> hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) goto done;

    hr = IAudioClient_GetBufferSize(ac, &bufsize);
    wprintf(L"GetBufferSize -> hr=0x%08lX  %u frames (%.2f ms)\n",
            (unsigned long)hr, bufsize, bufsize * 1000.0 / SAMPLE_RATE);

    hr = IAudioClient_GetService(ac, &kIID_IAudioCaptureClient, (void **)&cc);
    wprintf(L"GetService(IAudioCaptureClient) -> hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) goto done;

    if (!wav_open(&wav, out_path)) { wprintf(L"cannot open %s\n", out_path); goto done; }

    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);

    hr = IAudioClient_Start(ac);
    wprintf(L"Start -> hr=0x%08lX\n\n", (unsigned long)hr);
    if (FAILED(hr)) goto done;

    t0 = qpc_now();
    qpc_at_start_100ns = (UINT64)((double)t0 * 10000000.0 / (double)g_qpf.QuadPart);
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wprintf(L"Start wall clock: %02u:%02u:%02u.%03u (local)\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    }
    wprintf(L"QPC at Start: raw=%llu  -> %llu (100ns units)\n\n",
            (unsigned long long)t0, (unsigned long long)qpc_at_start_100ns);

    t_end = t0 + (UINT64)(seconds * (double)g_qpf.QuadPart);

    while (qpc_now() < t_end) {
        DWORD w = WaitForSingleObject(hev, wait_ms);
        UINT32 packet = 0;
        BOOL got_any = FALSE;

        waits++;
        if (w == WAIT_TIMEOUT) { timeouts++; }

        hr = IAudioCaptureClient_GetNextPacketSize(cc, &packet);
        if (FAILED(hr)) { wprintf(L"GetNextPacketSize -> 0x%08lX\n", (unsigned long)hr); break; }

        while (packet > 0) {
            BYTE *data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devpos = 0, qpcpos = 0;

            hr = IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, &devpos, &qpcpos);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) { wprintf(L"GetBuffer -> 0x%08lX\n", (unsigned long)hr); goto stoploop; }

            got_any = TRUE;
            {
                float peak = 0.0f;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    const float *s = (const float *)data;
                    UINT32 k, n = frames * CHANNELS;
                    for (k = 0; k < n; k++) {
                        float a = s[k] < 0 ? -s[k] : s[k];
                        if (a > peak) peak = a;
                    }
                    wav_write(&wav, data, (size_t)frames * BYTES_PER_FR);
                    if (peak == 0.0f) g_allzero_pkts++;
                } else {
                    /* SILENT means pData is undefined -- must not be copied. */
                    wav_write_silence(&wav, frames);
                    silent_frames += frames;
                    n_silent_pkts++;
                }
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) n_discont++;
                if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)    n_tserr++;

                if (g_pkt_n < MAX_EVENTS) {
                    PacketRec *r = &g_pkt[g_pkt_n++];
                    r->t_ms   = qpc_to_ms(qpc_now() - t0);
                    r->frames = frames;
                    r->flags  = flags;
                    r->qpc    = qpcpos;
                    r->devpos = devpos;
                    r->peak   = peak;
                }
                total_frames += frames;
                if (trace) {
                    char fb[64];
                    printf("  [%8.2f ms] frames=%-6u flags=%-18s qpc=%llu dev=%llu peak=%.5f\n",
                           qpc_to_ms(qpc_now() - t0), frames, flag_str(flags, fb, sizeof fb),
                           (unsigned long long)qpcpos, (unsigned long long)devpos, peak);
                }
            }

            hr = IAudioCaptureClient_ReleaseBuffer(cc, frames);
            if (FAILED(hr)) { wprintf(L"ReleaseBuffer -> 0x%08lX\n", (unsigned long)hr); goto stoploop; }

            hr = IAudioCaptureClient_GetNextPacketSize(cc, &packet);
            if (FAILED(hr)) goto stoploop;
        }

        if (w == WAIT_OBJECT_0 && !got_any) wake_empty++;
    }
stoploop:

    IAudioClient_Stop(ac);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    wav_close(&wav);

    /* ------------------------- report ------------------------- */
    {
        double wall_ms = qpc_to_ms(qpc_now() - t0);
        double audio_ms = (double)total_frames * 1000.0 / SAMPLE_RATE;

        wprintf(L"\n--- summary ------------------------------------------------\n");
        wprintf(L"wall clock captured : %.1f ms\n", wall_ms);
        wprintf(L"frames delivered    : %llu  (%.1f ms of audio)\n",
                (unsigned long long)total_frames, audio_ms);
        wprintf(L"MISSING vs wall     : %.1f ms   <<< silence that must be synthesized\n",
                wall_ms - audio_ms);
        wprintf(L"packets             : %zu\n", g_pkt_n);
        wprintf(L"wait() calls        : %zu   (timeouts: %zu, woke-with-no-data: %zu)\n",
                waits, timeouts, wake_empty);
        wprintf(L"SILENT packets      : %zu  (%llu frames)\n",
                n_silent_pkts, (unsigned long long)silent_frames);
        wprintf(L"DATA_DISCONTINUITY  : %zu\n", n_discont);
        wprintf(L"TIMESTAMP_ERROR     : %zu\n", n_tserr);
        wprintf(L"wav data bytes      : %llu\n", (unsigned long long)wav.data_bytes);
    }

    /* QPC unit check: is pu64QPCPosition really 100ns and on the same base as
     * QueryPerformanceCounter? */
    if (g_pkt_n >= 2) {
        UINT64 now_raw = qpc_now();
        double now_100ns = (double)now_raw * 10000000.0 / (double)g_qpf.QuadPart;
        PacketRec *a = &g_pkt[0], *z = &g_pkt[g_pkt_n - 1];
        double dq_100ns = (double)(z->qpc - a->qpc);
        double dwall_ms = z->t_ms - a->t_ms;

        wprintf(L"\n--- QPC / timestamp check ----------------------------------\n");
        wprintf(L"first pkt pu64QPCPosition : %llu\n", (unsigned long long)a->qpc);
        wprintf(L"last  pkt pu64QPCPosition : %llu\n", (unsigned long long)z->qpc);
        wprintf(L"QueryPerformanceCounter now, scaled to 100ns: %.0f\n", now_100ns);
        wprintf(L"delta(last-first) = %.0f (100ns) = %.1f ms;  wall delta = %.1f ms\n",
                dq_100ns, dq_100ns / 10000.0, dwall_ms);
        wprintf(L"last pu64QPCPosition vs QPC-now: %.1f ms behind\n",
                (now_100ns - (double)z->qpc) / 10000.0);
        wprintf(L"device position delta     : %llu frames (%.1f ms)\n",
                (unsigned long long)(z->devpos - a->devpos),
                (double)(z->devpos - a->devpos) * 1000.0 / SAMPLE_RATE);
    }

    /* Gap analysis: this is the silence-behaviour evidence. */
    if (g_pkt_n >= 2) {
        size_t k, gaps = 0;
        double biggest = 0.0;
        double total_gap_ms = 0.0;
        wprintf(L"\n--- gaps (packet QPC start vs previous packet QPC end) -----\n");
        for (k = 1; k < g_pkt_n; k++) {
            double prev_end = (double)g_pkt[k - 1].qpc
                            + (double)g_pkt[k - 1].frames * 10000000.0 / SAMPLE_RATE;
            double gap_ms = ((double)g_pkt[k].qpc - prev_end) / 10000.0;
            if (gap_ms > 5.0) {
                gaps++;
                total_gap_ms += gap_ms;
                if (gap_ms > biggest) biggest = gap_ms;
                if (gaps <= 40)
                    wprintf(L"  gap %.1f ms  before packet %zu at t=%.1f ms "
                            L"(prev flags 0x%lX, this flags 0x%lX)\n",
                            gap_ms, k, g_pkt[k].t_ms,
                            (unsigned long)g_pkt[k - 1].flags, (unsigned long)g_pkt[k].flags);
            }
        }
        wprintf(L"gaps > 5 ms: %zu   total %.1f ms   biggest %.1f ms\n",
                gaps, total_gap_ms, biggest);
    }

    /* Bucketed table -- makes "app went quiet at t=X" legible.
     * "allzero" counts packets whose PCM was entirely 0.0 but which were NOT
     * flagged SILENT: real buffers carrying digital silence. */
    if (g_pkt_n) {
        int b;
        int nb = (int)ceil(seconds * 1000.0 / bucket_ms);
        wprintf(L"\n--- per %u ms ----------------------------------------------\n", bucket_ms);
        wprintf(L"   t_ms | packets | frames | ms audio | fill%% | silentfl | allzero | peak\n");
        for (b = 0; b < nb; b++) {
            double lo = b * (double)bucket_ms, hi = lo + bucket_ms;
            size_t k, pk = 0, az = 0; UINT64 fr = 0, sf = 0; float peak = 0.0f;
            for (k = 0; k < g_pkt_n; k++) {
                if (g_pkt[k].t_ms >= lo && g_pkt[k].t_ms < hi) {
                    pk++; fr += g_pkt[k].frames;
                    if (g_pkt[k].flags & AUDCLNT_BUFFERFLAGS_SILENT) sf += g_pkt[k].frames;
                    else if (g_pkt[k].peak == 0.0f) az++;
                    if (g_pkt[k].peak > peak) peak = g_pkt[k].peak;
                }
            }
            wprintf(L"%7.0f | %7zu | %6llu | %8.1f | %5.1f | %8llu | %7zu | %.5f\n",
                    lo, pk, (unsigned long long)fr,
                    (double)fr * 1000.0 / SAMPLE_RATE,
                    (double)fr * 1000.0 / SAMPLE_RATE / bucket_ms * 100.0,
                    (unsigned long long)sf, az, peak);
        }
    }

done:
    if (cc) IAudioCaptureClient_Release(cc);
    if (ac) IAudioClient_Release(ac);
    if (hev) CloseHandle(hev);
    if (wav.f) wav_close(&wav);
    CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
