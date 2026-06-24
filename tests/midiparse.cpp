// midiparse.cpp
//
// Unit tests for MidiInApi::collectMessage, the shared MIDI input message
// assembly used by the backends. It needs no MIDI hardware or backend, so
// it runs anywhere and is wired into `make check` / `ctest`.
//
// Uses explicit checks (not assert) so it still validates under -DNDEBUG.

#include "RtMidi.h"
#include <vector>
#include <cstdio>

typedef MidiInApi::MidiMessage MidiMessage;

static int failures = 0;

static void expect( bool cond, const char *what )
{
  if ( !cond ) {
    fprintf( stderr, "midiparse: FAIL: %s\n", what );
    ++failures;
  }
}

static bool sameBytes( const MidiMessage &m, const std::vector<unsigned char> &b )
{
  return m.bytes == b;
}

// Convenience wrapper: feed one event.
static bool feed( const std::vector<unsigned char> &ev, unsigned char ignore,
                  bool &continueSysex, MidiMessage &msg )
{
  return MidiInApi::collectMessage( ev.data(), ev.size(), ignore,
                                    continueSysex, msg );
}

int main()
{
  // 1. Zero-length event must be a no-op and must NOT read out of bounds.
  {
    bool cont = false;
    MidiMessage m;
    unsigned char dummy = 0;
    bool deliver = MidiInApi::collectMessage( &dummy, 0, 0, cont, m );
    expect( deliver == false, "empty event: not delivered" );
    expect( cont == false, "empty event: continueSysex unchanged (false)" );
    expect( m.bytes.empty(), "empty event: no bytes accumulated" );

    // ... and mid-SysEx, an empty event must not terminate or crash.
    cont = true;
    bool deliver2 = MidiInApi::collectMessage( &dummy, 0, 0, cont, m );
    expect( deliver2 == false, "empty event mid-sysex: not delivered" );
    expect( cont == true, "empty event mid-sysex: continueSysex preserved" );
  }

  // 2. A simple 3-byte Note On is delivered intact.
  {
    bool cont = false;
    MidiMessage m;
    bool deliver = feed( { 0x90, 0x3C, 0x64 }, 0, cont, m );
    expect( deliver, "note on: delivered" );
    expect( cont == false, "note on: not in sysex" );
    expect( sameBytes( m, { 0x90, 0x3C, 0x64 } ), "note on: bytes intact" );

    // 8. Re-using the same message clears the previous contents.
    bool deliver2 = feed( { 0x80, 0x3C, 0x40 }, 0, cont, m );
    expect( deliver2, "note off: delivered" );
    expect( sameBytes( m, { 0x80, 0x3C, 0x40 } ), "note off: prior bytes cleared" );
  }

  // 3. A complete SysEx in a single event is delivered.
  {
    bool cont = false;
    MidiMessage m;
    bool deliver = feed( { 0xF0, 0x7E, 0x00, 0xF7 }, 0, cont, m );
    expect( deliver, "single-event sysex: delivered" );
    expect( cont == false, "single-event sysex: sysex complete" );
    expect( sameBytes( m, { 0xF0, 0x7E, 0x00, 0xF7 } ), "single-event sysex: intact" );
  }

  // 4. A SysEx split across three events accumulates and delivers once.
  {
    bool cont = false;
    MidiMessage m;
    expect( !feed( { 0xF0, 0x01, 0x02 }, 0, cont, m ), "split sysex a: not yet delivered" );
    expect( cont == true, "split sysex a: continuing" );
    expect( !feed( { 0x03, 0x04, 0x05 }, 0, cont, m ), "split sysex b: not yet delivered" );
    expect( cont == true, "split sysex b: still continuing" );
    bool deliver = feed( { 0x06, 0xF7 }, 0, cont, m );
    expect( deliver, "split sysex c: delivered on F7" );
    expect( cont == false, "split sysex c: sysex complete" );
    expect( sameBytes( m, { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xF7 } ),
            "split sysex: fully concatenated" );
  }

  // 5. With SysEx ignored, no bytes are kept and nothing is delivered, but
  //    the continuation state is still tracked across the whole message.
  {
    bool cont = false;
    MidiMessage m;
    expect( !feed( { 0xF0, 0x01 }, 0x01, cont, m ), "ignored sysex a: not delivered" );
    expect( cont == true, "ignored sysex a: continuation tracked" );
    expect( m.bytes.empty(), "ignored sysex a: no bytes kept" );
    expect( !feed( { 0x02, 0xF7 }, 0x01, cont, m ), "ignored sysex b: not delivered" );
    expect( cont == false, "ignored sysex b: terminated on F7" );
    expect( m.bytes.empty(), "ignored sysex b: still no bytes kept" );
  }

  // 6. Timing/clock (0xF8): delivered normally, dropped when ignored (0x02).
  {
    bool cont = false;
    MidiMessage m;
    expect( feed( { 0xF8 }, 0, cont, m ), "clock: delivered when not ignored" );
    cont = false;
    expect( !feed( { 0xF8 }, 0x02, cont, m ), "clock: dropped when ignored" );
  }

  // 7. Active sensing (0xFE): delivered normally, dropped when ignored (0x04).
  {
    bool cont = false;
    MidiMessage m;
    expect( feed( { 0xFE }, 0, cont, m ), "sensing: delivered when not ignored" );
    cont = false;
    expect( !feed( { 0xFE }, 0x04, cont, m ), "sensing: dropped when ignored" );
  }

  if ( failures ) {
    fprintf( stderr, "midiparse: %d FAILURE(S)\n", failures );
    return 1;
  }
  printf( "midiparse: OK, all cases passed\n" );
  return 0;
}
