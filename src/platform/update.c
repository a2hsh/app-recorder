/*
 * update.c -- the updater's decisions, its cryptography, and its file moves.
 * Read include/update.h first; it holds the threat model and the four rules.
 *
 * What follows are the things about THIS file that are load-bearing rather
 * than obvious.
 *
 * ---------------------------------------------------------------------------
 * 1. THE SIGNATURE COVERS THE BYTES AS FETCHED.
 *
 *    Not a re-serialization, not a canonical form, not "the fields we parsed".
 *    apr_update_verify_manifest() verifies FIRST and parses SECOND, over the
 *    same buffer, and nothing between the socket and BCryptVerifySignature
 *    touches it -- no trimming, no BOM removal, no newline translation. A
 *    verifier that normalizes before checking is a verifier with a second,
 *    undocumented format that only the attacker has read carefully.
 *
 * ---------------------------------------------------------------------------
 * 2. THE ETag IS ONLY REMEMBERED AFTER A GOOD SIGNATURE.
 *
 *    This is subtle and it matters. Store the ETag of a response whose
 *    signature failed, and the next check sends If-None-Match, gets 304, and
 *    reports "up to date" -- which means one forged release silences the
 *    refusal for ever after. So the ETag is written only on the path where the
 *    manifest verified.
 *
 * ---------------------------------------------------------------------------
 * 3. THE LAST-CHECK TIME ADVANCES WHEN THE SERVER ANSWERED, NOT WHEN THE
 *    CHECK SUCCEEDED.
 *
 *    An hour offline must not leave the machine believing it checked twelve
 *    times (it did not), and a release that is still half uploaded must not be
 *    retried in a tight loop (it would be, if a missing signature left the
 *    timestamp alone and something other than the five-minute timer asked
 *    again). "The host answered at all" is the honest predicate for both.
 *
 * ---------------------------------------------------------------------------
 * 4. JSON IS TOKENIZED BY THE SAME jsmn session_load.c USES.
 *
 *    Design section 9 names ONE OWNER FOR THE SESSION FORMAT, and that owner
 *    is session_load.c; this is a different format with a different owner.
 *    What must not happen is a SECOND tokenizer entering vendor/ for a
 *    six-field document (AGENTS.md rule 8), so this file includes the same
 *    header-only one, JSMN_STATIC and JSMN_STRICT, exactly as that file does.
 *    Nothing of jsmn reaches the link from either.
 *
 *    And the same lesson applies: jsmn is a tokenizer, not a validator. Every
 *    field's TYPE is checked here before it is read, and a missing field is
 *    fatal rather than defaulted -- a manifest with no "sha256" is not a
 *    manifest with an empty hash.
 */
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "log.h"
#include "strings.h"
#include "update.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

/* See note 4. */
#define JSMN_STATIC
#define JSMN_STRICT
#pragma warning(push)
#pragma warning(disable : 4127 4244 4245 4267 4389 4505 4701)
#include "jsmn.h"
#pragma warning(pop)

/* A manifest is six fields. This is the structural ceiling, not a budget. */
#define UPD_MAX_TOKENS 128

/* Long enough for any path Windows will hand back, plus the ".new" suffix. */
#define UPD_PATH_CCH 1024

/* Where the settings live. HKCU, so no elevation and no machine-wide state. */
#define UPD_KEY_REAL L"Software\\apprecorder\\Update"

/* Redirected by apr_update_state_test_redirect(); empty means the real key. */
static wchar_t g_state_key[256];
static CRITICAL_SECTION g_state_lock;
static INIT_ONCE         g_state_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK state_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_state_lock);
    return TRUE;
}

static void state_lock(void)
{
    InitOnceExecuteOnce(&g_state_once, state_init, NULL, NULL);
    EnterCriticalSection(&g_state_lock);
}

static void state_unlock(void)
{
    LeaveCriticalSection(&g_state_lock);
}

static const wchar_t *state_key(void)
{
    return g_state_key[0] ? g_state_key : UPD_KEY_REAL;
}

/* The trusted key, and the test override of it. See update.h. */
static const uint8_t *g_key_override;

void apr_update_test_set_key(const uint8_t *pubkey)
{
    g_key_override = pubkey;
}

static const uint8_t *trusted_key(void)
{
    return g_key_override ? g_key_override : apr_update_public_key();
}

static int have_trusted_key(void)
{
    return g_key_override ? 1 : apr_update_have_key();
}

/* ===========================================================================
 * SHA-256
 * ========================================================================= */

static AprErr sha256_buffer(const void *data, size_t len,
                            uint8_t out[APR_UPDATE_SHA256_BYTES])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    NTSTATUS           st;
    AprErr             e = apr_ok();

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (st != STATUS_SUCCESS)
        return APR_ERR(APR_E_UNSUPPORTED, L"update: SHA-256 unavailable (0x%08X)",
                       (unsigned)st);

    st = BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    if (st == STATUS_SUCCESS) {
        st = BCryptHashData(h, (PUCHAR)(void *)data, (ULONG)len, 0);
    }
    if (st == STATUS_SUCCESS) {
        st = BCryptFinishHash(h, out, APR_UPDATE_SHA256_BYTES, 0);
    }
    if (st != STATUS_SUCCESS) {
        e = APR_ERR(APR_E_IO, L"update: hashing failed (0x%08X)", (unsigned)st);
    }

    if (h)   BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return e;
}

AprErr apr_update_hash_file(const wchar_t *path,
                            uint8_t out[APR_UPDATE_SHA256_BYTES])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    HANDLE             f = INVALID_HANDLE_VALUE;
    NTSTATUS           st;
    AprErr             e = apr_ok();
    /* 64 KB: big enough that the syscall is not the cost, small enough to sit
     * on any thread's stack budget without thinking about it. */
    static const DWORD CHUNK = 65536;
    BYTE              *buf = NULL;

    if (!path || !out) return APR_ERR(APR_E_INVALID_ARG, L"apr_update_hash_file: NULL");

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return APR_ERR_LAST(L"update: opening \"%ls\" to hash it", path);

    buf = (BYTE *)malloc(CHUNK);
    if (!buf) {
        CloseHandle(f);
        return APR_ERR(APR_E_NO_MEMORY, L"update: hash buffer");
    }

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (st == STATUS_SUCCESS) st = BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    if (st != STATUS_SUCCESS) {
        e = APR_ERR(APR_E_UNSUPPORTED, L"update: SHA-256 unavailable (0x%08X)",
                    (unsigned)st);
        goto done;
    }

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f, buf, CHUNK, &got, NULL)) {
            e = APR_ERR_LAST(L"update: reading \"%ls\" to hash it", path);
            goto done;
        }
        if (got == 0) break;
        st = BCryptHashData(h, buf, got, 0);
        if (st != STATUS_SUCCESS) {
            e = APR_ERR(APR_E_IO, L"update: hashing failed (0x%08X)", (unsigned)st);
            goto done;
        }
    }

    st = BCryptFinishHash(h, out, APR_UPDATE_SHA256_BYTES, 0);
    if (st != STATUS_SUCCESS)
        e = APR_ERR(APR_E_IO, L"update: hashing failed (0x%08X)", (unsigned)st);

done:
    free(buf);
    if (h)   BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return e;
}

/* ===========================================================================
 * ECDSA P-256 verification
 * ========================================================================= */

/* The public key blob BCrypt wants: a header naming the curve and the key
 * size, followed by X||Y. Built here rather than stored, so the compiled-in
 * constant in update_key.c is exactly the 64 bytes a person can compare
 * against what the keygen printed and nothing else. */
static AprErr verify_ecdsa(const uint8_t *pubkey,
                           const uint8_t hash[APR_UPDATE_SHA256_BYTES],
                           const void *sig, size_t sig_len)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    NTSTATUS          st;
    AprErr            e = apr_ok();
    BYTE              blob[sizeof(BCRYPT_ECCKEY_BLOB) + APR_UPDATE_PUBKEY_BYTES];
    BCRYPT_ECCKEY_BLOB *hdr = (BCRYPT_ECCKEY_BLOB *)blob;

    if (sig_len != APR_UPDATE_SIG_BYTES) {
        /* A signature of the wrong length is refused HERE rather than handed
         * to BCrypt, so the sentence names the real problem: this is what a
         * half-uploaded or truncated .sig looks like. */
        return APR_ERR_SAY(APR_E_STATE, APR_S_ERR_UPDATE_SIGNATURE,
                           L"update: signature is %zu bytes, expected %d",
                           sig_len, (int)APR_UPDATE_SIG_BYTES);
    }

    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey   = APR_UPDATE_PUBKEY_BYTES / 2;
    memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), pubkey, APR_UPDATE_PUBKEY_BYTES);

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    if (st != STATUS_SUCCESS)
        return APR_ERR(APR_E_UNSUPPORTED,
                       L"update: ECDSA P-256 unavailable (0x%08X)", (unsigned)st);

    st = BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key,
                             blob, (ULONG)sizeof blob, 0);
    if (st != STATUS_SUCCESS) {
        /* A key that will not import is a key that is not a point on the
         * curve. That is a defect in this build, not in the download. */
        e = APR_ERR(APR_E_INVALID_ARG,
                    L"update: the compiled-in public key is not valid (0x%08X)",
                    (unsigned)st);
        goto done;
    }

    st = BCryptVerifySignature(key, NULL,
                               (PUCHAR)(void *)hash, APR_UPDATE_SHA256_BYTES,
                               (PUCHAR)(void *)sig, (ULONG)sig_len, 0);
    if (st != STATUS_SUCCESS) {
        e = APR_ERR_SAY(APR_E_STATE, APR_S_ERR_UPDATE_SIGNATURE,
                        L"update: signature does not verify (0x%08X)",
                        (unsigned)st);
    }

done:
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return e;
}

/* ===========================================================================
 * The manifest
 * ========================================================================= */

typedef struct MP {
    const char      *js;
    const jsmntok_t *t;
    int              ntok;
} MP;

static size_t tok_len(const jsmntok_t *t)
{
    return (size_t)(t->end - t->start);
}

static int key_is(const MP *p, int i, const char *name)
{
    size_t n = tok_len(&p->t[i]);
    if (p->t[i].type != JSMN_STRING) return 0;
    if (strlen(name) != n) return 0;
    return memcmp(p->js + p->t[i].start, name, n) == 0;
}

/* A JSON string token as wide text.
 *
 * NO ESCAPE HANDLING, DELIBERATELY, and refusing rather than ignoring: the
 * three fields that mean anything (version, asset, sha256) are constrained
 * character sets that cannot legitimately contain a backslash, and `notes` is
 * prose the author writes. A manifest with an escape in it is one this
 * program does not understand, and "does not understand" resolves to "refuse",
 * never to "read the part before the backslash". */
static int tok_wstr(const MP *p, int i, wchar_t *out, size_t cch)
{
    size_t n = tok_len(&p->t[i]);
    int    w;
    size_t k;

    out[0] = L'\0';
    if (p->t[i].type != JSMN_STRING) return 0;
    for (k = 0; k < n; k++) {
        if (p->js[p->t[i].start + k] == '\\') return 0;
    }
    if (n == 0) return 1;

    w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            p->js + p->t[i].start, (int)n,
                            out, (int)(cch - 1));
    if (w <= 0) return 0;
    out[w] = L'\0';
    return 1;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int tok_sha256(const MP *p, int i, uint8_t out[APR_UPDATE_SHA256_BYTES])
{
    const char *s = p->js + p->t[i].start;
    size_t      n = tok_len(&p->t[i]);
    size_t      k;

    if (p->t[i].type != JSMN_STRING) return 0;
    if (n != APR_UPDATE_SHA256_BYTES * 2) return 0;
    for (k = 0; k < APR_UPDATE_SHA256_BYTES; k++) {
        int hi = hex_nibble(s[k * 2]);
        int lo = hex_nibble(s[k * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[k] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static int tok_u64(const MP *p, int i, uint64_t *out)
{
    const char *s = p->js + p->t[i].start;
    size_t      n = tok_len(&p->t[i]);
    uint64_t    v = 0;
    size_t      k;

    if (p->t[i].type != JSMN_PRIMITIVE) return 0;
    if (n == 0 || n > 20) return 0;
    for (k = 0; k < n; k++) {
        if (s[k] < '0' || s[k] > '9') return 0;
        v = v * 10 + (uint64_t)(s[k] - '0');
    }
    *out = v;
    return 1;
}

static AprErr parse_manifest(const char *js, size_t len, AprUpdateManifest *out)
{
    jsmn_parser  jp;
    jsmntok_t    tok[UPD_MAX_TOKENS];
    MP           p;
    int          n, i, members;
    int          have_version = 0, have_sha = 0, have_asset = 0;
    AprVersion   v;

    memset(out, 0, sizeof *out);

    jsmn_init(&jp);
    n = jsmn_parse(&jp, js, len, tok, UPD_MAX_TOKENS);
    if (n < 1 || tok[0].type != JSMN_OBJECT) {
        return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                           L"update: release.json is not a JSON object");
    }

    p.js = js;
    p.t = tok;
    p.ntok = n;

    members = tok[0].size;
    i = 1;
    while (members-- > 0) {
        int kt = i, vt = i + 1;

        if (vt >= n) {
            return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                               L"update: release.json ends mid-member");
        }
        if (tok[kt].type != JSMN_STRING) {
            return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                               L"update: release.json has a non-string key");
        }
        /* Every value in this document is a scalar. An object or an array
         * where one is not expected is a document from somewhere else. */
        if (tok[vt].type == JSMN_OBJECT || tok[vt].type == JSMN_ARRAY) {
            return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                               L"update: release.json nests where it should not");
        }

        if (key_is(&p, kt, "version")) {
            if (!tok_wstr(&p, vt, out->version, APR_UPDATE_VERSION_CCH))
                return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                                   L"update: \"version\" is not readable");
            have_version = 1;
        } else if (key_is(&p, kt, "asset")) {
            if (!tok_wstr(&p, vt, out->asset, APR_UPDATE_ASSET_CCH))
                return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                                   L"update: \"asset\" is not readable");
            have_asset = 1;
        } else if (key_is(&p, kt, "notes")) {
            /* Prose, and the only field that is allowed to be absent or odd:
             * it decides nothing. */
            (void)tok_wstr(&p, vt, out->notes, APR_UPDATE_NOTES_CCH);
        } else if (key_is(&p, kt, "sha256")) {
            if (!tok_sha256(&p, vt, out->sha256))
                return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                                   L"update: \"sha256\" is not 64 hex digits");
            have_sha = 1;
        } else if (key_is(&p, kt, "size")) {
            if (!tok_u64(&p, vt, &out->size))
                return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                                   L"update: \"size\" is not a whole number");
        }
        /* An unknown key is IGNORED, exactly as session_load.c ignores one:
         * that is the whole forward-compatibility mechanism, and a later
         * release adding a field must not stop this build updating to it. */

        i = vt + 1;
    }

    if (!have_version || !have_sha || !have_asset) {
        return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                           L"update: release.json is missing version, asset or sha256");
    }
    if (!apr_version_parse(out->version, &v)) {
        return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                           L"update: \"%ls\" is not a version this build can order",
                           out->version);
    }
    /* An asset name is a FILE NAME that will be appended to a URL and used as
     * part of a path. A separator in it is a path traversal attempt. */
    if (out->asset[0] == L'\0' || wcschr(out->asset, L'/') ||
        wcschr(out->asset, L'\\') || wcschr(out->asset, L':')) {
        return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                           L"update: \"asset\" is not a plain file name");
    }
    return apr_ok();
}

AprErr apr_update_verify_manifest(const void *json, size_t json_len,
                                  const void *sig, size_t sig_len,
                                  const uint8_t *pubkey,
                                  AprUpdateManifest *out)
{
    uint8_t hash[APR_UPDATE_SHA256_BYTES];
    AprErr  e;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_update_verify_manifest: NULL out");
    memset(out, 0, sizeof *out);
    if (!json || !sig || !pubkey)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_update_verify_manifest: NULL");
    if (json_len == 0 || json_len > APR_UPDATE_MANIFEST_MAX)
        return APR_ERR_SAY(APR_E_INVALID_ARG, APR_S_ERR_UPDATE_MANIFEST,
                           L"update: release.json is %zu bytes", json_len);

    /* VERIFY BEFORE PARSE. The parser is the more complicated of the two and
     * it must never run on bytes nobody has vouched for. */
    e = sha256_buffer(json, json_len, hash);
    if (apr_failed(&e)) return e;

    e = verify_ecdsa(pubkey, hash, sig, sig_len);
    if (apr_failed(&e)) return e;

    return parse_manifest((const char *)json, json_len, out);
}

AprErr apr_update_verify_payload(const wchar_t *path, const AprUpdateManifest *m)
{
    uint8_t got[APR_UPDATE_SHA256_BYTES];
    AprErr  e;

    if (!path || !m) return APR_ERR(APR_E_INVALID_ARG, L"apr_update_verify_payload: NULL");

    e = apr_update_hash_file(path, got);
    if (apr_failed(&e)) return e;

    if (memcmp(got, m->sha256, APR_UPDATE_SHA256_BYTES) != 0) {
        return APR_ERR_SAY(APR_E_STATE, APR_S_ERR_UPDATE_PAYLOAD,
                           L"update: \"%ls\" does not match the hash the "
                           L"manifest signed", path);
    }
    return apr_ok();
}

/* ===========================================================================
 * Persisted state
 * ========================================================================= */

void apr_update_state_test_redirect(const wchar_t *subkey)
{
    state_lock();
    if (subkey && subkey[0]) lstrcpynW(g_state_key, subkey, 256);
    else                     g_state_key[0] = L'\0';
    state_unlock();
}

void apr_update_state_load(AprUpdateState *out)
{
    HKEY  k = NULL;
    DWORD type, cb, dw;

    if (!out) return;
    memset(out, 0, sizeof *out);
    /* RULE 3 IS OPT-OUT, so the default when nothing has been stored is ON.
     * A missing key is a first run, not a refusal. */
    out->enabled = 1;

    state_lock();
    if (RegOpenKeyExW(HKEY_CURRENT_USER, state_key(), 0, KEY_QUERY_VALUE, &k)
        == ERROR_SUCCESS) {
        cb = sizeof dw;
        if (RegQueryValueExW(k, L"Enabled", NULL, &type, (LPBYTE)&dw, &cb)
                == ERROR_SUCCESS && type == REG_DWORD) {
            out->enabled = dw ? 1 : 0;
        }
        {
            ULONGLONG q = 0;
            cb = sizeof q;
            if (RegQueryValueExW(k, L"LastCheck", NULL, &type, (LPBYTE)&q, &cb)
                    == ERROR_SUCCESS && type == REG_QWORD) {
                out->last_check_unix = (int64_t)q;
            }
        }
        cb = sizeof out->etag;
        if (RegQueryValueExW(k, L"ETag", NULL, &type, (LPBYTE)out->etag, &cb)
                != ERROR_SUCCESS || type != REG_SZ) {
            out->etag[0] = L'\0';
        }
        out->etag[APR_UPDATE_ETAG_CCH - 1] = L'\0';

        cb = sizeof out->pending;
        if (RegQueryValueExW(k, L"Pending", NULL, &type, (LPBYTE)out->pending, &cb)
                != ERROR_SUCCESS || type != REG_SZ) {
            out->pending[0] = L'\0';
        }
        out->pending[APR_UPDATE_VERSION_CCH - 1] = L'\0';
        RegCloseKey(k);
    }
    state_unlock();
}

AprErr apr_update_state_save(const AprUpdateState *st)
{
    HKEY      k = NULL;
    LSTATUS   r;
    DWORD     dw;
    ULONGLONG q;
    AprErr    e = apr_ok();

    if (!st) return APR_ERR(APR_E_INVALID_ARG, L"apr_update_state_save: NULL");

    state_lock();
    r = RegCreateKeyExW(HKEY_CURRENT_USER, state_key(), 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) {
        state_unlock();
        SetLastError((DWORD)r);
        return APR_ERR_LAST(L"update: opening the settings key");
    }

    dw = st->enabled ? 1u : 0u;
    RegSetValueExW(k, L"Enabled", 0, REG_DWORD, (const BYTE *)&dw, sizeof dw);
    q = (ULONGLONG)st->last_check_unix;
    RegSetValueExW(k, L"LastCheck", 0, REG_QWORD, (const BYTE *)&q, sizeof q);
    RegSetValueExW(k, L"ETag", 0, REG_SZ, (const BYTE *)st->etag,
                   (DWORD)((wcslen(st->etag) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Pending", 0, REG_SZ, (const BYTE *)st->pending,
                   (DWORD)((wcslen(st->pending) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    state_unlock();
    return e;
}

AprErr apr_update_state_test_erase(void)
{
    wchar_t key[256];
    LSTATUS r;

    state_lock();
    if (!g_state_key[0]) {
        state_unlock();
        /* REFUSES WITHOUT A REDIRECT IN FORCE. There is no path by which this
         * function can delete the author's real settings. */
        return APR_ERR(APR_E_STATE, L"apr_update_state_test_erase: no redirect");
    }
    lstrcpynW(key, g_state_key, 256);
    state_unlock();

    r = RegDeleteTreeW(HKEY_CURRENT_USER, key);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        SetLastError((DWORD)r);
        return APR_ERR_LAST(L"update: erasing the test settings key");
    }
    return apr_ok();
}

/* ===========================================================================
 * Cadence -- pure
 * ========================================================================= */

int apr_update_due(AprUpdateWhy why, int enabled, int64_t now_unix,
                   int64_t last_unix)
{
    int64_t interval = APR_UPDATE_INTERVAL_MS / 1000;

    /* THE OPT-OUT WINS OVER EVERYTHING, including an explicit request: the
     * menu item is greyed when checking is off, and a scripted "check now"
     * against a machine whose owner switched this off must not be a way round
     * the setting. */
    if (!enabled) return 0;

    if (why == APR_UPDATE_WHY_USER) return 1;

    /* Never checked, or a clock that has gone backwards -- a resumed VM, a
     * time-zone fix, an NTP correction. Both are "check now": refusing to
     * check because the stored time is in the future would leave a machine
     * never updating again. */
    if (last_unix <= 0) return 1;
    if (now_unix < last_unix) return 1;

    return (now_unix - last_unix) >= interval;
}

int apr_update_may_apply(int recording)
{
    return recording ? 0 : 1;
}

/* ===========================================================================
 * Paths
 * ========================================================================= */

size_t apr_update_image_path(wchar_t *buf, size_t cch)
{
    DWORD n;

    if (!buf || cch == 0) return 0;
    buf[0] = L'\0';
    n = GetModuleFileNameW(NULL, buf, (DWORD)cch);
    if (n == 0 || n >= cch) { buf[0] = L'\0'; return 0; }
    return n;
}

static size_t with_suffix(const wchar_t *image, const wchar_t *suffix,
                          wchar_t *buf, size_t cch)
{
    if (!buf || cch == 0) return 0;
    buf[0] = L'\0';
    if (!image || !image[0]) return 0;
    if (_snwprintf_s(buf, cch, _TRUNCATE, L"%ls%ls", image, suffix) < 0) {
        buf[0] = L'\0';
        return 0;
    }
    return wcslen(buf);
}

size_t apr_update_staged_path(const wchar_t *image, wchar_t *buf, size_t cch)
{
    return with_suffix(image, L".new", buf, cch);
}

size_t apr_update_backup_path(const wchar_t *image, wchar_t *buf, size_t cch)
{
    return with_suffix(image, L".old", buf, cch);
}

/* ===========================================================================
 * The check
 * ========================================================================= */

static void url_for(const wchar_t *asset, wchar_t *buf, size_t cch)
{
    _snwprintf_s(buf, cch, _TRUNCATE, L"%ls%ls", APR_UPDATE_BASE_URL, asset);
}

AprErr apr_update_check(const AprUpdateHttp *http, int64_t now_unix,
                        AprUpdateWhy why, AprUpdateResult *out)
{
    AprUpdateState    st;
    AprUpdateResponse rsp;
    AprUpdateResponse sig_rsp;
    static char       body[APR_UPDATE_MANIFEST_MAX];
    static BYTE       sig[512];
    wchar_t           url[512];
    AprErr            e;

    if (!out) return APR_ERR(APR_E_INVALID_ARG, L"apr_update_check: NULL out");
    memset(out, 0, sizeof *out);
    out->err = apr_ok();
    out->why = why;

    if (!http) http = apr_update_http_winhttp();
    if (!http || !http->get) {
        return APR_ERR(APR_E_INVALID_ARG, L"apr_update_check: no transport");
    }

    apr_update_state_load(&st);

    if (!st.enabled) {
        out->outcome = APR_UPDATE_DISABLED;
        return apr_ok();
    }
    if (!have_trusted_key()) {
        /* See update_key.c. Quiet, once, in the log -- there is nothing the
         * user can do about it and nothing to announce. */
        APR_INFO(L"update: no release key is compiled in; not checking");
        out->outcome = APR_UPDATE_NONE;
        return apr_ok();
    }
    if (!apr_update_due(why, st.enabled, now_unix, st.last_check_unix)) {
        out->outcome = APR_UPDATE_NONE;
        return apr_ok();
    }

    url_for(APR_UPDATE_MANIFEST_NAME, url, 512);
    memset(&rsp, 0, sizeof rsp);
    e = http->get(http->user, url, st.etag, body, sizeof body, &rsp);
    if (apr_failed(&e)) {
        /* Offline, DNS, TLS, a proxy. NOT an error worth telling anyone
         * about, and the timestamp does NOT advance: no check happened. */
        APR_INFO(L"update: check could not reach the server; will try again later");
        out->err = e;
        out->outcome = APR_UPDATE_NONE;
        return apr_ok();
    }

    /* The server answered. That is what advances the clock -- see note 3. */
    st.last_check_unix = now_unix;

    if (rsp.status == 304) {
        (void)apr_update_state_save(&st);
        out->outcome = APR_UPDATE_UP_TO_DATE;
        return apr_ok();
    }
    if (rsp.status != 200 || rsp.len == 0) {
        APR_INFO(L"update: release.json answered %d (%zu bytes); nothing to do",
                 rsp.status, rsp.len);
        (void)apr_update_state_save(&st);
        out->outcome = APR_UPDATE_NONE;
        return apr_ok();
    }

    url_for(APR_UPDATE_SIG_NAME, url, 512);
    memset(&sig_rsp, 0, sizeof sig_rsp);
    e = http->get(http->user, url, NULL, sig, sizeof sig, &sig_rsp);
    if (apr_failed(&e) || sig_rsp.status != 200 ||
        sig_rsp.len != APR_UPDATE_SIG_BYTES) {
        /* THE HALF-UPLOADED RELEASE (update.h section 2). No signature is not
         * a pass and it is not an alarm either: it is the shape of a release
         * still being published. Quiet, and try again at the next tick. */
        APR_INFO(L"update: no usable signature yet (status %d, %zu bytes); "
                 L"not trusting this release", sig_rsp.status, sig_rsp.len);
        (void)apr_update_state_save(&st);
        out->outcome = APR_UPDATE_NONE;
        return apr_ok();
    }

    e = apr_update_verify_manifest(body, rsp.len, sig, sig_rsp.len,
                                   trusted_key(), &out->manifest);
    if (apr_failed(&e)) {
        /* LOUD. The ETag is deliberately NOT stored (note 2), so the next
         * check asks again in full rather than being told 304 for ever. */
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        (void)apr_update_state_save(&st);
        out->err = e;
        out->outcome = APR_UPDATE_REFUSED;
        return apr_ok();
    }

    /* Verified. Only now is it safe to remember what we saw. */
    lstrcpynW(st.etag, rsp.etag, APR_UPDATE_ETAG_CCH);
    (void)apr_update_state_save(&st);

    if (apr_version_is_newer_than_current(out->manifest.version)) {
        APR_INFO(L"update: %ls is available (running %ls)",
                 out->manifest.version, APR_VERSION_STRING);
        out->outcome = APR_UPDATE_AVAILABLE;
    } else {
        out->outcome = APR_UPDATE_UP_TO_DATE;
    }
    return apr_ok();
}

typedef struct AsyncCtx {
    const AprUpdateHttp *http;
    int64_t              now_unix;
    AprUpdateWhy         why;
    AprUpdateDoneFn      done;
    void                *user;
} AsyncCtx;

static DWORD WINAPI check_thread(LPVOID param)
{
    AsyncCtx       *cx = (AsyncCtx *)param;
    AprUpdateResult r;
    AprErr          e;

    memset(&r, 0, sizeof r);
    e = apr_update_check(cx->http, cx->now_unix, cx->why, &r);
    if (apr_failed(&e)) {
        r.outcome = APR_UPDATE_NONE;
        r.err = e;
    }
    if (cx->done) cx->done(cx->user, &r);
    free(cx);
    return 0;
}

AprErr apr_update_check_async(const AprUpdateHttp *http, int64_t now_unix,
                              AprUpdateWhy why, AprUpdateDoneFn done,
                              void *user)
{
    AsyncCtx *cx;
    HANDLE    th;

    cx = (AsyncCtx *)calloc(1, sizeof *cx);
    if (!cx) return APR_ERR(APR_E_NO_MEMORY, L"update: check context");
    cx->http = http;
    cx->now_unix = now_unix;
    cx->why = why;
    cx->done = done;
    cx->user = user;

    th = CreateThread(NULL, 0, check_thread, cx, 0, NULL);
    if (!th) {
        AprErr e = APR_ERR_LAST(L"update: starting the check thread");
        free(cx);
        return e;
    }
    /* Detached on purpose. Nothing waits for a check: it either produces a
     * notice or it does not, and a front end closing must not block on a
     * socket timeout. */
    CloseHandle(th);
    return apr_ok();
}

/* ===========================================================================
 * Staging and replacement
 * ========================================================================= */

AprErr apr_update_stage(const AprUpdateHttp *http, const wchar_t *image,
                        const AprUpdateManifest *m)
{
    wchar_t staged[UPD_PATH_CCH];
    wchar_t url[512];
    AprErr  e;

    if (!image || !m) return APR_ERR(APR_E_INVALID_ARG, L"apr_update_stage: NULL");
    if (!http) http = apr_update_http_winhttp();
    if (!http || !http->download)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_update_stage: no transport");

    if (!apr_update_staged_path(image, staged, UPD_PATH_CCH))
        return APR_ERR(APR_E_INVALID_ARG, L"apr_update_stage: no staged path");

    url_for(m->asset, url, 512);
    e = http->download(http->user, url, staged);
    if (apr_failed(&e)) {
        DeleteFileW(staged);
        return e;
    }

    e = apr_update_verify_payload(staged, m);
    if (apr_failed(&e)) {
        /* A file whose only known property is "wrong" does not get to sit
         * beside the executable. */
        DeleteFileW(staged);
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        return e;
    }
    APR_INFO(L"update: staged %ls at \"%ls\"", m->version, staged);
    return apr_ok();
}

AprErr apr_update_swap(const wchar_t *image, const wchar_t *version)
{
    wchar_t        staged[UPD_PATH_CCH];
    wchar_t        backup[UPD_PATH_CCH];
    AprUpdateState st;

    if (!image || !version)
        return APR_ERR(APR_E_INVALID_ARG, L"apr_update_swap: NULL");
    if (!apr_update_staged_path(image, staged, UPD_PATH_CCH) ||
        !apr_update_backup_path(image, backup, UPD_PATH_CCH))
        return APR_ERR(APR_E_INVALID_ARG, L"apr_update_swap: no path");

    if (GetFileAttributesW(staged) == INVALID_FILE_ATTRIBUTES)
        return APR_ERR_SAY(APR_E_NOT_FOUND, APR_S_ERR_UPDATE_SWAP,
                           L"update: nothing staged at \"%ls\"", staged);

    /* Windows forbids writing to or deleting a running image; it permits
     * RENAMING one. This is the whole trick, and it is why there is no second
     * process anywhere in this feature. */
    if (!MoveFileExW(image, backup, MOVEFILE_REPLACE_EXISTING)) {
        AprErr e = APR_ERR_LAST_SAY(APR_S_ERR_UPDATE_SWAP,
                                    L"update: moving \"%ls\" aside", image);
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        return e;
    }
    if (!MoveFileExW(staged, image, 0)) {
        AprErr e = APR_ERR_LAST_SAY(APR_S_ERR_UPDATE_SWAP,
                                    L"update: putting \"%ls\" in place", staged);
        /* BOTH RENAMES OR NEITHER. A machine left with no apprecorder.exe is
         * the one outcome worse than not updating, so the first move is
         * undone before this returns. */
        if (!MoveFileExW(backup, image, MOVEFILE_REPLACE_EXISTING)) {
            APR_ERROR(L"update: could not put \"%ls\" back -- it is at \"%ls\"",
                      image, backup);
        }
        APR_LOG_ERR(APR_LOG_ERROR, &e);
        return e;
    }

    /* Recorded BEFORE anything else can happen, because this marker is what
     * the next start uses to decide whether the swap worked. */
    apr_update_state_load(&st);
    lstrcpynW(st.pending, version, APR_UPDATE_VERSION_CCH);
    (void)apr_update_state_save(&st);

    APR_INFO(L"update: swapped in %ls; previous image kept at \"%ls\"",
             version, backup);
    return apr_ok();
}

AprUpdateStartupAction apr_update_startup_action(const wchar_t *pending,
                                                 const wchar_t *running)
{
    AprVersion p, r;

    if (!pending || !pending[0]) return APR_UPDATE_STARTUP_NOTHING;
    if (!running  || !running[0]) return APR_UPDATE_STARTUP_KEEP_BACKUP;

    /* Compared as VERSIONS, not as strings, so "v0.0.2" recorded from a tag
     * and "0.0.2" compiled in are the same build -- which they are. */
    if (!apr_version_parse(pending, &p) || !apr_version_parse(running, &r))
        return APR_UPDATE_STARTUP_KEEP_BACKUP;

    if (apr_version_compare(&p, &r) == 0) return APR_UPDATE_STARTUP_RETIRE_BACKUP;

    /* A swap was recorded and something else is running: the rename did not
     * take, or a human put the old file back. Either way the backup is the
     * thing that got them here and it stays. */
    return APR_UPDATE_STARTUP_KEEP_BACKUP;
}

void apr_update_startup(const wchar_t *image)
{
    AprUpdateState         st;
    AprUpdateStartupAction act;
    wchar_t                backup[UPD_PATH_CCH];
    wchar_t                staged[UPD_PATH_CCH];

    if (!image || !image[0]) return;

    /* A staged file still sitting here means the last exit did not swap it in
     * -- the user said "later", or the process died. It is stale by
     * definition (the next check re-downloads whatever is current), and an
     * unverified-by-this-run executable beside the real one is not something
     * to leave lying about. */
    if (apr_update_staged_path(image, staged, UPD_PATH_CCH)) {
        if (GetFileAttributesW(staged) != INVALID_FILE_ATTRIBUTES) {
            if (DeleteFileW(staged)) {
                APR_INFO(L"update: removed a stale staged build at \"%ls\"", staged);
            }
        }
    }

    apr_update_state_load(&st);
    act = apr_update_startup_action(st.pending, APR_VERSION_STRING);

    if (act == APR_UPDATE_STARTUP_RETIRE_BACKUP) {
        if (apr_update_backup_path(image, backup, UPD_PATH_CCH)) {
            DeleteFileW(backup);
        }
        APR_INFO(L"update: %ls started successfully; the previous image has "
                 L"been retired", st.pending);
        st.pending[0] = L'\0';
        (void)apr_update_state_save(&st);
    } else if (act == APR_UPDATE_STARTUP_KEEP_BACKUP) {
        /* Kept, and said out loud in the log, because this is the state
         * somebody recovers from by hand. */
        APR_WARN(L"update: %ls was swapped in but %ls is running; the previous "
                 L"image is being kept", st.pending, APR_VERSION_STRING);
    }
}
