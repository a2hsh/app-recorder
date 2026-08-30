/*
 * update_key.c -- the ONE trusted thing in the updater.
 *
 * ===========================================================================
 * WHAT THIS FILE IS
 *
 *   The author's ECDSA P-256 PUBLIC key, as the raw 64 bytes X||Y that BCrypt
 *   wants. Nothing else in this program decides whether a release is genuine;
 *   this is the whole root of trust, which is why it is a file of its own with
 *   nothing else in it. A reviewer can read this file in ten seconds and know
 *   exactly what apprecorder trusts.
 *
 *   THE PRIVATE HALF IS NEVER IN THIS REPOSITORY AND NEVER WILL BE. It lives
 *   with the author, offline, and it is the only thing that can produce a
 *   release this program will install. If it leaks, the fix is a new keypair,
 *   a new value here, and a release signed with the old one that carries the
 *   new binary -- which is why the old key must be kept until every install
 *   has moved past it.
 *
 * ===========================================================================
 * HOW THE AUTHOR FILLS THIS IN
 *
 *       uv run tools/release/apprelease.py keygen
 *
 *   writes the private key to a path outside the repository (it names it, and
 *   it refuses to overwrite one that already exists) and prints exactly the
 *   64-byte block below, ready to paste over it. Nothing else in the tree
 *   changes.
 *
 * ===========================================================================
 * WHY ALL-ZEROS IS A MEANINGFUL VALUE
 *
 *   Until that command has been run, this array is zeros, and
 *   apr_update_have_key() returns 0, and the updater DOES NOT LOOK.
 *
 *   Not "looks and fails to verify": a build that announced a refused
 *   signature every five minutes would teach its user -- who is blind, and for
 *   whom every one of those announcements interrupts something -- that the
 *   refusal sentence means nothing. And emphatically not "looks and skips
 *   verification", which is the same bug as having no updater but with a
 *   download attached.
 *
 *   A build with no trusted key cannot tell a release from a forgery, so it
 *   does not ask for one. tests/test_update.c pins that.
 */
#include "update.h"

#include <string.h>

/* ECDSA P-256 public key, X||Y, big-endian, 32 bytes each.
 *
 * REPLACE THIS BLOCK, and nothing else, when generating the release keypair.
 */
static const uint8_t g_public_key[APR_UPDATE_PUBKEY_BYTES] = {
    /* X */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Y */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t *apr_update_public_key(void)
{
    return g_public_key;
}

int apr_update_have_key(void)
{
    size_t i;
    for (i = 0; i < APR_UPDATE_PUBKEY_BYTES; i++) {
        if (g_public_key[i] != 0) return 1;
    }
    return 0;
}
