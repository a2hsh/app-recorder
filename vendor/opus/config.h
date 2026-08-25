/*
 * config.h -- THE ONE FILE IN vendor/opus/ THAT IS OURS.
 *
 * Upstream generates this with autoconf (config.h.in) or lets CMake write a
 * one-line version and pass everything else on the command line
 * (cmake/config.h.cmake.in is literally just PACKAGE_VERSION). apprecorder
 * builds neither of those, so this stands in. Every other file under
 * vendor/opus/ is byte-identical to the 1.5.2 release -- see PROVENANCE.md,
 * and keep it that way: "diffs clean against the tarball" is the property
 * that makes vendoring auditable.
 *
 * The rest of the switches libopus wants are on the command line in
 * CMakeLists.txt (OPUS_BUILD, USE_ALLOCA), because that is where upstream's
 * own CMake puts them and it keeps this file to the one thing autoconf would
 * actually have written.
 *
 * FOUR DELIBERATE CHOICES, ALL VISIBLE FROM HERE:
 *
 * 1. FLOAT, NOT FIXED_POINT. FIXED_POINT is undefined, so SILK compiles its
 *    float path (silk/float/) and CELT keeps float internally. That is the
 *    reference configuration, it is what every desktop libopus ships, and it
 *    suits a host that has already spent the mixer's whole pipeline in
 *    float32. silk/fixed/ is therefore not vendored at all.
 *
 * 2. NO RUNTIME CPU DISPATCH. OPUS_HAVE_RTCD and every OPUS_X86_MAY_HAVE_*
 *    are undefined, so celt/x86/, silk/x86/ and the arm trees are neither
 *    vendored nor compiled and libopus takes its portable C paths. Those
 *    directories are the ones that carry per-ISA function tables, and the
 *    dispatch cost is paid in code size on a build whose whole point is to
 *    stay under a megabyte. This is the same trade vendor/lame/config.h makes
 *    by dropping the NASM kernels.
 *
 * 3. NO DEEP PLC, NO DRED, NO OSCE. ENABLE_DEEP_PLC, ENABLE_DRED and
 *    ENABLE_OSCE stay undefined, which is upstream's default and which is why
 *    the 18 MB dnn/ directory (neural network weights) is not vendored: every
 *    include of it in silk/, celt/ and src/ is inside one of those three
 *    #ifdefs. They are decoder-side concealment features; a recorder never
 *    decodes.
 *
 * 4. NO CUSTOM_MODES. Non-standard sample rates and frame sizes are exactly
 *    what an Ogg Opus file may not contain, so enabling them could only ever
 *    produce a file no other player accepts.
 */
#ifndef APR_VENDOR_OPUS_CONFIG_H
#define APR_VENDOR_OPUS_CONFIG_H

/* Reported by opus_get_version_string(), and by nothing else. It is the one
 * value autoconf's config.h would have carried that libopus actually reads.
 * If this directory is ever re-vendored from a different release, this line
 * and PROVENANCE.md are what have to move together. */
#define PACKAGE_VERSION "1.5.2"

/* MSVC has no variable-length arrays, so the temporary-allocation mode has to
 * be alloca; celt/stack_alloc.h #errors out if none of the three is chosen.
 * Defined here as well as on the command line so a stray translation unit
 * that gets config.h but not the target's options still compiles. */
#ifndef USE_ALLOCA
#define USE_ALLOCA 1
#endif

#endif /* APR_VENDOR_OPUS_CONFIG_H */
