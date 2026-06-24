/* testc_hardening.c
 *
 * Exercises the defensive argument guards added to the C wrapper:
 * a negative send length, a NULL message buffer, and a NULL output-length
 * pointer. Each must be rejected (return -1) rather than crashing or
 * letting an exception cross the C ABI.
 *
 * These guards reject their input BEFORE touching the wrapped backend
 * object, so the test deliberately does NOT create a real RtMidi device:
 * a usable MIDI backend isn't available on CI runners, and a half-open
 * one (e.g. JACK with no server) is unsafe to operate on. We pass a bare,
 * zero-initialized wrapper instead -- enough for the guards, which only
 * read ok/msg, and free is via the C library (matching its allocator).
 *
 * The NULL string-argument and exception-boundary guards are covered by a
 * local dummy-backend run; they require a constructed device and so are
 * not exercised here.
 */

#include "rtmidi_c.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) \
  do { if (!(cond)) { fprintf(stderr, "hardening: FAIL: %s\n", msg); ++failures; } } while (0)

int main(void)
{
    /* A bare wrapper with no backend object (ptr == NULL). */
    struct RtMidiWrapper *w = (struct RtMidiWrapper *) calloc(1, sizeof *w);
    if (!w) { fprintf(stderr, "hardening: calloc failed\n"); return 1; }

    unsigned char note[3] = { 0x90, 60, 100 };

    /* Negative length must be rejected, not converted to a huge size_t. */
    CHECK(rtmidi_out_send_message(w, note, -1) == -1, "negative length rejected");

    /* NULL buffer with a positive length must be rejected. */
    CHECK(rtmidi_out_send_message(w, NULL, 5) == -1, "null buffer rejected");

    /* A NULL bufLen must be rejected rather than dereferenced. */
    char buf[64];
    CHECK(rtmidi_get_port_name(w, 0, buf, NULL) == -1, "null bufLen rejected");

    /* The guards set an error message via the library's allocator; free it
     * the same way (rtmidi has no public msg-free, and the C library here
     * shares the wrapper's CRT/allocator). */
    if (w->msg) free(w->msg);
    free(w);

    if (failures) {
        fprintf(stderr, "hardening: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("hardening: OK\n");
    return 0;
}
