/*
 * outpath.h -- where a recording is saved, and what the name of it is.
 *
 * ===========================================================================
 * WHY THIS IS A MODULE AND NOT THREE LINES IN bus.c
 *
 *   Three separate places have to answer the same three questions -- can this
 *   path be written, what does this name expand to, and what happens when the
 *   name is already taken:
 *
 *     - the command line, when it checks a plan before opening anything;
 *     - the bus, when the user adds an output and must be told NOW if the
 *       folder does not exist;
 *     - the bus again, at the instant a recording starts, when the file is
 *       actually created.
 *
 *   src/cli/cli.c had the first of those as a private static. A second copy in
 *   bus.c would be exactly the DRY failure AGENTS.md rule 3 names, and the two
 *   would drift the first time either grew a case. So there is one owner, and
 *   this is it. Nothing here knows about audio.
 *
 * ===========================================================================
 * THE COLLISION POLICY IS AUTO-INCREMENT, NOT A PROMPT, AND THAT IS DELIBERATE
 *
 *   Recording twice to one name must not lose the first take. There were two
 *   candidate policies -- ask, or pick a free name -- and asking loses on
 *   three counts:
 *
 *     1. THERE IS OFTEN NOBODY TO ASK. This code runs from the runner at the
 *        instant recording starts, which is also `apprecorder --out x.wav` in
 *        a script, under --quiet, from a scheduled task. A policy that can
 *        prompt cannot be the single policy for both front ends, so it would
 *        have to be two policies -- and the command line's would be this one
 *        anyway.
 *     2. A PROMPT BETWEEN THE PRESS AND THE FIRST SAMPLE IS LOST AUDIO. The
 *        thing being recorded does not wait for a modal. For a recorder, the
 *        seconds spent answering a dialog are seconds missing from the take
 *        the user pressed record for.
 *     3. THE MODAL IS THE WORST PART OF THE PRODUCT FOR THIS USER. A dialog
 *        that steals focus mid-gesture is the failure mode this codebase has
 *        already been bitten by; a Record key that just records is better.
 *
 *   So a name that is taken is never overwritten and never refused: the next
 *   free one is used. `mix.wav` becomes `mix-2.wav`, then `mix-3.wav`. NOTHING
 *   IS SILENT ABOUT IT -- apr_bus_action_current_path() reports where the take
 *   really went, the runner raises APR_RUN_EV_OUTPUT_RENAMED, and both front
 *   ends say so out loud.
 *
 * ===========================================================================
 * THE TEMPLATE
 *
 *   A path may contain tokens in braces. Everything outside a token is copied
 *   literally, which is what keeps an ordinary Windows path -- which contains
 *   no braces -- its own template.
 *
 *     {date}   the local date,  2026-08-26
 *     {time}   the local time,  14-03-52   (colons are illegal in a filename)
 *     {bus}    the bus's name, with anything a filename cannot hold replaced
 *     {ext}    the extension the chosen format writes, without the dot
 *     {n}      the lowest number for which the whole path does not exist yet
 *     {{      a literal {
 *
 *   AN UNKNOWN TOKEN IS COPIED THROUGH UNCHANGED rather than refused. A brace
 *   in a Windows path is legal, and a path that used to work must not stop
 *   working because this feature was added.
 *
 *   {n} IS RESOLVED AGAINST THE DISK, not against a counter in memory. A
 *   counter would restart at 1 when the session was reopened tomorrow and
 *   collide with everything recorded today; "the lowest number that is free"
 *   is the same answer on both days and needs nothing written down.
 *
 * ===========================================================================
 * THREADING
 *
 *   Every function here touches the file system and resolves nothing from the
 *   string catalog. Call them from a front end or from the runner's own
 *   thread. NEVER from on_audio or a capture pump -- CreateFileW is exactly
 *   the unbounded stall action.h forbids there.
 */
#ifndef APPRECORDER_OUTPATH_H
#define APPRECORDER_OUTPATH_H

#include <stddef.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long an output path may be, template and expansion alike. THE ONE bound
 * for this, declared here because this module is what expands into it: bus.c
 * used to state its own APR_BUS_ACTION_PATH_CCH, and two constants that
 * happened to agree would have parted company the first time either moved.
 * MAX_PATH, which is what every other path in this product is bounded by. */
#define APR_OUT_PATH_CCH 260

/* What the tokens expand to. Every field may be NULL or empty; a token whose
 * material is missing expands to nothing rather than to the token's spelling,
 * so a template is never left with braces in a real filename. */
typedef struct AprOutContext {
    const wchar_t *bus_name;    /* {bus} */
    const wchar_t *extension;   /* {ext}, without the dot */
} AprOutContext;

/* Nonzero when `tmpl` contains at least one token this module recognises.
 * A path with none is a literal path and behaves exactly as it always did. */
int apr_out_has_tokens(const wchar_t *tmpl);

/* Nonzero when ONE named token appears in `tmpl`. `name` is the token spelled
 * without its braces and in lower case -- "n", "bus", "date", "time", "ext" --
 * and the match is case-insensitive on the template side, exactly as the
 * expander's is.
 *
 * It exists because "{n}" changes what a template MEANS: a caller comparing
 * two output names has to know that "take{n}.wav" on two buses is two files
 * and not one, which comparing their expansions cannot tell it (both expand to
 * "take1" until the disk is consulted). */
int apr_out_has_token(const wchar_t *tmpl, const wchar_t *name);

/* Expand the tokens once, with the CURRENT local time. Touches no disk, so
 * {n} expands to 1 here; use apr_out_resolve when the answer has to be a name
 * that is actually free. */
AprErr apr_out_expand(const wchar_t *tmpl, const AprOutContext *ctx,
                      wchar_t *out, size_t cch);

/* Expand, then apply THE COLLISION POLICY: the result never names a file that
 * already exists. This is what a recording start calls.
 *
 * `out_collided` is optional and is set when the policy HAD TO MOVE THE NAME
 * ASIDE -- i.e. the name the user asked for was already a recording. It is the
 * caller's cue to say so out loud, which is what stops auto-increment being a
 * silent rename.
 *
 * A template using {n} never sets it: the user asked for numbering, so getting
 * a number is the answer, not a surprise. */
AprErr apr_out_resolve(const wchar_t *tmpl, const AprOutContext *ctx,
                       wchar_t *out, size_t cch, int *out_collided);

/* CREATE THE FILE, and make the create itself the claim on the name.
 *
 * apr_out_resolve answers "which name is free"; this OPENS one, with
 * CREATE_NEW, and walks the same "-2, -3, ..." ladder when the file system
 * says somebody else got there first. That closes the window between the two,
 * which was not theoretical: two apprecorder processes started in the same
 * second with the same template both passed the existence check and then both
 * truncated the same file with CREATE_ALWAYS. "A take is never overwritten"
 * is a promise the file system now keeps, not one that depends on how little
 * time passes between two calls.
 *
 * This is what every action's create() calls INSTEAD of CreateFileW. It is
 * not a replacement for apr_out_resolve: the bus still resolves the template
 * first, because that is what it reports as the recording's name and what the
 * rename notice is raised from. In the ordinary case the name resolve chose is
 * still free here and this opens it on the first try.
 *
 * `path` is already expanded -- no tokens. `out_final` receives the name that
 * was actually created, which differs from `path` only when the race happened.
 * `out_collided` is optional and is set in exactly that case. `out_handle`
 * receives a Win32 HANDLE opened GENERIC_WRITE / FILE_SHARE_READ, which the
 * caller closes. */
AprErr apr_out_open_new(const wchar_t *path, wchar_t *out_final, size_t cch,
                        int *out_collided, void **out_handle);

/* Can a file be created at `path`? Creates nothing that was not already
 * there -- a file it had to create is deleted again -- which is what makes it
 * safe inside --dry-run and safe to call the moment a user types a name. */
AprErr apr_out_probe_writable(const wchar_t *path);

/* THE EARLY CHECK. Expand `tmpl` and probe the result, so that an unwritable
 * folder is refused at the moment the output is ADDED rather than an hour
 * later when the user presses record. Creates nothing.
 *
 * `out_expanded` is optional and receives what the template expanded to, which
 * is what a caller shows when it explains the refusal. */
AprErr apr_out_validate(const wchar_t *tmpl, const AprOutContext *ctx,
                        wchar_t *out_expanded, size_t cch);

/* The default name for a new output: a timestamped template under the user's
 * Music folder, which by construction never collides.
 *
 * It ends in ".{ext}" rather than in a fixed extension ON PURPOSE -- the format
 * is chosen in the same dialog, and a default that said ".wav" would be quietly
 * wrong the moment somebody picked MP3. */
void apr_out_default_template(wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif
#endif /* APPRECORDER_OUTPATH_H */
