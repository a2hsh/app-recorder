/*
 * com_shim.c -- IActivateAudioInterfaceCompletionHandler, hand-vtabled in C.
 *
 * Graduated from spike/spike_loopback.c lines 68-219 (marked "COM SHIM"),
 * which worked first try against real WASAPI. The things that made it work are
 * deliberate and are NOT to be tidied away:
 *
 *   1. The SDK interface is embedded BY VALUE as the first member. The SDK C
 *      view of the interface is exactly { CONST_VTBL Vtbl *lpVtbl; }, so this
 *      is layout-identical to hand-rolling an lpVtbl, but it type-checks
 *      against the COBJMACROS accessors instead of needing casts everywhere.
 *   2. QueryInterface answers IID_IAgileObject. Not optional in practice:
 *      without it the activation can be marshalled to another apartment and
 *      fail.
 *   3. CONST_VTBL expands to nothing in C, so lpVtbl is non-const and the
 *      vtable assignment needs the cast below.
 *   4. TWO HRESULTs come back from an activation -- the return value of
 *      GetActivateResult and its out-parameter -- and either can fail
 *      independently. Both are checked.
 *
 * FIXED ON GRADUATION (the spike leaked here): if the activation completed
 * after the caller had already timed out, the IUnknown handed back by
 * GetActivateResult was never released. Now covered twice over:
 *   - the waiter marks the handler abandoned before dropping its reference, so
 *     a late callback releases the interface immediately instead of storing it;
 *   - and the handler destructor releases anything still stored, which closes
 *     the window where the callback read the flag just before it was set.
 * Whichever of the two fires, the reference is dropped exactly once.
 */
#include "apr_winver.h"

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <stdlib.h>

#include "com_shim.h"
#include "wasapi_common.h"
#include "log.h"

/* GUIDs used only by the shim. Defined here rather than linked from uuid.lib
 * so the exact bytes are visible and there are no link-order surprises; named
 * apr_* so they cannot collide with the SDK exported symbols. */
static const GUID apr_iid_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
/* IAgileObject: 94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90 */
static const GUID apr_iid_IAgileObject =
    { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90 } };
/* IActivateAudioInterfaceCompletionHandler: 41d949ab-9862-444a-80f6-c261334da5eb */
static const GUID apr_iid_ICompletionHandler =
    { 0x41d949ab, 0x9862, 0x444a, { 0x80,0xf6,0xc2,0x61,0x33,0x4d,0xa5,0xeb } };

typedef struct AprActivationHandler {
    /* MUST be first -- see note 1 in the file comment. */
    IActivateAudioInterfaceCompletionHandler base;

    LONG      ref;
    volatile LONG abandoned;   /* set by the waiter once it has given up */
    HANDLE    done;            /* signalled from the callback thread */
    HRESULT   hr_getresult;    /* the return value of GetActivateResult */
    HRESULT   hr_activate;     /* the HRESULT the activation itself produced */
    IUnknown *punk;            /* activated interface, AddRef'd by the callee */
    DWORD     callback_tid;    /* proof it lands on another thread */
} AprActivationHandler;

static HRESULT STDMETHODCALLTYPE
AH_QueryInterface(IActivateAudioInterfaceCompletionHandler *This,
                  REFIID riid, void **ppv)
{
    if (ppv == NULL) return E_POINTER;
    if (IsEqualGUID(riid, &apr_iid_IUnknown) ||
        IsEqualGUID(riid, &apr_iid_ICompletionHandler) ||
        IsEqualGUID(riid, &apr_iid_IAgileObject))
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
    AprActivationHandler *self = (AprActivationHandler *)This;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE
AH_Release(IActivateAudioInterfaceCompletionHandler *This)
{
    AprActivationHandler *self = (AprActivationHandler *)This;
    LONG n = InterlockedDecrement(&self->ref);
    if (n == 0) {
        /* Second half of the graduation fix: whoever dies last drops the
         * interface. Reaching here with punk != NULL means the callback landed
         * between the timeout of the waiter and its abandonment flag. */
        if (self->punk) {
            IUnknown_Release(self->punk);
            self->punk = NULL;
        }
        if (self->done) CloseHandle(self->done);
        free(self);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE
AH_ActivateCompleted(IActivateAudioInterfaceCompletionHandler *This,
                     IActivateAudioInterfaceAsyncOperation *op)
{
    AprActivationHandler *self = (AprActivationHandler *)This;
    HRESULT hr_activate = E_FAIL;
    HRESULT hr_get;
    IUnknown *punk = NULL;

    self->callback_tid = GetCurrentThreadId();
    hr_get = IActivateAudioInterfaceAsyncOperation_GetActivateResult(
                 op, &hr_activate, &punk);

    if (InterlockedCompareExchange(&self->abandoned, 0, 0) != 0) {
        /* The caller gave up. Nobody is coming to collect this. */
        if (punk) IUnknown_Release(punk);
    } else {
        self->hr_getresult = hr_get;
        self->hr_activate  = hr_activate;
        self->punk         = punk;
    }

    SetEvent(self->done);
    /* Returning a failure here does not propagate anywhere useful. */
    return S_OK;
}

static const IActivateAudioInterfaceCompletionHandlerVtbl g_ah_vtbl = {
    AH_QueryInterface,
    AH_AddRef,
    AH_Release,
    AH_ActivateCompleted
};

static AprActivationHandler *ah_create(void)
{
    AprActivationHandler *self =
        (AprActivationHandler *)calloc(1, sizeof(*self));
    if (!self) return NULL;
    /* Note 3: CONST_VTBL is empty in C, so this cast is required. */
    self->base.lpVtbl =
        (IActivateAudioInterfaceCompletionHandlerVtbl *)&g_ah_vtbl;
    self->ref  = 1;
    self->done = CreateEventW(NULL, TRUE /*manual reset*/, FALSE, NULL);
    if (!self->done) { free(self); return NULL; }
    return self;
}

AprErr apr_com_activate_process_loopback(uint32_t pid, int exclude,
                                         DWORD timeout_ms,
                                         IAudioClient **out_client)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params;
    PROPVARIANT pv;
    AprActivationHandler *h;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    IActivateAudioInterfaceCompletionHandler *ih;
    HRESULT hr;
    AprErr  err = apr_ok();

    if (!out_client) return APR_ERR(APR_E_INVALID_ARG, L"out_client is NULL");
    *out_client = NULL;
    if (pid == 0) return APR_ERR(APR_E_INVALID_ARG, L"pid 0 cannot be captured");

    h = ah_create();
    if (!h) return APR_ERR(APR_E_NO_MEMORY, L"activation handler");
    ih = (IActivateAudioInterfaceCompletionHandler *)h;

    ZeroMemory(&params, sizeof(params));
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = (DWORD)pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = exclude
        ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PropVariantInit(&pv);
    pv.vt             = VT_BLOB;
    pv.blob.cbSize    = (ULONG)sizeof(params);
    pv.blob.pBlobData = (BYTE *)&params;

    hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                     &apr_iid_IAudioClient, &pv, ih, &op);
    if (FAILED(hr)) {
        IActivateAudioInterfaceCompletionHandler_Release(ih);
        return APR_ERR_HR(hr, L"ActivateAudioInterfaceAsync(pid %u, %s)",
                          pid, exclude ? L"EXCLUDE" : L"INCLUDE");
    }

    if (WaitForSingleObject(h->done, timeout_ms) != WAIT_OBJECT_0) {
        /* First half of the graduation fix: tell a late callback that nobody
         * will collect its interface, THEN drop our reference. */
        InterlockedExchange(&h->abandoned, 1);
        if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
        IActivateAudioInterfaceCompletionHandler_Release(ih);
        return APR_ERR(APR_E_TIMEOUT,
                       L"activation for pid %u did not complete in %lu ms",
                       pid, timeout_ms);
    }

    APR_TRACE(L"activation callback ran on thread %lu (opener is %lu)",
              h->callback_tid, GetCurrentThreadId());

    /* Note 4: both HRESULTs. */
    hr = h->hr_getresult;
    if (SUCCEEDED(hr)) hr = h->hr_activate;

    if (FAILED(hr)) {
        err = APR_ERR_HR(hr, L"process loopback activation (pid %u, %s)",
                         pid, exclude ? L"EXCLUDE" : L"INCLUDE");
    } else if (!h->punk) {
        err = APR_ERR(APR_E_STATE,
                      L"activation for pid %u succeeded with no interface", pid);
    } else {
        hr = IUnknown_QueryInterface(h->punk, &apr_iid_IAudioClient,
                                     (void **)out_client);
        if (FAILED(hr)) {
            *out_client = NULL;
            err = APR_ERR_HR(hr, L"QueryInterface(IAudioClient) for pid %u", pid);
        }
    }

    if (h->punk) { IUnknown_Release(h->punk); h->punk = NULL; }
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
    IActivateAudioInterfaceCompletionHandler_Release(ih);
    return err;
}
