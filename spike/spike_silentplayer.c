/*
 * spike_silentplayer.c -- a *deliberately inaudible* WASAPI render process,
 * built solely so that process-loopback capture has something to capture.
 *
 * READ AGENTS.md RULE #1 BEFORE TOUCHING THIS FILE.
 *
 * The author of this project is blind and works by listening.  A previous agent
 * looped a tone through his default output device and could not stop it.  This
 * program exists to make that class of accident structurally impossible:
 *
 *   SAFETY PROPERTY 1 -- inaudible by construction.
 *       ISimpleAudioVolume::SetMasterVolume(v, NULL) is called on this
 *       process's own audio session BEFORE IAudioClient::Start(), and then
 *       READ BACK.  If the read-back is not exactly the requested value, or the
 *       requested value exceeds MAX_SAFE_VOLUME, the program exits WITHOUT ever
 *       calling Start().  Fail-closed: any doubt means no audio.
 *       Default volume is 0.0f, i.e. multiply-by-zero, i.e. digital silence at
 *       the endpoint.
 *
 *   SAFETY PROPERTY 2 -- finite by construction.
 *       There is no playback loop.  A fixed, precomputed total frame count is
 *       rendered once and the stream then stops.  The render pump is bounded
 *       *twice*: by frames_done < total_frames AND by an absolute QPC deadline.
 *
 *   SAFETY PROPERTY 3 -- dies on its own, with no stop path to fail.
 *       A watchdog thread started before any audio object is created sleeps for
 *       a hard cap and then TerminateProcess()es this process.  It shares no
 *       state with the render path, so no render-side bug can disable it.
 *       Nothing outside this process ever needs to call stop.
 *
 * Timeline (what it is FOR -- design section 5 / spike Part C):
 *       phase 1  tone       -- app rendering non-silent audio
 *       phase 2  zeros      -- app rendering, but digitally silent
 *       phase 3  tone       -- app audible again
 *       phase 4  stopped    -- stream Stop()ped, process still alive
 *   so a concurrent process-loopback capture can observe all three regimes and
 *   we can answer: does a quiet app produce no buffers, or SILENT-flagged ones?
 *
 * Build: build.cmd Debug spikes
 * Run:   spike_silentplayer.exe [--volume V] [--start-delay-ms N] [--trace]
 */

#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  WINVER
#define WINVER 0x0A00
#undef  NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000A

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* HARD SAFETY CEILING.                                               */
/*                                                                    */
/* Nothing this program can be asked to do may put more than this much */
/* gain on the session.  0.001 linear is -60 dB; combined with the     */
/* 0.25 source amplitude that is -72 dBFS at the endpoint, which is    */
/* below the dither floor of 16-bit audio.  The DEFAULT is 0.0.        */
/* Raising this constant is a safety change, not a tuning change.      */
/* ------------------------------------------------------------------ */
#define MAX_SAFE_VOLUME   0.001f
#define TONE_AMPLITUDE    0.25f
#define TONE_HZ           440.0

/* Phase plan, seconds. */
#define PH1_TONE_S    3.0
#define PH2_ZEROS_S   3.0
#define PH3_TONE_S    3.0
#define PH4_QUIET_S   4.0     /* stream stopped, process alive */

/* GUIDs defined locally: no link-order surprises, exact bytes visible. */
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6 } };
static const GUID kIID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2 } };
static const GUID kIID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2 } };
static const GUID kIID_ISimpleAudioVolume =
    { 0x87CE5498, 0x68D6, 0x44E5, { 0x92,0x15,0x6D,0xA4,0x7E,0xF8,0x83,0xD8 } };
static const GUID kIID_IAudioClock =
    { 0xCD63314F, 0x3FBA, 0x4A1B, { 0x81,0x2C,0xEF,0x96,0x35,0x87,0x28,0xE7 } };
static const GUID kSubtypeIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 } };
static const GUID kSubtypePcm =
    { 0x00000001, 0x0000, 0x0010, { 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 } };

/* ------------------------------------------------------------------ */
/* SAFETY PROPERTY 3: watchdog.                                        */
/* Shares no state with anything.  Cannot be cancelled.  Cannot be     */
/* starved by the render thread (it only sleeps).                      */
/* ------------------------------------------------------------------ */
static DWORD WINAPI watchdog_thread(LPVOID param)
{
    DWORD cap_ms = (DWORD)(ULONG_PTR)param;
    Sleep(cap_ms);
    /* If control reaches here the orderly path overran its budget.  Do not
     * negotiate, do not try to Stop() anything that may itself be wedged. */
    fwprintf(stderr, L"[watchdog] hard cap %lu ms reached -- terminating self\n",
             cap_ms);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

static LARGE_INTEGER g_qpf;
static UINT64 qpc_now(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (UINT64)li.QuadPart;
}

typedef enum { FMT_F32, FMT_S16 } SampleFmt;

int wmain(int argc, wchar_t **argv)
{
    float   want_vol = 0.0f;          /* DEFAULT: absolute digital silence */
    DWORD   start_delay_ms = 1500;
    BOOL    trace = FALSE;
    int     i;

    HRESULT hr = S_OK;
    IMMDeviceEnumerator *enu = NULL;
    IMMDevice           *dev = NULL;
    IAudioClient        *ac  = NULL;
    IAudioRenderClient  *rc  = NULL;
    ISimpleAudioVolume  *vol = NULL;
    IAudioClock         *clk = NULL;
    WAVEFORMATEX        *mix = NULL;

    UINT32 bufFrames = 0, sr = 0, ch = 0;
    SampleFmt sfmt = FMT_F32;
    UINT64 total_frames = 0, frames_done = 0;
    UINT64 ph1_end = 0, ph2_end = 0, ph3_end = 0;
    UINT64 deadline = 0;
    DWORD  cap_ms;
    double phase = 0.0, phase_inc = 0.0;
    float  readback = -1.0f;
    UINT64 clock_pos = 0, clock_freq = 0;
    UINT32 zero_writes = 0, tone_writes = 0;
    BOOL   started = FALSE;

    QueryPerformanceFrequency(&g_qpf);

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--volume") == 0 && i + 1 < argc)
            want_vol = (float)_wtof(argv[++i]);
        else if (_wcsicmp(argv[i], L"--start-delay-ms") == 0 && i + 1 < argc)
            start_delay_ms = (DWORD)_wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--trace") == 0)
            trace = TRUE;
    }

    wprintf(L"=== spike_silentplayer ===\n");
    wprintf(L"PID=%lu\n", GetCurrentProcessId());
    wprintf(L"requested session volume : %.6f  (ceiling %.6f, default 0.0)\n",
            (double)want_vol, (double)MAX_SAFE_VOLUME);
    fflush(stdout);

    /* --- SAFETY GATE 1: refuse anything above the ceiling, before any COM. -- */
    if (!(want_vol >= 0.0f) || want_vol > MAX_SAFE_VOLUME) {
        wprintf(L"REFUSED: volume %.6f exceeds MAX_SAFE_VOLUME %.6f (or is NaN). "
                L"No audio object will be created.\n",
                (double)want_vol, (double)MAX_SAFE_VOLUME);
        return 2;
    }

    /* --- SAFETY PROPERTY 3: watchdog armed before anything else can fail. --- */
    cap_ms = start_delay_ms + (DWORD)((PH1_TONE_S + PH2_ZEROS_S + PH3_TONE_S +
                                       PH4_QUIET_S) * 1000.0) + 8000;
    {
        HANDLE wd = CreateThread(NULL, 0, watchdog_thread,
                                 (LPVOID)(ULONG_PTR)cap_ms, 0, NULL);
        if (!wd) {
            wprintf(L"REFUSED: cannot create watchdog thread; no audio without it.\n");
            return 2;
        }
        CloseHandle(wd);
        wprintf(L"watchdog armed          : %lu ms hard cap (self-terminate)\n", cap_ms);
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx 0x%08lX\n", (unsigned long)hr); return 1; }

    hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &kIID_IMMDeviceEnumerator, (void **)&enu);
    if (FAILED(hr)) { wprintf(L"CoCreateInstance 0x%08lX\n", (unsigned long)hr); goto done; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enu, eRender, eConsole, &dev);
    if (FAILED(hr)) { wprintf(L"GetDefaultAudioEndpoint 0x%08lX\n", (unsigned long)hr); goto done; }

    hr = IMMDevice_Activate(dev, &kIID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { wprintf(L"Activate(IAudioClient) 0x%08lX\n", (unsigned long)hr); goto done; }

    hr = IAudioClient_GetMixFormat(ac, &mix);
    if (FAILED(hr) || !mix) { wprintf(L"GetMixFormat 0x%08lX\n", (unsigned long)hr); goto done; }

    sr = mix->nSamplesPerSec;
    ch = mix->nChannels;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE *we = (WAVEFORMATEXTENSIBLE *)mix;
        if (IsEqualGUID(&we->SubFormat, &kSubtypeIeeeFloat) && mix->wBitsPerSample == 32)
            sfmt = FMT_F32;
        else if (IsEqualGUID(&we->SubFormat, &kSubtypePcm) && mix->wBitsPerSample == 16)
            sfmt = FMT_S16;
        else { wprintf(L"unsupported mix subformat (%u bits)\n", mix->wBitsPerSample); goto done; }
    } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 32) {
        sfmt = FMT_F32;
    } else if (mix->wFormatTag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16) {
        sfmt = FMT_S16;
    } else {
        wprintf(L"unsupported mix format tag %u\n", mix->wFormatTag);
        goto done;
    }

    wprintf(L"endpoint mix format     : %u Hz / %u ch / %u bits (%s)\n",
            sr, ch, mix->wBitsPerSample, sfmt == FMT_F32 ? L"float32" : L"int16");

    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0,
                                 20000000 /* 2 s buffer, 100ns */, 0, mix, NULL);
    if (FAILED(hr)) { wprintf(L"Initialize 0x%08lX\n", (unsigned long)hr); goto done; }

    /* ------------------------------------------------------------------ */
    /* SAFETY PROPERTY 1: set the session volume and PROVE it took, before */
    /* a single frame can be rendered.  Start() is below this gate.        */
    /* ------------------------------------------------------------------ */
    hr = IAudioClient_GetService(ac, &kIID_ISimpleAudioVolume, (void **)&vol);
    if (FAILED(hr) || !vol) {
        wprintf(L"REFUSED: GetService(ISimpleAudioVolume) 0x%08lX -- cannot guarantee "
                L"inaudibility, so nothing will be rendered.\n", (unsigned long)hr);
        goto done;
    }
    hr = ISimpleAudioVolume_SetMasterVolume(vol, want_vol, NULL);
    if (FAILED(hr)) {
        wprintf(L"REFUSED: SetMasterVolume 0x%08lX -- nothing will be rendered.\n",
                (unsigned long)hr);
        goto done;
    }
    hr = ISimpleAudioVolume_GetMasterVolume(vol, &readback);
    if (FAILED(hr)) {
        wprintf(L"REFUSED: GetMasterVolume 0x%08lX -- cannot verify; nothing rendered.\n",
                (unsigned long)hr);
        goto done;
    }
    wprintf(L"session volume readback : %.9f\n", (double)readback);
    if (readback != want_vol || readback > MAX_SAFE_VOLUME) {
        wprintf(L"REFUSED: read-back %.9f != requested %.9f. Nothing will be rendered.\n",
                (double)readback, (double)want_vol);
        hr = E_FAIL;
        goto done;
    }
    {
        BOOL muted = FALSE;
        if (SUCCEEDED(ISimpleAudioVolume_GetMute(vol, &muted)))
            wprintf(L"session mute flag       : %s (informational; not used as the guard)\n",
                    muted ? L"TRUE" : L"FALSE");
    }
    wprintf(L"VOLUME GATE PASSED -- endpoint output is amplitude*%.9f\n",
            (double)readback);

    hr = IAudioClient_GetService(ac, &kIID_IAudioRenderClient, (void **)&rc);
    if (FAILED(hr)) { wprintf(L"GetService(render) 0x%08lX\n", (unsigned long)hr); goto done; }
    (void)IAudioClient_GetService(ac, &kIID_IAudioClock, (void **)&clk);

    hr = IAudioClient_GetBufferSize(ac, &bufFrames);
    if (FAILED(hr)) { wprintf(L"GetBufferSize 0x%08lX\n", (unsigned long)hr); goto done; }
    wprintf(L"render buffer           : %u frames (%.0f ms)\n",
            bufFrames, bufFrames * 1000.0 / sr);

    ph1_end = (UINT64)(PH1_TONE_S * sr);
    ph2_end = ph1_end + (UINT64)(PH2_ZEROS_S * sr);
    ph3_end = ph2_end + (UINT64)(PH3_TONE_S * sr);
    total_frames = ph3_end;
    phase_inc = 2.0 * 3.14159265358979323846 * TONE_HZ / (double)sr;

    wprintf(L"plan                    : %.0fs tone / %.0fs ZEROS / %.0fs tone / "
            L"%.0fs stream-stopped\n", PH1_TONE_S, PH2_ZEROS_S, PH3_TONE_S, PH4_QUIET_S);
    wprintf(L"start delay             : %lu ms\n", start_delay_ms);
    fflush(stdout);

    Sleep(start_delay_ms);

    /* Absolute wall deadline -- second, independent bound on the pump. */
    deadline = qpc_now() + (UINT64)((PH1_TONE_S + PH2_ZEROS_S + PH3_TONE_S + 4.0)
                                    * (double)g_qpf.QuadPart);

    hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { wprintf(L"Start 0x%08lX\n", (unsigned long)hr); goto done; }
    started = TRUE;
    wprintf(L"RENDER START qpc=%llu\n", (unsigned long long)qpc_now());
    fflush(stdout);

    /* SAFETY PROPERTY 2: bounded twice.  Not a playback loop -- a pump over a
     * finite, precomputed frame budget that can only shrink. */
    while (frames_done < total_frames && qpc_now() < deadline) {
        UINT32 pad = 0, avail, want;
        BYTE *data = NULL;
        UINT32 k, c;
        BOOL   silent_phase;

        hr = IAudioClient_GetCurrentPadding(ac, &pad);
        if (FAILED(hr)) { wprintf(L"GetCurrentPadding 0x%08lX\n", (unsigned long)hr); break; }

        avail = bufFrames - pad;
        if (avail == 0) { Sleep(10); continue; }

        /* Clamp to the frame budget AND to the current phase, so a block never
         * straddles a tone/zeros boundary.  Done before GetBuffer so the
         * requested and written counts always agree. */
        silent_phase = (frames_done >= ph1_end && frames_done < ph2_end);
        {
            UINT64 room = total_frames - frames_done;
            UINT64 phase_room = silent_phase ? (ph2_end - frames_done)
                              : (frames_done < ph1_end ? (ph1_end - frames_done)
                                                       : (ph3_end - frames_done));
            if (room > phase_room) room = phase_room;
            want = ((UINT64)avail > room) ? (UINT32)room : avail;
        }
        if (want == 0) break;

        hr = IAudioRenderClient_GetBuffer(rc, want, &data);
        if (FAILED(hr)) { wprintf(L"GetBuffer 0x%08lX\n", (unsigned long)hr); break; }

        for (k = 0; k < want; k++) {
            float s;
            if (silent_phase) {
                s = 0.0f;
            } else {
                s = TONE_AMPLITUDE * (float)sin(phase);
                phase += phase_inc;
                if (phase > 6.283185307179586) phase -= 6.283185307179586;
            }
            for (c = 0; c < ch; c++) {
                if (sfmt == FMT_F32)
                    ((float *)data)[k * ch + c] = s;
                else
                    ((short *)data)[k * ch + c] = (short)(s * 32767.0f);
            }
        }

        hr = IAudioRenderClient_ReleaseBuffer(rc, want, 0);
        if (FAILED(hr)) { wprintf(L"ReleaseBuffer 0x%08lX\n", (unsigned long)hr); break; }

        if (silent_phase) zero_writes++; else tone_writes++;
        if (trace)
            wprintf(L"  wrote %6u frames at %8.2f s  (%s)\n", want,
                    (double)frames_done / sr, silent_phase ? L"ZEROS" : L"tone");

        frames_done += want;
    }

    /* Let the engine drain what is already queued, then stop.  Bounded sleep,
     * no polling loop. */
    Sleep((DWORD)(bufFrames * 1000.0 / sr) + 150);

    if (clk) {
        if (SUCCEEDED(IAudioClock_GetFrequency(clk, &clock_freq)))
            (void)IAudioClock_GetPosition(clk, &clock_pos, NULL);
    }

    hr = IAudioClient_Stop(ac);
    wprintf(L"RENDER STOP qpc=%llu  hr=0x%08lX\n",
            (unsigned long long)qpc_now(), (unsigned long)hr);
    started = FALSE;

    wprintf(L"\n--- player summary ---\n");
    wprintf(L"frames submitted        : %llu (%.2f s) of %llu planned\n",
            (unsigned long long)frames_done, (double)frames_done / sr,
            (unsigned long long)total_frames);
    wprintf(L"tone GetBuffer blocks   : %u\n", tone_writes);
    wprintf(L"zero GetBuffer blocks   : %u\n", zero_writes);
    if (clock_freq)
        wprintf(L"IAudioClock position    : %llu / freq %llu = %.3f s ACTUALLY RENDERED\n",
                (unsigned long long)clock_pos, (unsigned long long)clock_freq,
                (double)clock_pos / (double)clock_freq);
    wprintf(L"source amplitude        : %.3f  x session volume %.9f  = %.9f at endpoint\n",
            (double)TONE_AMPLITUDE, (double)readback,
            (double)TONE_AMPLITUDE * (double)readback);
    wprintf(L"now holding stream STOPPED but process alive for %.0f s\n", PH4_QUIET_S);
    fflush(stdout);

    Sleep((DWORD)(PH4_QUIET_S * 1000.0));
    wprintf(L"exiting normally\n");
    hr = S_OK;

done:
    if (started) (void)IAudioClient_Stop(ac);
    if (clk) IAudioClock_Release(clk);
    if (vol) ISimpleAudioVolume_Release(vol);
    if (rc)  IAudioRenderClient_Release(rc);
    if (ac)  IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (enu) IMMDeviceEnumerator_Release(enu);
    if (mix) CoTaskMemFree(mix);
    CoUninitialize();
    fflush(stdout);
    return FAILED(hr) ? 1 : 0;
}
