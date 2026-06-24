/* testc_hardening.c
 *
 * Exercises the defensive guards added to the C wrapper: NULL string
 * arguments, a NULL output-length pointer, and an invalid send length.
 * These must never crash or let a C++ exception cross the C ABI.
 *
 * The argument-validation guards are checked unconditionally (they do not
 * require a working MIDI backend). The paths that dereference the backend
 * object are run only when one could actually be created, so this test is
 * safe on CI machines with no MIDI subsystem.
 */

#include "rtmidi_c.h"
#include <stdio.h>
#include <stddef.h>

static int failures = 0;

#define CHECK(cond, msg) \
  do { if (!(cond)) { fprintf(stderr, "hardening: FAIL: %s\n", msg); ++failures; } } while (0)

int main(void)
{
    RtMidiOutPtr out = rtmidi_out_create_default();
    CHECK(out != NULL, "out_create_default returns a wrapper");

    /* Record backend availability up front: the argument-guard checks below
     * intentionally set ok = false, so we must sample it before then. */
    int have_backend = (out != NULL && out->ok);

    /* Backend-dependent paths: a NULL port name must not crash. Run these
     * first, while a usable backend object exists. */
    if (have_backend) {
        rtmidi_open_port(out, 0, NULL);
        rtmidi_open_virtual_port(out, NULL);
    } else {
        printf("hardening: no usable MIDI backend; skipped open-port checks\n");
    }

    /* Invalid send arguments must be rejected, not turned into a huge read. */
    unsigned char note[3] = { 0x90, 60, 100 };
    CHECK(rtmidi_out_send_message(out, note, -1) == -1, "negative length rejected");
    CHECK(rtmidi_out_send_message(out, NULL, 5) == -1, "null buffer rejected");

    /* A NULL bufLen must be rejected rather than dereferenced. */
    char buf[64];
    CHECK(rtmidi_get_port_name(out, 0, buf, NULL) == -1, "null bufLen rejected");

    /* A NULL client name must not crash construction. */
    RtMidiInPtr in = rtmidi_in_create(RTMIDI_API_UNSPECIFIED, NULL, 100);
    CHECK(in != NULL, "in_create with NULL name returns a wrapper");

    rtmidi_in_free(in);
    rtmidi_out_free(out);

    if (failures) {
        fprintf(stderr, "hardening: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("hardening: OK\n");
    return 0;
}
