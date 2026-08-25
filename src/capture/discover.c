/*
 * discover.c -- "what can I record right now?", answered from the audio
 * engine rather than from the process table (see discover.h for why).
 *
 * Nothing here opens an IAudioClient. Enumerating endpoints and sessions is a
 * property query; it starts no stream, allocates no buffer and makes no sound.
 * That is what lets --dry-run check a whole configuration without recording,
 * and it is why this file lives in capture/ but is not an AprCapture.
 *
 * The session-walking loop is deliberately shaped like the one in
 * capture_process.c's find_session_volume: every ACTIVE render endpoint, not
 * merely the default one, because an application can be playing to any of them
 * and on the author's rig several are live at once.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>

#include "discover.h"
#include "wasapi_common.h"

/* ---------------------------------------------------------------------------
 * COM scope. Nested CoInitializeEx on a thread already in the MTA returns
 * S_FALSE and still needs its CoUninitialize, so the flag records whether this
 * call made one, not whether it was first.
 * ------------------------------------------------------------------------- */
typedef struct ComScope { int entered; } ComScope;

static AprErr com_enter(ComScope *cs)
{
    HRESULT hr;

    cs->entered = 0;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        /* An STA thread can still query; it simply must not uninitialise. */
        return apr_ok();
    }
    if (FAILED(hr)) return APR_ERR_HR(hr, L"CoInitializeEx(MTA)");
    cs->entered = 1;
    return apr_ok();
}

static void com_leave(ComScope *cs)
{
    if (cs->entered) { CoUninitialize(); cs->entered = 0; }
}

static void copy_cch(wchar_t *dst, size_t cch, const wchar_t *src)
{
    if (!dst || cch == 0) return;
    dst[0] = L'\0';
    if (!src) return;
    wcsncpy_s(dst, cch, src, _TRUNCATE);
}

/* ---------------------------------------------------------------------------
 * Processes
 * ------------------------------------------------------------------------- */

int apr_process_exists(uint32_t pid)
{
    HANDLE h;

    if (pid == 0) return 0;   /* the System Idle Process is not an application */
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        /* ERROR_ACCESS_DENIED means it exists and we may not look at it. */
        return GetLastError() == ERROR_ACCESS_DENIED ? 1 : 0;
    }
    CloseHandle(h);
    return 1;
}

static size_t image_path(uint32_t pid, wchar_t *buf, size_t cch)
{
    HANDLE h;
    DWORD  n = (DWORD)cch;

    if (!buf || cch == 0) return 0;
    buf[0] = L'\0';
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    if (!QueryFullProcessImageNameW(h, 0, buf, &n)) { n = 0; buf[0] = L'\0'; }
    CloseHandle(h);
    return (size_t)n;
}

static const wchar_t *leaf_of(const wchar_t *path)
{
    const wchar_t *slash;

    if (!path) return NULL;
    slash = wcsrchr(path, L'\\');
    if (slash) return slash + 1;
    slash = wcsrchr(path, L'/');
    return slash ? slash + 1 : path;
}

size_t apr_process_image_name(uint32_t pid, wchar_t *buf, size_t cch)
{
    wchar_t full[APR_DISC_PATH_CCH];
    const wchar_t *leaf;

    if (!buf || cch == 0) return 0;
    buf[0] = L'\0';
    if (image_path(pid, full, sizeof full / sizeof full[0]) == 0) return 0;
    leaf = leaf_of(full);
    copy_cch(buf, cch, leaf);
    return wcslen(buf);
}

/* ---------------------------------------------------------------------------
 * The process tree
 *
 * Toolhelp gives a flat parent/child snapshot; the transitive closure is a
 * repeated sweep rather than recursion, which keeps the stack out of it and
 * cannot loop even if a snapshot ever contained a cycle (a pid is only ever
 * added once).
 * ------------------------------------------------------------------------- */

#define TREE_SNAP_MAX 4096

AprErr apr_enum_process_tree(uint32_t root, uint32_t *out, size_t cap,
                             size_t *out_count)
{
    static struct { DWORD pid, parent; } snap[TREE_SNAP_MAX];
    static uint32_t members[TREE_SNAP_MAX];
    size_t  snap_n = 0, mem_n = 0, i;
    int     grew = 1;
    HANDLE  h;
    PROCESSENTRY32W pe;

    if (out_count) *out_count = 0;
    if (!apr_process_exists(root))
        return APR_ERR(APR_E_NOT_FOUND, L"no process with id %u", root);

    h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE)
        return APR_ERR_LAST(L"CreateToolhelp32Snapshot");

    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    if (Process32FirstW(h, &pe)) {
        do {
            if (snap_n >= TREE_SNAP_MAX) break;
            snap[snap_n].pid    = pe.th32ProcessID;
            snap[snap_n].parent = pe.th32ParentProcessID;
            snap_n++;
        } while (Process32NextW(h, &pe));
    }
    CloseHandle(h);

    members[mem_n++] = root;
    while (grew) {
        grew = 0;
        for (i = 0; i < snap_n; i++) {
            size_t k;
            int    already = 0, parent_in = 0;

            if (snap[i].pid == 0) continue;
            for (k = 0; k < mem_n; k++) {
                if (members[k] == (uint32_t)snap[i].pid)    already   = 1;
                if (members[k] == (uint32_t)snap[i].parent) parent_in = 1;
            }
            if (already || !parent_in) continue;
            if (mem_n >= TREE_SNAP_MAX) break;
            members[mem_n++] = (uint32_t)snap[i].pid;
            grew = 1;
        }
    }

    for (i = 0; i < mem_n && out && i < cap; i++) out[i] = members[i];
    if (out_count) *out_count = mem_n;
    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * Audio applications
 * ------------------------------------------------------------------------- */

static int app_index(const AprAudioApp *rows, size_t n, uint32_t pid)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (rows[i].pid == pid) return (int)i;
    }
    return -1;
}

static void fill_app(AprAudioApp *a, uint32_t pid, IAudioSessionControl *ctl)
{
    wchar_t *display = NULL;
    wchar_t  full[APR_DISC_PATH_CCH];

    memset(a, 0, sizeof *a);
    a->pid    = pid;
    a->volume = 1.0f;

    if (image_path(pid, full, sizeof full / sizeof full[0]) > 0) {
        copy_cch(a->path, APR_DISC_PATH_CCH, full);
        copy_cch(a->exe,  APR_DISC_NAME_CCH, leaf_of(full));
    }

    /* The session's own display name is usually empty for ordinary apps -- it
     * is set by the ones that bother -- so the executable is the fallback and
     * not the other way round. */
    if (SUCCEEDED(IAudioSessionControl_GetDisplayName(ctl, &display)) && display) {
        if (display[0]) copy_cch(a->display, APR_DISC_NAME_CCH, display);
        CoTaskMemFree(display);
    }
    if (!a->display[0]) copy_cch(a->display, APR_DISC_NAME_CCH, a->exe);
}

static void collect_sessions(IMMDevice *dev, AprAudioApp *out, size_t cap,
                             size_t *total)
{
    IAudioSessionManager2   *mgr = NULL;
    IAudioSessionEnumerator *sen = NULL;
    int n = 0, k;

    if (FAILED(IMMDevice_Activate(dev, &apr_iid_IAudioSessionManager2,
                                  CLSCTX_ALL, NULL, (void **)&mgr)))
        return;
    if (SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &sen)) &&
        SUCCEEDED(IAudioSessionEnumerator_GetCount(sen, &n)))
    {
        for (k = 0; k < n; k++) {
            IAudioSessionControl  *ctl  = NULL;
            IAudioSessionControl2 *ctl2 = NULL;
            ISimpleAudioVolume    *vol  = NULL;
            AudioSessionState      state = AudioSessionStateInactive;
            DWORD spid = 0;
            int   idx;

            if (FAILED(IAudioSessionEnumerator_GetSession(sen, k, &ctl)))
                continue;

            if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                    ctl, &apr_iid_IAudioSessionControl2, (void **)&ctl2)))
            {
                if (FAILED(IAudioSessionControl2_GetProcessId(ctl2, &spid)))
                    spid = 0;
                IAudioSessionControl2_Release(ctl2);
            }
            if (spid == 0 || spid == GetCurrentProcessId()) {
                /* pid 0 is the system-sounds session, which has no process to
                 * capture; our own pid would be a recorder recording itself. */
                IAudioSessionControl_Release(ctl);
                continue;
            }
            if (FAILED(IAudioSessionControl_GetState(ctl, &state)))
                state = AudioSessionStateInactive;
            if (state == AudioSessionStateExpired) {
                IAudioSessionControl_Release(ctl);
                continue;
            }

            idx = app_index(out, (*total < cap) ? *total : cap, (uint32_t)spid);
            if (idx < 0) {
                if (*total < cap) {
                    fill_app(&out[*total], (uint32_t)spid, ctl);
                    idx = (int)*total;
                }
                (*total)++;
            }

            if (idx >= 0) {
                if (state == AudioSessionStateActive) out[idx].active = 1;
                if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                        ctl, &apr_iid_ISimpleAudioVolume, (void **)&vol)))
                {
                    BOOL  mute = FALSE;
                    float v = 1.0f;
                    if (SUCCEEDED(ISimpleAudioVolume_GetMute(vol, &mute)) &&
                        SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(vol, &v)))
                    {
                        out[idx].volume = v;
                        /* Both record as pure silence, so both raise the flag. */
                        if (mute || v == 0.0f) out[idx].muted = 1;
                    }
                    ISimpleAudioVolume_Release(vol);
                }
            }
            IAudioSessionControl_Release(ctl);
        }
    }

    if (sen) IAudioSessionEnumerator_Release(sen);
    if (mgr) IAudioSessionManager2_Release(mgr);
}

AprErr apr_enum_audio_apps(AprAudioApp *out, size_t cap, size_t *out_count)
{
    ComScope             cs;
    IMMDeviceEnumerator *enu = NULL;
    IMMDeviceCollection *coll = NULL;
    AprErr  e;
    HRESULT hr;
    UINT    count = 0, i;
    size_t  total = 0;

    if (out_count) *out_count = 0;
    if (cap > 0 && !out) return APR_ERR(APR_E_INVALID_ARG, L"cap without array");

    e = com_enter(&cs);
    if (apr_failed(&e)) return e;

    hr = CoCreateInstance(&apr_clsid_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &apr_iid_IMMDeviceEnumerator, (void **)&enu);
    if (FAILED(hr)) {
        com_leave(&cs);
        return APR_ERR_HR(hr, L"CoCreateInstance(MMDeviceEnumerator)");
    }

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enu, eRender, DEVICE_STATE_ACTIVE,
                                                &coll);
    if (FAILED(hr)) {
        IMMDeviceEnumerator_Release(enu);
        com_leave(&cs);
        return APR_ERR_HR(hr, L"EnumAudioEndpoints(eRender)");
    }

    if (FAILED(IMMDeviceCollection_GetCount(coll, &count))) count = 0;
    for (i = 0; i < count; i++) {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev))) continue;
        collect_sessions(dev, out, cap, &total);
        IMMDevice_Release(dev);
    }

    IMMDeviceCollection_Release(coll);
    IMMDeviceEnumerator_Release(enu);
    com_leave(&cs);

    if (out_count) *out_count = total;
    return apr_ok();
}

/* ---------------------------------------------------------------------------
 * Capture endpoints
 * ------------------------------------------------------------------------- */

static void endpoint_name(IMMDevice *dev, wchar_t *buf, size_t cch)
{
    IPropertyStore *props = NULL;
    PROPVARIANT     pv;

    if (!buf || cch == 0) return;
    buf[0] = L'\0';
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) return;

    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal)
    {
        copy_cch(buf, cch, pv.pwszVal);
    }
    PropVariantClear(&pv);
    IPropertyStore_Release(props);
}

AprErr apr_enum_capture_endpoints(AprAudioEndpoint *out, size_t cap,
                                  size_t *out_count)
{
    ComScope             cs;
    IMMDeviceEnumerator *enu = NULL;
    IMMDeviceCollection *coll = NULL;
    IMMDevice           *def = NULL;
    wchar_t             *def_id = NULL;
    AprErr  e;
    HRESULT hr;
    UINT    count = 0, i;
    size_t  total = 0;

    if (out_count) *out_count = 0;
    if (cap > 0 && !out) return APR_ERR(APR_E_INVALID_ARG, L"cap without array");

    e = com_enter(&cs);
    if (apr_failed(&e)) return e;

    hr = CoCreateInstance(&apr_clsid_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &apr_iid_IMMDeviceEnumerator, (void **)&enu);
    if (FAILED(hr)) {
        com_leave(&cs);
        return APR_ERR_HR(hr, L"CoCreateInstance(MMDeviceEnumerator)");
    }

    /* A machine with no capture device at all has no default one either, and
     * that is not an error -- it is the answer. */
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enu, eCapture,
                                                              eConsole, &def)))
    {
        (void)IMMDevice_GetId(def, &def_id);
    }

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enu, eCapture,
                                                DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) {
        if (def_id) CoTaskMemFree(def_id);
        if (def) IMMDevice_Release(def);
        IMMDeviceEnumerator_Release(enu);
        com_leave(&cs);
        return APR_ERR_HR(hr, L"EnumAudioEndpoints(eCapture)");
    }

    if (FAILED(IMMDeviceCollection_GetCount(coll, &count))) count = 0;
    for (i = 0; i < count; i++) {
        IMMDevice *dev = NULL;
        wchar_t   *id  = NULL;

        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev))) continue;
        if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) {
            if (total < cap) {
                memset(&out[total], 0, sizeof out[total]);
                copy_cch(out[total].id, APR_DISC_ENDPOINT_CCH, id);
                endpoint_name(dev, out[total].name, APR_DISC_NAME_CCH);
                out[total].is_default = (def_id && wcscmp(def_id, id) == 0);
            }
            total++;
            CoTaskMemFree(id);
        }
        IMMDevice_Release(dev);
    }

    IMMDeviceCollection_Release(coll);
    if (def_id) CoTaskMemFree(def_id);
    if (def) IMMDevice_Release(def);
    IMMDeviceEnumerator_Release(enu);
    com_leave(&cs);

    if (out_count) *out_count = total;
    return apr_ok();
}
