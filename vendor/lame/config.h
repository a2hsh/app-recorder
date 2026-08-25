/*
 * config.h -- apprecorder's build configuration for the vendored LAME 3.100.
 *
 * NOT part of the upstream tarball. LAME's sources all begin with
 *
 *     #ifdef HAVE_CONFIG_H
 *     # include <config.h>
 *     #endif
 *
 * and upstream generates that file with autoconf, which this tree does not
 * run. Upstream ships `configMS.h` for exactly this situation; this file is
 * that file, reduced to what an MSVC x64 build actually needs and corrected
 * in three places. Everything else under vendor/lame/ is byte-identical to
 * the release tarball -- see PROVENANCE.md.
 *
 * The three deliberate differences from configMS.h:
 *
 *   1. NO int8_t/uint32_t/... typedefs. configMS.h predates MSVC shipping
 *      <stdint.h>; typedefing them again in 2026 is a C2371 redefinition the
 *      moment anything includes the real header. HAVE_STDINT_H is set and
 *      machine.h includes it instead.
 *
 *   2. HAVE_MPGLIB is defined but DECODE_ON_THE_FLY is NOT. That combination
 *      is what lets tests/test_action_mp3.c decode with hip_decode while the
 *      shipping binary pays nothing for it: HAVE_MPGLIB only controls whether
 *      mpglib_interface.c compiles its hip_* entry points, and
 *      DECODE_ON_THE_FLY is the only thing that makes the ENCODER reference
 *      them (bitstream.c, lame.c, util.c). With it off, no encoder object
 *      references hip_*, so the linker never pulls mpglib into apprecorder.exe
 *      -- only into the test that calls hip_decode directly. configMS.h ties
 *      the two together (`#ifdef HAVE_MPGLIB -> DECODE_ON_THE_FLY`), which
 *      would have cost the binary ~60 KB of decoder for a test-only feature.
 *
 *   3. MIN_ARCH_SSE is defined. Without HAVE_NASM, LAME leaves its runtime
 *      CPU_features struct all zero, so the SSE paths in fft.c and quantize.c
 *      are reachable ONLY through MIN_ARCH_SSE. x86-64 guarantees SSE2
 *      architecturally, so on this target the guard is free to assert.
 */
#ifndef APR_LAME_CONFIG_H
#define APR_LAME_CONFIG_H

#define PACKAGE "lame"
#define VERSION "3.100"

/* Host headers MSVC provides. */
#define STDC_HEADERS
#define HAVE_ERRNO_H
#define HAVE_FCNTL_H
#define HAVE_LIMITS_H
#define HAVE_STDINT_H
#define HAVE_INTTYPES_H
#define HAVE_STRCHR
#define HAVE_MEMCPY

#define PROTOTYPES 1

/* Upstream's faster log2 approximation. Accurate enough for the psychoacoustic
 * model, which is what uses it. */
#define USE_FAST_LOG 1

/* Building the library itself, not a client of it: exports the internal
 * symbols the objects share and suppresses the DLL import decoration. */
#define LAME_LIBRARY_BUILD

/* Sizes. LAME reads these only through the ieee754_* typedefs below. */
#define SIZEOF_SHORT  2
#define SIZEOF_INT    4
#define SIZEOF_LONG   4
#define SIZEOF_FLOAT  4
#define SIZEOF_DOUBLE 8

typedef double ieee754_float64_t;
typedef float  ieee754_float32_t;

/* hip_decode et al. compile; the encoder still never calls them (see note 2). */
#define HAVE_MPGLIB

/* SSE2 is part of x86-64 (see note 3). */
#if defined(_M_X64) || defined(_M_AMD64)
#  define HAVE_XMMINTRIN_H
#  define MIN_ARCH_SSE
#endif

#endif /* APR_LAME_CONFIG_H */
