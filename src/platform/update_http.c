/*
 * update_http.c -- the only place in this program that opens a socket.
 *
 * It is a separate translation unit from update.c for one reason: everything
 * in update.c can be driven by a test, and nothing in this file can. Keeping
 * the seam at a file boundary makes "which code did the suite actually
 * exercise" a question with a one-word answer.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS NOT NEGOTIABLE HERE
 *
 *   REDIRECTS ARE FOLLOWED. The whole point of the /releases/latest/download/
 *   URL is that it 302s to whatever the current release's asset is; WinHTTP
 *   follows redirects by default and WINHTTP_OPTION_DISABLE_FEATURE is not
 *   used to switch that off.
 *
 *   CERTIFICATE VALIDATION IS NEVER RELAXED. There is no
 *   SECURITY_FLAG_IGNORE_* in this file and none is to be added -- not "just
 *   for testing", because the test path does not come through here at all
 *   (update.h section 8). The signature does not make TLS redundant: TLS is
 *   what stops a passive observer learning which builds this machine runs,
 *   and it is what stops an active one serving a stale-but-genuine release
 *   for ever to keep a known bug alive.
 *
 *   HTTPS ONLY. The scheme is not taken from the URL and trusted; a URL that
 *   is not https is refused before a connection is opened.
 *
 * ---------------------------------------------------------------------------
 * TIMEOUTS ARE SHORT ON PURPOSE
 *
 *   This runs on a worker thread while somebody may be recording. A captive
 *   portal that accepts a connection and then says nothing is the common case
 *   on hotel and conference wifi, and the right behaviour there is to give up
 *   quickly and be silent -- a check that cannot run is not an error
 *   (update.h section 7). Ten seconds a phase, and the download gets longer
 *   because a megabyte on a bad line legitimately takes a while.
 */
#include <windows.h>
#include <winhttp.h>

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "update.h"

#define UPD_UA L"apprecorder/" APR_VERSION_STRING

#define UPD_TIMEOUT_MS      10000
#define UPD_DOWNLOAD_MS     120000
#define UPD_URL_PART_CCH    512

typedef struct Split {
    wchar_t host[256];
    wchar_t path[UPD_URL_PART_CCH];
    INTERNET_PORT port;
} Split;

static int split_url(const wchar_t *url, Split *s)
{
    URL_COMPONENTS uc;

    memset(s, 0, sizeof *s);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = s->host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = s->path;
    uc.dwUrlPathLength = UPD_URL_PART_CCH;

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    /* See the header: the scheme is checked, not assumed. */
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return 0;
    s->port = uc.nPort;
    return 1;
}

/* One request, from session to response, with everything closed on every
 * path. `sink` is called with each chunk; NULL means "count only". */
typedef int (*SinkFn)(void *user, const void *data, size_t n);

typedef struct Req {
    const wchar_t *url;
    const wchar_t *etag_in;
    DWORD          timeout_ms;
    SinkFn         sink;
    void          *sink_user;
    DWORD          status;
    wchar_t        etag_out[APR_UPDATE_ETAG_CCH];
} Req;

static AprErr do_request(Req *r)
{
    Split     sp;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    AprErr    e = apr_ok();
    DWORD     cb, status = 0;

    if (!split_url(r->url, &sp)) {
        return APR_ERR(APR_E_INVALID_ARG,
                       L"update: \"%ls\" is not an https URL", r->url);
    }

    ses = WinHttpOpen(UPD_UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        /* Older builds reject AUTOMATIC_PROXY; fall back rather than making
         * the updater a Windows-version question. */
        ses = WinHttpOpen(UPD_UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) return APR_ERR_LAST(L"update: WinHttpOpen");

    WinHttpSetTimeouts(ses, (int)r->timeout_ms, (int)r->timeout_ms,
                       (int)r->timeout_ms, (int)r->timeout_ms);

    con = WinHttpConnect(ses, sp.host, sp.port, 0);
    if (!con) { e = APR_ERR_LAST(L"update: connecting to %ls", sp.host); goto done; }

    req = WinHttpOpenRequest(con, L"GET", sp.path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
    if (!req) { e = APR_ERR_LAST(L"update: opening a request"); goto done; }

    if (r->etag_in && r->etag_in[0]) {
        wchar_t hdr[APR_UPDATE_ETAG_CCH + 32];
        _snwprintf_s(hdr, sizeof hdr / sizeof hdr[0], _TRUNCATE,
                     L"If-None-Match: %ls", r->etag_in);
        WinHttpAddRequestHeaders(req, hdr, (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        e = APR_ERR_LAST(L"update: sending the request");
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        e = APR_ERR_LAST(L"update: waiting for the response");
        goto done;
    }

    cb = sizeof status;
    if (!WinHttpQueryHeaders(req,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb,
                             WINHTTP_NO_HEADER_INDEX)) {
        e = APR_ERR_LAST(L"update: reading the status line");
        goto done;
    }
    r->status = status;

    cb = sizeof r->etag_out;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_ETAG,
                             WINHTTP_HEADER_NAME_BY_INDEX, r->etag_out, &cb,
                             WINHTTP_NO_HEADER_INDEX)) {
        r->etag_out[0] = L'\0';
    }
    r->etag_out[APR_UPDATE_ETAG_CCH - 1] = L'\0';

    if (status == 304 || !r->sink) goto done;

    for (;;) {
        DWORD avail = 0, got = 0;
        BYTE  chunk[8192];

        if (!WinHttpQueryDataAvailable(req, &avail)) {
            e = APR_ERR_LAST(L"update: reading the response body");
            goto done;
        }
        if (avail == 0) break;
        if (avail > sizeof chunk) avail = sizeof chunk;
        if (!WinHttpReadData(req, chunk, avail, &got)) {
            e = APR_ERR_LAST(L"update: reading the response body");
            goto done;
        }
        if (got == 0) break;
        if (!r->sink(r->sink_user, chunk, got)) {
            e = APR_ERR(APR_E_OVERRUN, L"update: the response is larger than "
                                       L"apprecorder will accept");
            goto done;
        }
    }

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return e;
}

/* --- sink: into a caller's fixed buffer ---------------------------------- */

typedef struct BufSink {
    BYTE  *buf;
    size_t cap;
    size_t len;
} BufSink;

static int buf_sink(void *user, const void *data, size_t n)
{
    BufSink *b = (BufSink *)user;
    if (b->len + n > b->cap) return 0;      /* refuse, do not truncate */
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return 1;
}

static AprErr http_get(void *user, const wchar_t *url, const wchar_t *etag_in,
                       void *body, size_t cap, AprUpdateResponse *out)
{
    Req     r;
    BufSink b;
    AprErr  e;

    (void)user;
    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"update: get with no response");
    memset(out, 0, sizeof *out);

    b.buf = (BYTE *)body;
    b.cap = cap;
    b.len = 0;

    memset(&r, 0, sizeof r);
    r.url = url;
    r.etag_in = etag_in;
    r.timeout_ms = UPD_TIMEOUT_MS;
    r.sink = buf_sink;
    r.sink_user = &b;

    e = do_request(&r);
    if (apr_failed(&e)) return e;

    out->status = (int)r.status;
    out->len = b.len;
    lstrcpynW(out->etag, r.etag_out, APR_UPDATE_ETAG_CCH);
    return apr_ok();
}

/* --- sink: straight to a file -------------------------------------------- */

typedef struct FileSink {
    HANDLE h;
    int    failed;
} FileSink;

static int file_sink(void *user, const void *data, size_t n)
{
    FileSink *f = (FileSink *)user;
    DWORD     wrote = 0;
    if (!WriteFile(f->h, data, (DWORD)n, &wrote, NULL) || wrote != n) {
        f->failed = 1;
        return 0;
    }
    return 1;
}

static AprErr http_download(void *user, const wchar_t *url,
                            const wchar_t *dest_path)
{
    Req      r;
    FileSink f;
    AprErr   e;

    (void)user;
    f.h = CreateFileW(dest_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (f.h == INVALID_HANDLE_VALUE)
        return APR_ERR_LAST(L"update: creating \"%ls\"", dest_path);
    f.failed = 0;

    memset(&r, 0, sizeof r);
    r.url = url;
    r.etag_in = NULL;
    /* Longer than a manifest fetch: a megabyte on a poor line is not a fault.
     * Still bounded, because a stalled download must not hold a thread open
     * across a whole recording. */
    r.timeout_ms = UPD_DOWNLOAD_MS;
    r.sink = file_sink;
    r.sink_user = &f;

    e = do_request(&r);
    CloseHandle(f.h);

    if (apr_failed(&e)) { DeleteFileW(dest_path); return e; }
    if (r.status != 200) {
        DeleteFileW(dest_path);
        return APR_ERR(APR_E_NOT_FOUND, L"update: \"%ls\" answered %lu",
                       url, (unsigned long)r.status);
    }
    if (f.failed) {
        DeleteFileW(dest_path);
        return APR_ERR(APR_E_IO, L"update: writing \"%ls\"", dest_path);
    }
    return apr_ok();
}

static const AprUpdateHttp g_winhttp = { http_get, http_download, NULL };

const AprUpdateHttp *apr_update_http_winhttp(void)
{
    return &g_winhttp;
}
