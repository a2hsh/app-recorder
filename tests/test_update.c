/*
 * test_update.c -- the updater: its cryptography, its cadence, its settings,
 * and the file moves that replace a running executable.
 *
 * ===========================================================================
 * THE TWO CASES THIS FILE EXISTS FOR
 *
 *   a_tampered_manifest_is_refused
 *   a_tampered_binary_is_refused
 *
 * Everything else here supports those two. The claim the whole feature rests
 * on is that a compromised GitHub account can fail to serve a good build and
 * can do nothing else, and that claim is worth exactly as much as the evidence
 * for it -- so both are demonstrated by producing a signature that IS good,
 * breaking one byte, and watching the refusal, rather than asserted in a
 * comment.
 *
 * ===========================================================================
 * WHERE THE KEY COMES FROM
 *
 *   BCrypt, here, per case. The release private key is not in this repository
 *   and never will be (update_key.c), so a suite that could only verify would
 *   be reduced to checking that garbage fails -- which passes just as well
 *   against a verifier that rejects everything, including real releases.
 *   Generating a throwaway P-256 pair and signing with it is what makes the
 *   POSITIVE case real, and only then is breaking it meaningful.
 *
 * ===========================================================================
 * NOTHING HERE TOUCHES THE NETWORK, THE CLOCK, OR THE AUTHOR'S SETTINGS
 *
 *   - The network: every request goes through a fake AprUpdateHttp. The real
 *     WinHTTP one lives in its own translation unit and is never called from
 *     this file (AGENTS.md rule 1, and a suite that reached github.com would
 *     fail on an aeroplane).
 *   - The clock: `now` is a parameter to apr_update_due() and to
 *     apr_update_check(), so the five-minute cadence is a TABLE and not a
 *     wait. There is not one Sleep in this file.
 *   - The settings: apr_update_state_test_redirect() points the module at a
 *     per-process subkey, which is deleted at the end of every case that uses
 *     it. HKCU\Software\apprecorder\Update is never opened.
 *   - The disk: the staging and swap cases work inside a fresh directory under
 *     %TEMP% on files this suite created, and remove it afterwards. The
 *     running test executable is never a candidate for renaming.
 */
#include "test_runner.h"

#include <windows.h>
#include <bcrypt.h>

#include "errmsg.h"
#include "strings.h"
#include "update.h"
#include "version.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

/* ===========================================================================
 * A throwaway signing key, and a signature over arbitrary bytes
 * ========================================================================= */

typedef struct TestKey {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_KEY_HANDLE key;
    uint8_t           pub[APR_UPDATE_PUBKEY_BYTES];   /* X||Y */
} TestKey;

static int key_make(TestKey *k)
{
    BYTE     blob[sizeof(BCRYPT_ECCKEY_BLOB) + APR_UPDATE_PUBKEY_BYTES];
    ULONG    got = 0;
    NTSTATUS st;

    memset(k, 0, sizeof *k);
    st = BCryptOpenAlgorithmProvider(&k->alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    if (st != STATUS_SUCCESS) return 0;
    st = BCryptGenerateKeyPair(k->alg, &k->key, 256, 0);
    if (st != STATUS_SUCCESS) return 0;
    st = BCryptFinalizeKeyPair(k->key, 0);
    if (st != STATUS_SUCCESS) return 0;
    st = BCryptExportKey(k->key, NULL, BCRYPT_ECCPUBLIC_BLOB, blob,
                         (ULONG)sizeof blob, &got, 0);
    if (st != STATUS_SUCCESS || got != sizeof blob) return 0;
    memcpy(k->pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), APR_UPDATE_PUBKEY_BYTES);
    return 1;
}

static void key_free(TestKey *k)
{
    if (k->key) BCryptDestroyKey(k->key);
    if (k->alg) BCryptCloseAlgorithmProvider(k->alg, 0);
    memset(k, 0, sizeof *k);
}

static int sha256(const void *data, size_t len, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    NTSTATUS           st;
    int                ok = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)
        != STATUS_SUCCESS) return 0;
    st = BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    if (st == STATUS_SUCCESS)
        st = BCryptHashData(h, (PUCHAR)(void *)data, (ULONG)len, 0);
    if (st == STATUS_SUCCESS)
        st = BCryptFinishHash(h, out, 32, 0);
    ok = (st == STATUS_SUCCESS);
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static int key_sign(TestKey *k, const void *data, size_t len,
                    uint8_t sig[APR_UPDATE_SIG_BYTES])
{
    uint8_t  hash[32];
    ULONG    got = 0;
    NTSTATUS st;

    if (!sha256(data, len, hash)) return 0;
    st = BCryptSignHash(k->key, NULL, hash, 32, sig, APR_UPDATE_SIG_BYTES,
                        &got, 0);
    return st == STATUS_SUCCESS && got == APR_UPDATE_SIG_BYTES;
}

/* ===========================================================================
 * A manifest, as bytes
 * ========================================================================= */

static const char k_manifest[] =
    "{\n"
    "  \"version\": \"0.0.2\",\n"
    "  \"asset\": \"apprecorder.exe\",\n"
    "  \"sha256\": \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\n"
    "  \"size\": 1013760,\n"
    "  \"notes\": \"Nothing much.\"\n"
    "}\n";

/* The SHA-256 of the empty payload the staging cases write, spelled out so the
 * manifest and the file agree by construction. */
static void hex_of(const uint8_t h[32], char out[65])
{
    static const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2]     = d[h[i] >> 4];
        out[i * 2 + 1] = d[h[i] & 15];
    }
    out[64] = '\0';
}

/* ===========================================================================
 * Verification -- the part that matters
 * ========================================================================= */

TEST(a_genuine_manifest_verifies_and_parses)
{
    TestKey k;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    AprUpdateManifest m;
    AprErr e;

    ASSERT_TRUE(key_make(&k));
    ASSERT_TRUE(key_sign(&k, k_manifest, sizeof k_manifest - 1, sig));

    e = apr_update_verify_manifest(k_manifest, sizeof k_manifest - 1,
                                   sig, sizeof sig, k.pub, &m);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"0.0.2", m.version);
    ASSERT_WSTR_EQ(L"apprecorder.exe", m.asset);
    ASSERT_WSTR_EQ(L"Nothing much.", m.notes);
    ASSERT_EQ_U64(1013760u, m.size);
    ASSERT_EQ_INT(0xe3, m.sha256[0]);
    ASSERT_EQ_INT(0x55, m.sha256[31]);

    key_free(&k);
}

/* THE FIRST OF THE TWO CASES THIS FILE EXISTS FOR.
 *
 * A compromised host that rewrote the manifest -- to point at a different
 * hash, to claim a version, to change the notes -- must be refused. Every byte
 * position is exercised rather than one convenient one, because a verifier
 * that checked only a prefix would pass a single-byte test at offset 3. */
TEST(a_tampered_manifest_is_refused)
{
    TestKey k;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    char    bad[sizeof k_manifest];
    AprUpdateManifest m;
    AprErr  e;
    size_t  n = sizeof k_manifest - 1;
    size_t  i;
    int     accepted = 0;

    ASSERT_TRUE(key_make(&k));
    ASSERT_TRUE(key_sign(&k, k_manifest, n, sig));

    for (i = 0; i < n; i++) {
        memcpy(bad, k_manifest, sizeof k_manifest);
        bad[i] = (char)(bad[i] ^ 0x20);   /* one bit, in one byte */
        e = apr_update_verify_manifest(bad, n, sig, sizeof sig, k.pub, &m);
        if (!apr_failed(&e)) {
            printf("      byte %zu could be changed without the signature "
                   "noticing\n", i);
            accepted++;
        }
        /* And nothing partial is left behind for a caller to act on. */
        if (!apr_failed(&e)) break;
        ASSERT_EQ_INT(0, (int)m.version[0]);
    }
    ASSERT_EQ_INT(0, accepted);

    key_free(&k);
}

TEST(a_manifest_signed_with_another_key_is_refused)
{
    TestKey mine, theirs;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    AprUpdateManifest m;
    AprErr e;

    /* The exact shape of a stolen GitHub account: a perfectly well-formed
     * release, signed by somebody who is not the author. */
    ASSERT_TRUE(key_make(&mine));
    ASSERT_TRUE(key_make(&theirs));
    ASSERT_TRUE(key_sign(&theirs, k_manifest, sizeof k_manifest - 1, sig));

    e = apr_update_verify_manifest(k_manifest, sizeof k_manifest - 1,
                                   sig, sizeof sig, mine.pub, &m);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_S_ERR_UPDATE_SIGNATURE, (int)apr_err_reason_id(&e));

    key_free(&mine);
    key_free(&theirs);
}

TEST(a_signature_of_the_wrong_length_is_refused_not_ignored)
{
    TestKey k;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    AprUpdateManifest m;
    AprErr e;

    ASSERT_TRUE(key_make(&k));
    ASSERT_TRUE(key_sign(&k, k_manifest, sizeof k_manifest - 1, sig));

    /* THE HALF-UPLOADED RELEASE (update.h section 2). A truncated or absent
     * signature is not a pass. */
    e = apr_update_verify_manifest(k_manifest, sizeof k_manifest - 1,
                                   sig, 0, k.pub, &m);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_S_ERR_UPDATE_SIGNATURE, (int)apr_err_reason_id(&e));

    e = apr_update_verify_manifest(k_manifest, sizeof k_manifest - 1,
                                   sig, 32, k.pub, &m);
    ASSERT_TRUE(apr_failed(&e));

    e = apr_update_verify_manifest(k_manifest, sizeof k_manifest - 1,
                                   sig, 63, k.pub, &m);
    ASSERT_TRUE(apr_failed(&e));

    key_free(&k);
}

TEST(an_empty_or_oversized_manifest_is_refused)
{
    TestKey k;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    AprUpdateManifest m;
    AprErr e;

    ASSERT_TRUE(key_make(&k));
    ASSERT_TRUE(key_sign(&k, k_manifest, sizeof k_manifest - 1, sig));

    e = apr_update_verify_manifest(k_manifest, 0, sig, sizeof sig, k.pub, &m);
    ASSERT_TRUE(apr_failed(&e));

    e = apr_update_verify_manifest(k_manifest, APR_UPDATE_MANIFEST_MAX + 1,
                                   sig, sizeof sig, k.pub, &m);
    ASSERT_TRUE(apr_failed(&e));

    key_free(&k);
}

/* A SIGNED DOCUMENT CAN STILL BE A BAD ONE. These all carry a genuine
 * signature -- the author's own key over the author's own mistake -- and each
 * must be refused on its content rather than waved through because the
 * cryptography was fine. */
static void refuse_signed(const char *json)
{
    TestKey k;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    AprUpdateManifest m;
    AprErr e;
    size_t n = strlen(json);

    if (!key_make(&k)) { FAIL("could not make a key"); }
    if (!key_sign(&k, json, n, sig)) { key_free(&k); FAIL("could not sign"); }

    e = apr_update_verify_manifest(json, n, sig, sizeof sig, k.pub, &m);
    if (!apr_failed(&e)) {
        printf("      accepted: %s\n", json);
        tr_fail_head(__FILE__, __LINE__, "refuse_signed");
    }
    key_free(&k);
}

TEST(a_signed_but_malformed_manifest_is_still_refused)
{
    refuse_signed("not json at all");
    refuse_signed("[]");
    refuse_signed("{}");
    /* Missing sha256: not "a manifest with an empty hash". */
    refuse_signed("{\"version\":\"0.0.2\",\"asset\":\"apprecorder.exe\"}");
    /* Missing version. */
    refuse_signed("{\"asset\":\"a.exe\",\"sha256\":\""
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}");
    /* A hash of the wrong length, and one that is not hex. */
    refuse_signed("{\"version\":\"0.0.2\",\"asset\":\"a.exe\",\"sha256\":\"abcd\"}");
    refuse_signed("{\"version\":\"0.0.2\",\"asset\":\"a.exe\",\"sha256\":\""
                  "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"}");
    /* A version this build cannot order against its own. */
    refuse_signed("{\"version\":\"latest\",\"asset\":\"a.exe\",\"sha256\":\""
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}");
    /* AN ASSET NAME THAT IS A PATH. It is appended to a URL and used to build
     * a file name beside the executable. */
    refuse_signed("{\"version\":\"0.0.2\",\"asset\":\"../../evil.exe\",\"sha256\":\""
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}");
    refuse_signed("{\"version\":\"0.0.2\",\"asset\":\"C:\\\\evil.exe\",\"sha256\":\""
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}");
    ASSERT_EQ_INT(0, 0);
}

TEST(an_unknown_key_in_a_manifest_is_ignored_so_a_later_release_still_installs)
{
    TestKey k;
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    AprUpdateManifest m;
    AprErr e;
    static const char json[] =
        "{\"version\":\"0.0.3\",\"asset\":\"apprecorder.exe\",\"sha256\":\""
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
        "\"somethingNewer\":\"a setting from the future\"}";

    ASSERT_TRUE(key_make(&k));
    ASSERT_TRUE(key_sign(&k, json, sizeof json - 1, sig));
    e = apr_update_verify_manifest(json, sizeof json - 1, sig, sizeof sig,
                                   k.pub, &m);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"0.0.3", m.version);
    key_free(&k);
}

/* ===========================================================================
 * Files, and the second of the two cases this file exists for
 * ========================================================================= */

/* A COUNTER, NOT A TICK COUNT. Two cases in this file legitimately run inside
 * the same millisecond, so GetTickCount() handed them the same directory --
 * and the second then saw files the first had left behind after an early
 * ASSERT return. It surfaced in a red run and would have been an occasional
 * flake in a green one, which is the worse of the two. */
static long g_temp_seq;

static void temp_dir(wchar_t *out, size_t cch)
{
    wchar_t base[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, base);
    if (n == 0) base[0] = L'\0';
    _snwprintf_s(out, cch, _TRUNCATE, L"%lsapr_upd_%lu_%ld", base,
                 (unsigned long)GetCurrentProcessId(),
                 InterlockedIncrement(&g_temp_seq));
    CreateDirectoryW(out, NULL);
}

static void wipe_dir(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH * 2];
    HANDLE  h;

    _snwprintf_s(pattern, MAX_PATH * 2, _TRUNCATE, L"%ls\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAX_PATH * 2];
            if (fd.cFileName[0] == L'.') continue;
            _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int write_file(const wchar_t *path, const void *data, size_t n)
{
    DWORD  wrote = 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (n && !WriteFile(h, data, (DWORD)n, &wrote, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    return wrote == (DWORD)n;
}

static int read_all(const wchar_t *path, char *buf, size_t cap, size_t *len)
{
    DWORD  got = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, (DWORD)cap, &got, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    *len = got;
    return 1;
}

TEST(hashing_a_file_matches_hashing_its_bytes)
{
    wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2];
    static const char payload[] = "MZ this is not really an executable";
    uint8_t a[32], b[32];
    AprErr  e;

    temp_dir(dir, MAX_PATH * 2);
    _snwprintf_s(path, MAX_PATH * 2, _TRUNCATE, L"%ls\\payload.bin", dir);
    ASSERT_TRUE(write_file(path, payload, sizeof payload - 1));

    ASSERT_TRUE(sha256(payload, sizeof payload - 1, a));
    e = apr_update_hash_file(path, b);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_MEM_EQ(a, b, 32);

    /* And a file that is not there is a failure, not a zero hash. */
    e = apr_update_hash_file(L"Z:\\nothing\\here.bin", b);
    ASSERT_TRUE(apr_failed(&e));

    wipe_dir(dir);
}

/* THE SECOND OF THE TWO CASES THIS FILE EXISTS FOR.
 *
 * The manifest is genuine and its signature verifies. The BINARY beside it is
 * not the one the author hashed -- a host that swapped the asset, a proxy that
 * rewrote it, a truncated download. The hash inside the signed document is
 * what catches it, which is the whole reason it is inside the signed document
 * and not beside it. */
TEST(a_tampered_binary_is_refused)
{
    wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2];
    static const char good[] = "the build the author actually made";
    static const char bad[]  = "the build somebody else made";
    AprUpdateManifest m;
    uint8_t h[32];
    AprErr  e;

    memset(&m, 0, sizeof m);
    ASSERT_TRUE(sha256(good, sizeof good - 1, h));
    memcpy(m.sha256, h, 32);

    temp_dir(dir, MAX_PATH * 2);
    _snwprintf_s(path, MAX_PATH * 2, _TRUNCATE, L"%ls\\apprecorder.exe", dir);

    /* The honest file passes. Without this half the case would also pass
     * against a verifier that refuses everything. */
    ASSERT_TRUE(write_file(path, good, sizeof good - 1));
    e = apr_update_verify_payload(path, &m);
    ASSERT_FALSE(apr_failed(&e));

    /* One byte is enough. */
    ASSERT_TRUE(write_file(path, bad, sizeof bad - 1));
    e = apr_update_verify_payload(path, &m);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_S_ERR_UPDATE_PAYLOAD, (int)apr_err_reason_id(&e));

    {
        char one[sizeof good];
        memcpy(one, good, sizeof good);
        one[0] = (char)(one[0] ^ 1);
        ASSERT_TRUE(write_file(path, one, sizeof good - 1));
        e = apr_update_verify_payload(path, &m);
        ASSERT_TRUE(apr_failed(&e));
        ASSERT_EQ_INT(APR_S_ERR_UPDATE_PAYLOAD, (int)apr_err_reason_id(&e));
    }

    wipe_dir(dir);
}

/* ===========================================================================
 * Cadence -- a table, because apr_update_due() is pure
 * ========================================================================= */

#define FIVE_MIN (APR_UPDATE_INTERVAL_MS / 1000)

TEST(the_opt_out_stops_every_check_including_one_the_user_asked_for)
{
    /* Rule 3. A disabled check stays disabled: "check now" is a way to ask
     * sooner, not a way round the setting. */
    ASSERT_FALSE(apr_update_due(APR_UPDATE_WHY_STARTUP, 0, 100000, 0));
    ASSERT_FALSE(apr_update_due(APR_UPDATE_WHY_TIMER, 0, 100000, 0));
    ASSERT_FALSE(apr_update_due(APR_UPDATE_WHY_RECORDING_STOPPED, 0, 100000, 0));
    ASSERT_FALSE(apr_update_due(APR_UPDATE_WHY_USER, 0, 100000, 0));
}

TEST(a_user_request_ignores_the_interval)
{
    /* One second after the last check. Somebody who presses the menu item is
     * owed an answer. */
    ASSERT_TRUE(apr_update_due(APR_UPDATE_WHY_USER, 1, 100001, 100000));
}

TEST(five_minutes_gates_startup_the_timer_and_the_end_of_a_recording)
{
    AprUpdateWhy whys[3];
    int i;

    whys[0] = APR_UPDATE_WHY_STARTUP;
    whys[1] = APR_UPDATE_WHY_TIMER;
    whys[2] = APR_UPDATE_WHY_RECORDING_STOPPED;

    for (i = 0; i < 3; i++) {
        /* Never checked. */
        ASSERT_TRUE(apr_update_due(whys[i], 1, 100000, 0));
        /* Just checked -- this is the case that stops start/stop cycling
         * turning ten takes into ten requests, and the case that stops a
         * restart loop being a request storm. */
        ASSERT_FALSE(apr_update_due(whys[i], 1, 100000, 100000));
        ASSERT_FALSE(apr_update_due(whys[i], 1, 100000 + FIVE_MIN - 1, 100000));
        /* Exactly five minutes is due. */
        ASSERT_TRUE(apr_update_due(whys[i], 1, 100000 + FIVE_MIN, 100000));
        ASSERT_TRUE(apr_update_due(whys[i], 1, 100000 + FIVE_MIN * 10, 100000));
    }
}

TEST(a_clock_that_went_backwards_does_not_wedge_the_check_for_ever)
{
    /* A resumed VM, an NTP correction, a time-zone fix. A stored timestamp in
     * the future would otherwise mean "never check again". */
    ASSERT_TRUE(apr_update_due(APR_UPDATE_WHY_TIMER, 1, 100, 999999));
}

TEST(nothing_is_applied_while_a_recording_is_running)
{
    /* Rule 1. The refusal is a separate question from whether to CHECK,
     * because checking during a recording costs nothing and swapping the
     * image under one is unrepeatable. */
    ASSERT_FALSE(apr_update_may_apply(1));
    ASSERT_TRUE(apr_update_may_apply(0));
    ASSERT_TRUE(apr_update_due(APR_UPDATE_WHY_TIMER, 1, 100000, 0));
}

/* ===========================================================================
 * Persisted settings
 * ========================================================================= */

static void use_test_settings(void)
{
    wchar_t key[128];
    _snwprintf_s(key, 128, _TRUNCATE,
                 L"Software\\apprecorder-test-%lu\\Update",
                 (unsigned long)GetCurrentProcessId());
    apr_update_state_test_redirect(key);
}

static void drop_test_settings(void)
{
    (void)apr_update_state_test_erase();
    apr_update_state_test_redirect(NULL);
}

TEST(a_first_run_has_checking_switched_on_and_has_never_checked)
{
    AprUpdateState st;

    use_test_settings();
    (void)apr_update_state_test_erase();

    apr_update_state_load(&st);
    /* OPT-OUT, not opt-in: a missing key is a first run, not a refusal. */
    ASSERT_EQ_INT(1, st.enabled);
    ASSERT_EQ_INT(0, (int)st.last_check_unix);
    ASSERT_EQ_INT(0, (int)st.etag[0]);
    ASSERT_EQ_INT(0, (int)st.pending[0]);

    drop_test_settings();
}

TEST(the_opt_out_and_the_last_check_time_survive_a_restart)
{
    AprUpdateState st;
    AprErr e;

    use_test_settings();
    (void)apr_update_state_test_erase();

    apr_update_state_load(&st);
    st.enabled = 0;
    st.last_check_unix = 1755000000;
    lstrcpynW(st.etag, L"\"abc123\"", APR_UPDATE_ETAG_CCH);
    lstrcpynW(st.pending, L"0.0.9", APR_UPDATE_VERSION_CCH);
    e = apr_update_state_save(&st);
    ASSERT_FALSE(apr_failed(&e));

    /* "A restart" is exactly a fresh load: nothing is cached in memory. */
    memset(&st, 0xAB, sizeof st);
    apr_update_state_load(&st);
    ASSERT_EQ_INT(0, st.enabled);
    ASSERT_EQ_U64(1755000000u, (unsigned long long)st.last_check_unix);
    ASSERT_WSTR_EQ(L"\"abc123\"", st.etag);
    ASSERT_WSTR_EQ(L"0.0.9", st.pending);

    drop_test_settings();
}

TEST(erasing_the_settings_refuses_without_a_redirect_in_force)
{
    AprErr e;

    /* There must be no path by which a test can delete the author's real
     * settings, so the erase is refused rather than merely discouraged. */
    apr_update_state_test_redirect(NULL);
    e = apr_update_state_test_erase();
    ASSERT_TRUE(apr_failed(&e));
}

/* ===========================================================================
 * The check, end to end, through a fake transport
 * ========================================================================= */

typedef struct Fake {
    const char *manifest;
    size_t      manifest_len;
    int         manifest_status;
    const uint8_t *sig;
    size_t      sig_len;
    int         sig_status;
    const wchar_t *etag;
    int         fail_transport;      /* offline: a failed AprErr, not a status */
    int         manifest_gets;
    int         sig_gets;
    int         downloads;
    const char *payload;             /* what download() writes */
    size_t      payload_len;
} Fake;

static int is_manifest_url(const wchar_t *url)
{
    return wcsstr(url, APR_UPDATE_MANIFEST_NAME) != NULL &&
           wcsstr(url, APR_UPDATE_SIG_NAME) == NULL;
}

static AprErr fake_get(void *user, const wchar_t *url, const wchar_t *etag_in,
                       void *body, size_t cap, AprUpdateResponse *out)
{
    Fake *f = (Fake *)user;

    memset(out, 0, sizeof *out);
    if (f->fail_transport)
        return APR_ERR(APR_E_IO, L"test: pretending to be offline");

    if (is_manifest_url(url)) {
        f->manifest_gets++;
        /* THE CONDITIONAL REQUEST. A stored ETag that the server considers
         * current answers 304 with no body, which is what makes a five-minute
         * cadence defensible. */
        if (etag_in && etag_in[0] && f->etag && wcscmp(etag_in, f->etag) == 0) {
            out->status = 304;
            return apr_ok();
        }
        out->status = f->manifest_status;
        if (out->status == 200 && f->manifest_len <= cap) {
            memcpy(body, f->manifest, f->manifest_len);
            out->len = f->manifest_len;
        }
        if (f->etag) lstrcpynW(out->etag, f->etag, APR_UPDATE_ETAG_CCH);
        return apr_ok();
    }

    f->sig_gets++;
    out->status = f->sig_status;
    if (out->status == 200 && f->sig && f->sig_len <= cap) {
        memcpy(body, f->sig, f->sig_len);
        out->len = f->sig_len;
    }
    return apr_ok();
}

static AprErr fake_download(void *user, const wchar_t *url, const wchar_t *dest)
{
    Fake *f = (Fake *)user;
    (void)url;
    f->downloads++;
    if (!write_file(dest, f->payload, f->payload_len))
        return APR_ERR(APR_E_IO, L"test: could not write the fake payload");
    return apr_ok();
}

/* A manifest naming `version`, whose sha256 is that of `payload`. */
static void build_manifest(char *out, size_t cap, const char *version,
                           const char *payload, size_t payload_len)
{
    uint8_t h[32];
    char    hex[65];

    sha256(payload, payload_len, h);
    hex_of(h, hex);
    _snprintf_s(out, cap, _TRUNCATE,
                "{\"version\":\"%s\",\"asset\":\"apprecorder.exe\","
                "\"sha256\":\"%s\",\"size\":%u,\"notes\":\"Test build.\"}",
                version, hex, (unsigned)payload_len);
}

TEST(a_newer_signed_release_is_reported_as_available)
{
    TestKey k;
    Fake    f;
    AprUpdateHttp http;
    AprUpdateResult r;
    char    json[512];
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    static const char payload[] = "a newer apprecorder";
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();
    ASSERT_TRUE(key_make(&k));
    apr_update_test_set_key(k.pub);

    build_manifest(json, sizeof json, "9.9.9", payload, sizeof payload - 1);
    ASSERT_TRUE(key_sign(&k, json, strlen(json), sig));

    memset(&f, 0, sizeof f);
    f.manifest = json;
    f.manifest_len = strlen(json);
    f.manifest_status = 200;
    f.sig = sig;
    f.sig_len = sizeof sig;
    f.sig_status = 200;
    f.etag = L"\"v1\"";
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_check(&http, 1000, APR_UPDATE_WHY_STARTUP, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_UPDATE_AVAILABLE, (int)r.outcome);
    ASSERT_WSTR_EQ(L"9.9.9", r.manifest.version);
    ASSERT_EQ_INT(APR_UPDATE_WHY_STARTUP, (int)r.why);

    /* The ETag was remembered, so the next check is a 304 -- and the
     * five-minute gate refuses the one before that. */
    {
        AprUpdateState st;
        apr_update_state_load(&st);
        ASSERT_WSTR_EQ(L"\"v1\"", st.etag);
        ASSERT_EQ_U64(1000u, (unsigned long long)st.last_check_unix);
    }

    e = apr_update_check(&http, 1001, APR_UPDATE_WHY_TIMER, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_UPDATE_NONE, (int)r.outcome);
    ASSERT_EQ_INT(1, f.manifest_gets);          /* not asked again */

    e = apr_update_check(&http, 1000 + FIVE_MIN, APR_UPDATE_WHY_TIMER, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(2, f.manifest_gets);
    ASSERT_EQ_INT(APR_UPDATE_UP_TO_DATE, (int)r.outcome);   /* 304 */
    ASSERT_EQ_INT(1, f.sig_gets);               /* a 304 fetches no signature */

    apr_update_test_set_key(NULL);
    key_free(&k);
    drop_test_settings();
}

TEST(a_forged_release_is_refused_and_does_not_silence_the_next_check)
{
    TestKey mine, theirs;
    Fake    f;
    AprUpdateHttp http;
    AprUpdateResult r;
    char    json[512];
    uint8_t sig[APR_UPDATE_SIG_BYTES];
    static const char payload[] = "not the author's build";
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();
    ASSERT_TRUE(key_make(&mine));
    ASSERT_TRUE(key_make(&theirs));
    apr_update_test_set_key(mine.pub);

    build_manifest(json, sizeof json, "9.9.9", payload, sizeof payload - 1);
    ASSERT_TRUE(key_sign(&theirs, json, strlen(json), sig));

    memset(&f, 0, sizeof f);
    f.manifest = json;
    f.manifest_len = strlen(json);
    f.manifest_status = 200;
    f.sig = sig;
    f.sig_len = sizeof sig;
    f.sig_status = 200;
    f.etag = L"\"forged\"";
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_check(&http, 1000, APR_UPDATE_WHY_STARTUP, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_UPDATE_REFUSED, (int)r.outcome);
    ASSERT_EQ_INT(APR_S_ERR_UPDATE_SIGNATURE, (int)apr_err_reason_id(&r.err));

    /* AND THE ETag WAS NOT STORED. Remembering it would mean the next check
     * gets a 304 and cheerfully reports "up to date" for ever after -- one
     * forged release silencing the refusal permanently. */
    {
        AprUpdateState st;
        apr_update_state_load(&st);
        ASSERT_EQ_INT(0, (int)st.etag[0]);
    }
    e = apr_update_check(&http, 1000 + FIVE_MIN, APR_UPDATE_WHY_TIMER, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_UPDATE_REFUSED, (int)r.outcome);
    ASSERT_EQ_INT(2, f.manifest_gets);

    apr_update_test_set_key(NULL);
    key_free(&mine);
    key_free(&theirs);
    drop_test_settings();
}

TEST(a_release_with_no_signature_yet_is_quietly_not_trusted)
{
    TestKey k;
    Fake    f;
    AprUpdateHttp http;
    AprUpdateResult r;
    char    json[512];
    static const char payload[] = "half uploaded";
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();
    ASSERT_TRUE(key_make(&k));
    apr_update_test_set_key(k.pub);

    build_manifest(json, sizeof json, "9.9.9", payload, sizeof payload - 1);

    memset(&f, 0, sizeof f);
    f.manifest = json;
    f.manifest_len = strlen(json);
    f.manifest_status = 200;
    f.sig_status = 404;              /* the .sig has not been uploaded yet */
    f.etag = L"\"partial\"";
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_check(&http, 1000, APR_UPDATE_WHY_STARTUP, &r);
    ASSERT_FALSE(apr_failed(&e));
    /* NOT a pass, and NOT an alarm: this is the shape of a release still
     * being published. */
    ASSERT_EQ_INT(APR_UPDATE_NONE, (int)r.outcome);

    /* No ETag was remembered, so the retry is a real fetch rather than a 304
     * that would report "up to date" against a release we never verified. */
    {
        AprUpdateState st;
        apr_update_state_load(&st);
        ASSERT_EQ_INT(0, (int)st.etag[0]);
        /* But the timestamp DID move: the host answered, and leaving it alone
         * would let every recording-stopped trigger re-fetch for as long as
         * the upload took. */
        ASSERT_EQ_U64(1000u, (unsigned long long)st.last_check_unix);
    }

    apr_update_test_set_key(NULL);
    key_free(&k);
    drop_test_settings();
}

TEST(being_offline_is_not_an_error_and_does_not_count_as_a_check)
{
    Fake    f;
    TestKey k;
    AprUpdateHttp http;
    AprUpdateResult r;
    AprUpdateState st;
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();
    ASSERT_TRUE(key_make(&k));
    apr_update_test_set_key(k.pub);

    memset(&f, 0, sizeof f);
    f.fail_transport = 1;
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_check(&http, 5000, APR_UPDATE_WHY_STARTUP, &r);
    /* The CALL succeeds. Only the check did not happen, and that is not
     * something to tell anybody about (update.h section 7). */
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_UPDATE_NONE, (int)r.outcome);

    /* AND AN HOUR OFFLINE MUST NOT LOOK LIKE TWELVE CHECKS. */
    apr_update_state_load(&st);
    ASSERT_EQ_INT(0, (int)st.last_check_unix);

    apr_update_test_set_key(NULL);
    key_free(&k);
    drop_test_settings();
}

TEST(a_disabled_check_opens_no_connection_at_all)
{
    Fake    f;
    TestKey k;
    AprUpdateHttp http;
    AprUpdateResult r;
    AprUpdateState st;
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();
    ASSERT_TRUE(key_make(&k));
    apr_update_test_set_key(k.pub);

    apr_update_state_load(&st);
    st.enabled = 0;
    e = apr_update_state_save(&st);
    ASSERT_FALSE(apr_failed(&e));

    memset(&f, 0, sizeof f);
    f.manifest_status = 200;
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_check(&http, 9000, APR_UPDATE_WHY_USER, &r);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_EQ_INT(APR_UPDATE_DISABLED, (int)r.outcome);
    /* THE POINT OF THE SETTING. Not "checks and discards the answer": nothing
     * left this machine. */
    ASSERT_EQ_INT(0, f.manifest_gets);
    ASSERT_EQ_INT(0, f.sig_gets);

    apr_update_test_set_key(NULL);
    key_free(&k);
    drop_test_settings();
}

TEST(a_build_with_no_release_key_does_not_look)
{
    Fake    f;
    AprUpdateHttp http;
    AprUpdateResult r;
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();
    apr_update_test_set_key(NULL);      /* i.e. the compiled-in one */

    memset(&f, 0, sizeof f);
    f.manifest_status = 200;
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_check(&http, 9000, APR_UPDATE_WHY_USER, &r);
    ASSERT_FALSE(apr_failed(&e));

    if (apr_update_have_key()) {
        /* The author has generated his keypair: the check ran, and what it
         * found is somebody else's business. */
        ASSERT_LE_INT(1, f.manifest_gets);
    } else {
        /* No key: it cannot tell a release from a forgery, so it does not
         * ask. Not "asks and refuses everything", which would announce a
         * refusal every five minutes and teach its user to ignore the one
         * sentence that matters. */
        ASSERT_EQ_INT(APR_UPDATE_NONE, (int)r.outcome);
        ASSERT_EQ_INT(0, f.manifest_gets);
    }

    drop_test_settings();
}

/* ===========================================================================
 * Staging, the swap, and the recovery window
 * ========================================================================= */

TEST(staging_deletes_a_payload_that_does_not_match_the_signed_hash)
{
    Fake    f;
    AprUpdateHttp http;
    AprUpdateManifest m;
    wchar_t dir[MAX_PATH * 2], image[MAX_PATH * 2], staged[MAX_PATH * 2];
    static const char promised[] = "the build that was signed";
    static const char served[]   = "something else entirely";
    uint8_t h[32];
    AprErr  e;

    temp_dir(dir, MAX_PATH * 2);
    _snwprintf_s(image, MAX_PATH * 2, _TRUNCATE, L"%ls\\apprecorder.exe", dir);
    ASSERT_TRUE(write_file(image, "the build in place", 18));
    ASSERT_GT_INT(0, (int)apr_update_staged_path(image, staged, MAX_PATH * 2));

    memset(&m, 0, sizeof m);
    lstrcpynW(m.version, L"9.9.9", APR_UPDATE_VERSION_CCH);
    lstrcpynW(m.asset, L"apprecorder.exe", APR_UPDATE_ASSET_CCH);
    ASSERT_TRUE(sha256(promised, sizeof promised - 1, h));
    memcpy(m.sha256, h, 32);

    memset(&f, 0, sizeof f);
    f.payload = served;                       /* not what the manifest says */
    f.payload_len = sizeof served - 1;
    http.get = fake_get;
    http.download = fake_download;
    http.user = &f;

    e = apr_update_stage(&http, image, &m);
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_EQ_INT(APR_S_ERR_UPDATE_PAYLOAD, (int)apr_err_reason_id(&e));
    /* A file whose only known property is "wrong" does not get to sit beside
     * the executable. */
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES,
                  (int)GetFileAttributesW(staged));

    /* The honest payload stages, so this is not passing because staging is
     * simply broken. */
    f.payload = promised;
    f.payload_len = sizeof promised - 1;
    e = apr_update_stage(&http, image, &m);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_NE_INT((int)INVALID_FILE_ATTRIBUTES,
                  (int)GetFileAttributesW(staged));

    wipe_dir(dir);
}

TEST(the_swap_keeps_the_previous_image_and_is_all_or_nothing)
{
    wchar_t dir[MAX_PATH * 2], image[MAX_PATH * 2];
    wchar_t staged[MAX_PATH * 2], backup[MAX_PATH * 2];
    char    buf[64];
    size_t  n = 0;
    AprErr  e;

    use_test_settings();
    (void)apr_update_state_test_erase();

    temp_dir(dir, MAX_PATH * 2);
    _snwprintf_s(image, MAX_PATH * 2, _TRUNCATE, L"%ls\\apprecorder.exe", dir);
    apr_update_staged_path(image, staged, MAX_PATH * 2);
    apr_update_backup_path(image, backup, MAX_PATH * 2);

    ASSERT_TRUE(write_file(image, "OLD", 3));

    /* Nothing staged: refused, and the image is untouched. */
    e = apr_update_swap(image, L"9.9.9");
    ASSERT_TRUE(apr_failed(&e));
    ASSERT_TRUE(read_all(image, buf, sizeof buf, &n));
    ASSERT_EQ_INT(3, (int)n);

    ASSERT_TRUE(write_file(staged, "NEW", 3));
    e = apr_update_swap(image, L"9.9.9");
    ASSERT_FALSE(apr_failed(&e));

    /* The new build is in place... */
    ASSERT_TRUE(read_all(image, buf, sizeof buf, &n));
    ASSERT_EQ_INT(3, (int)n);
    ASSERT_MEM_EQ("NEW", buf, 3);
    /* ...and the previous one is KEPT. Rule 2: it is not discarded until the
     * new one has proven it starts. */
    ASSERT_TRUE(read_all(backup, buf, sizeof buf, &n));
    ASSERT_MEM_EQ("OLD", buf, 3);
    /* The staged file is gone: it was moved, not copied. */
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(staged));

    /* And the pending marker is what the next start will read. */
    {
        AprUpdateState st;
        apr_update_state_load(&st);
        ASSERT_WSTR_EQ(L"9.9.9", st.pending);
    }

    wipe_dir(dir);
    drop_test_settings();
}

TEST(the_backup_is_retired_only_once_the_new_build_has_started)
{
    /* Pure, so every branch is a row rather than a reboot. */
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_NOTHING,
                  (int)apr_update_startup_action(L"", L"0.0.1"));
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_NOTHING,
                  (int)apr_update_startup_action(NULL, L"0.0.1"));

    /* The pending build is what is running: it started, so the old image can
     * go. */
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_RETIRE_BACKUP,
                  (int)apr_update_startup_action(L"0.0.2", L"0.0.2"));
    /* Compared as VERSIONS: a tag spelling and a compiled-in spelling are the
     * same build. */
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_RETIRE_BACKUP,
                  (int)apr_update_startup_action(L"v0.0.2", L"0.0.2"));

    /* A swap was recorded and something ELSE is running: the rename did not
     * take, or somebody put the old file back. The backup is what got them
     * here, and it stays. */
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_KEEP_BACKUP,
                  (int)apr_update_startup_action(L"0.0.2", L"0.0.1"));
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_KEEP_BACKUP,
                  (int)apr_update_startup_action(L"nonsense", L"0.0.1"));
    ASSERT_EQ_INT(APR_UPDATE_STARTUP_KEEP_BACKUP,
                  (int)apr_update_startup_action(L"0.0.2", L""));
}

TEST(startup_retires_the_backup_and_clears_the_marker)
{
    wchar_t dir[MAX_PATH * 2], image[MAX_PATH * 2];
    wchar_t backup[MAX_PATH * 2], staged[MAX_PATH * 2];
    AprUpdateState st;

    use_test_settings();
    (void)apr_update_state_test_erase();

    temp_dir(dir, MAX_PATH * 2);
    _snwprintf_s(image, MAX_PATH * 2, _TRUNCATE, L"%ls\\apprecorder.exe", dir);
    apr_update_backup_path(image, backup, MAX_PATH * 2);
    apr_update_staged_path(image, staged, MAX_PATH * 2);

    ASSERT_TRUE(write_file(image, "NEW", 3));
    ASSERT_TRUE(write_file(backup, "OLD", 3));
    ASSERT_TRUE(write_file(staged, "STALE", 5));

    /* The marker says the build now running is the one that was swapped in. */
    apr_update_state_load(&st);
    lstrcpynW(st.pending, APR_VERSION_STRING, APR_UPDATE_VERSION_CCH);
    (void)apr_update_state_save(&st);

    apr_update_startup(image);

    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(backup));
    /* And a staged build the last exit never applied is stale by definition. */
    ASSERT_EQ_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(staged));
    ASSERT_NE_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(image));

    apr_update_state_load(&st);
    ASSERT_EQ_INT(0, (int)st.pending[0]);

    wipe_dir(dir);
    drop_test_settings();
}

TEST(startup_keeps_the_backup_when_the_swap_did_not_take)
{
    wchar_t dir[MAX_PATH * 2], image[MAX_PATH * 2], backup[MAX_PATH * 2];
    AprUpdateState st;

    use_test_settings();
    (void)apr_update_state_test_erase();

    temp_dir(dir, MAX_PATH * 2);
    _snwprintf_s(image, MAX_PATH * 2, _TRUNCATE, L"%ls\\apprecorder.exe", dir);
    apr_update_backup_path(image, backup, MAX_PATH * 2);

    ASSERT_TRUE(write_file(image, "OLD", 3));
    ASSERT_TRUE(write_file(backup, "OLDER", 5));

    apr_update_state_load(&st);
    /* A version that is NOT what is running. */
    lstrcpynW(st.pending, L"99.0.0", APR_UPDATE_VERSION_CCH);
    (void)apr_update_state_save(&st);

    apr_update_startup(image);

    /* THE ONE FILE THAT MUST SURVIVE. This is the state a person recovers
     * from by renaming one file. */
    ASSERT_NE_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(backup));
    apr_update_state_load(&st);
    ASSERT_WSTR_EQ(L"99.0.0", st.pending);

    wipe_dir(dir);
    drop_test_settings();
}

TEST(the_paths_are_built_beside_the_image_and_nowhere_else)
{
    wchar_t buf[MAX_PATH * 2];

    ASSERT_GT_INT(0, (int)apr_update_staged_path(L"C:\\Apps\\apprecorder.exe",
                                                 buf, MAX_PATH * 2));
    ASSERT_WSTR_EQ(L"C:\\Apps\\apprecorder.exe.new", buf);
    ASSERT_GT_INT(0, (int)apr_update_backup_path(L"C:\\Apps\\apprecorder.exe",
                                                 buf, MAX_PATH * 2));
    ASSERT_WSTR_EQ(L"C:\\Apps\\apprecorder.exe.old", buf);

    ASSERT_EQ_INT(0, (int)apr_update_staged_path(NULL, buf, MAX_PATH * 2));
    ASSERT_EQ_INT(0, (int)apr_update_backup_path(L"", buf, MAX_PATH * 2));

    /* The running image is a real, absolute path. */
    ASSERT_GT_INT(0, (int)apr_update_image_path(buf, MAX_PATH * 2));
    ASSERT_NE_INT((int)INVALID_FILE_ATTRIBUTES, (int)GetFileAttributesW(buf));
}

/* ===========================================================================
 * THE GOLDEN VECTOR: the release tool and this verifier, pinned together
 *
 * These bytes were produced by tools/release/apprelease.py -- keygen, then
 * sign -- against a throwaway key, and they are checked in as fixed data. The
 * private half was discarded; the public half below is not the release key and
 * verifies nothing but this one document.
 *
 * WHY THIS EXISTS. The release side is Python and the install side is C, and
 * they agree on three things nothing else in either tree would catch drifting:
 *
 *   - the signature is RAW r||s, 64 bytes, big-endian -- not DER. If a later
 *     edit made the tool emit DER, or made this file grow an ASN.1 decoder,
 *     this case is what fails.
 *   - the signed bytes are the file's bytes EXACTLY: two-space indent, LF, no
 *     trailing newline, ASCII-escaped. Any normalization on either side breaks
 *     it.
 *   - the field names and the hex spelling of the hash.
 *
 * A round trip run by hand proves the two agree TODAY. This proves it in every
 * run afterwards, and it needs no private key to do so.
 * ========================================================================= */

static const char k_golden_json[] =
    "{\n"
    "  \"version\": \"9.9.9\",\n"
    "  \"asset\": \"fake.exe\",\n"
    "  \"sha256\": \"99feefdbba258cc678b03e5310ae2ab2928caa7f81975c2ec963f72d9cc77579\",\n"
    "  \"size\": 16,\n"
    "  \"notes\": \"Release tool round trip.\"\n"
    "}";

static const uint8_t k_golden_sig[APR_UPDATE_SIG_BYTES] = {
    0x9E, 0xE4, 0xCD, 0x66, 0x9D, 0xB1, 0x30, 0x86,
    0x3A, 0x75, 0xA1, 0x0C, 0x49, 0x4A, 0x1A, 0x2F,
    0xC6, 0x7F, 0x6F, 0x40, 0x46, 0xD7, 0x76, 0x1B,
    0x00, 0xCF, 0x38, 0x4A, 0x8D, 0xC1, 0x2C, 0xDB,
    0x3D, 0x46, 0x50, 0x76, 0x69, 0xD4, 0x6A, 0x13,
    0xBD, 0x72, 0x10, 0x57, 0x8D, 0xFC, 0xFE, 0x02,
    0x55, 0x67, 0x54, 0xA3, 0xC0, 0xD9, 0x87, 0x83,
    0xCC, 0xD0, 0x79, 0x6A, 0x82, 0x99, 0x06, 0xCF
};

static const uint8_t k_golden_pub[APR_UPDATE_PUBKEY_BYTES] = {
    0x81, 0x56, 0xEE, 0xE9, 0x0D, 0x18, 0x6C, 0x5D,
    0x91, 0x0C, 0xF7, 0x76, 0x7A, 0x7F, 0xDD, 0x07,
    0x3A, 0xDA, 0xBB, 0xC3, 0xE8, 0xE4, 0x2E, 0x99,
    0xE5, 0x05, 0x26, 0xEC, 0x4F, 0x81, 0xE8, 0xC5,
    0xAD, 0x9D, 0x7B, 0x3B, 0xA3, 0x84, 0xEC, 0x5A,
    0xB4, 0x7B, 0x52, 0xD9, 0x10, 0xCD, 0xBC, 0xAE,
    0x62, 0xC6, 0xFC, 0xAC, 0xFE, 0x63, 0x47, 0x02,
    0xA0, 0xFF, 0x9A, 0xCB, 0xA3, 0x9A, 0x27, 0xDE
};

TEST(what_the_release_tool_signs_is_what_this_build_accepts)
{
    AprUpdateManifest m;
    AprErr e;

    /* The tool wrote 180 bytes. If this fails, the literal above has drifted
     * from the file the tool produces and the signature case below is testing
     * something else. */
    ASSERT_EQ_INT(180, (int)strlen(k_golden_json));

    e = apr_update_verify_manifest(k_golden_json, strlen(k_golden_json),
                                   k_golden_sig, sizeof k_golden_sig,
                                   k_golden_pub, &m);
    ASSERT_FALSE(apr_failed(&e));
    ASSERT_WSTR_EQ(L"9.9.9", m.version);
    ASSERT_WSTR_EQ(L"fake.exe", m.asset);
    ASSERT_WSTR_EQ(L"Release tool round trip.", m.notes);
    ASSERT_EQ_U64(16u, m.size);
    ASSERT_EQ_INT(0x99, m.sha256[0]);
    ASSERT_EQ_INT(0x79, m.sha256[31]);
}

TEST(the_release_url_is_the_download_redirect_and_is_https)
{
    /* NOT api.github.com: 60 requests an hour per IP, shared across a NAT
     * (update.h section 3). And https, because there is no code path in this
     * product that fetches a release over anything else. */
    ASSERT_NULL(wcsstr(APR_UPDATE_BASE_URL, L"api.github.com"));
    ASSERT_EQ_INT(0, wcsncmp(APR_UPDATE_BASE_URL, L"https://", 8));
    ASSERT_NOT_NULL(wcsstr(APR_UPDATE_BASE_URL, L"/releases/latest/download/"));
}
