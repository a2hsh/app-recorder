/* winver.h -- what Windows is this, and is it new enough to run on?
 *
 * WHY THIS IS A MODULE AND NOT TWO LINES IN main()
 *
 *   Both front ends need the answer, and one of them is a console program that
 *   cannot show a dialog. `ui/darkmode.c` already had to resolve the real build
 *   number for its own reasons, so putting the question here rather than
 *   answering it twice is AGENTS.md rule 3: one owner per cross-cutting fact.
 *
 * WHY GetVersionExW IS NOT USED
 *
 *   It lies -- an unmanifested process is told 6.2 -- and VerifyVersionInfo
 *   needs a manifest to tell the truth about 10 and later. We do ship a
 *   manifest, so both would work today, but the answer would then depend on
 *   our own compatibility section rather than on the machine. The floor below
 *   is a property of the OS, so it is asked of the OS:
 *   RtlGetNtVersionNumbers is undocumented, has been stable since Windows
 *   2000, and reports the real build regardless of manifest.
 */
#ifndef APR_WINVER_H
#define APR_WINVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Windows 10 version 2004. The floor is not a preference: it is the build in
 * which ActivateAudioInterfaceAsync gained
 * AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK (design 4.1). Below it there is
 * no way to capture one application's audio at all, which is the entire
 * product -- so this is a refusal to start, not a degraded mode. */
#define APR_WIN_MIN_BUILD 19041u

/* The running build. Cached after the first call; safe from any thread.
 * Returns 0 only if ntdll could not be asked at all, which is treated as
 * "too old" by apr_win_meets_floor() -- a machine that cannot answer is not a
 * machine to start recording on. */
uint32_t apr_win_build(void);

/* Non-zero when apr_win_build() >= APR_WIN_MIN_BUILD. */
int apr_win_meets_floor(void);

#ifdef __cplusplus
}
#endif
#endif /* APR_WINVER_H */
