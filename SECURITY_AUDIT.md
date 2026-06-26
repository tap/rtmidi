# RtMidi — Code & Security Audit

**Repository:** `tap/rtmidi` (fork of RtMidi by Gary P. Scavone)
**Version audited:** 6.0.0 (soname 7.0.0) — consistent across `RtMidi.h`, `configure.ac`, `CMakeLists.txt`
**Commit:** `a3233c2`
**Date:** 2026-06-24

## Resolution status

Fixes have landed in two rounds. **Round 1** (merged via PR #1): the MidiQueue
data race, the shared input parser + zero-length-event OOB (JACK/Android), the
queue-sizing crash, the duplicate Web MIDI API entry, and the C-wrapper
hardening — plus the test suite and CI expansion. **Round 2** (this branch):
the remaining backend guards — ALSA free-of-garbage subscription, JACK
`MidiOutJack::getPortName` OOB, WinMM `sysex->dwUser` bounds, CoreMIDI
`CFRetain(NULL)` and uninitialized name buffers — followed by the WinMM buffer
and critical-section leaks (openPort/closePort) and the JACK `closePort`
teardown race (now fixed via a `jack_deactivate`-on-close / reactivate-on-open
`active` flag, following JACK's documented model; compile-verified, though CI
runs no JACK server to exercise it at runtime).

**WinUWP callback state race — fixed.** The MessageReceived handler updated
shared timestamp state (`last_time_`, `before_qpc_`, `b_overflow_low_`,
`firstMessage`) with no synchronization against a concurrent `close()`. A
dedicated `mtx_in_callback_` now serializes the callback's state access, and an
`in_closing_` flag (set under that mutex before the handler is revoked) makes a
callback that races teardown bail before touching any state. A separate mutex
from `mtx_open_close_` is used deliberately, since `close()` holds the latter
while calling `in_port_.Close()` and the callback must not contend on it. After
the WinUWP CI compile job was added, this is compile-verified; the runtime
concurrency still can't be exercised in CI (no UWP device), so the residual
in-flight-callback-vs-destruction window inherent to token-based WinRT
revocation should be confirmed on a real UWP build.

## Summary

RtMidi is a mature, widely used realtime MIDI I/O library with a clean public API
and several well-engineered internals (ALSA self-pipe shutdown, JACK ringbuffer
protocol, compile-time enum/name-table synchronization). This audit nonetheless
found one **systemic correctness bug** (a "lock-free" queue with no atomics) and a
**recurring input-parsing pattern** (`buffer[size - 1]` with no zero-length guard)
repeated across several backends. Most findings are pre-existing upstream.

Every headline item below was confirmed by reading the source, not merely inferred.
Line numbers refer to the audited commit and may drift.

## Critical / High — correctness & memory safety

1. **`MidiQueue` lock-free SPSC ring buffer has no atomics or memory ordering.**
   `RtMidi.h:599-611`. `front`/`back` are plain `unsigned int`. The MIDI input thread
   (producer) writes `back`; the user thread (`getMessage`, consumer) writes `front`;
   `size()` reads both — all unsynchronized. Under the C++ memory model this is a data
   race (UB): the consumer can observe `back` advance before the `ring[back] = msg`
   payload store is visible, reading a torn `std::vector` → heap corruption. Tends to
   "work" on x86, can corrupt on weakly ordered ARM (Apple Silicon, Android).
   *Fix:* make `front`/`back` `std::atomic<unsigned int>`; release-store on publish,
   acquire-load on read.

2. **Zero-length-event out-of-bounds read, repeated across backends.** The idiom
   `buffer[event.size - 1]` runs without first checking `size > 0`; if size is 0 the
   index is `(unsigned)-1` → wild OOB read.
   - JACK: `RtMidi.cpp:4047, 4057, 4072`
   - Android: `RtMidi.cpp:5135, 5138` (plus a useless `numBytesReceived >= 0` guard on a `size_t`)
   - CoreMIDI continuation path: `RtMidi.cpp:1052`
   - ALSA guards this correctly (`nBytes > 0`), so it is an inconsistency, not a universal idiom.
   *Fix:* `if (event.size == 0) continue;` before any buffer access, per backend.

3. **WinMM: unvalidated `sysex->dwUser` used as a `std::vector` index.**
   `RtMidi.cpp:2763, 2766`. `dwUser` is read out of an OS-supplied `MIDIHDR` and used to
   index `sysexBuffer[]` with no bounds check; the resulting pointer is then dereferenced.
   *Fix:* validate `dwUser < sysexBuffer.size()`.

4. **C wrapper: NULL args and uncaught exceptions cross the `extern "C"` boundary (UB).**
   `rtmidi_c.cpp:101, 113, 197, 358` — `std::string name = portName;` is constructed
   *outside* the try block, but the headers document the name args as optional;
   `std::string((const char*)NULL)` is UB. The callback proxies (`rtmidi_c.cpp:248, 255`)
   and `rtmidi_in_ignore_types` have no `catch(...)`, so an exception crossing the C ABI
   is UB. *Fix:* guard `portName ? portName : ""`; wrap every body, including `catch(...)`.

5. **JACK input thread vs. teardown use-after-free.** `RtMidi.cpp:4014-4091` vs. close/dtor.
   `closePort` unregisters the port and nulls `data->port` but never calls
   `jack_deactivate`, so a realtime process callback already in flight can run against a
   half-freed `JackMidiData`. *Fix:* `jack_deactivate()` before unregister/free.

6. **`queueSizeLimit == 1` causes unsigned underflow.** `RtMidi.cpp:903`. With
   `ringSize == 1`, `_size < ringSize - 1` becomes `_size < UINT_MAX` (always true);
   `back` never advances, pushes silently overwrite slot 0, and `size()` always returns 0.
   *Fix:* require/clamp `ringSize >= 2`.

## Medium — leaks, races, contract bugs

- **Duplicate `__WEB_MIDI_API__` block** at `RtMidi.cpp:522-527` — the compiled-API table
  lists the Web MIDI API twice. Trivial, definite bug.
- **Android SysEx reassembly broken:** `RtMidi.cpp:5160` `auto message = self->inputData_.message;`
  copies by value, so multi-packet SysEx accumulates into a stale local copy. *Fix:* `auto&`.
- **WinMM `MIDIHDR`/buffer leaks + lock leak** on open/close error paths
  (`RtMidi.cpp:2870-2904, 2924-2940`): early `return` inside a held `CRITICAL_SECTION`
  without `LeaveCriticalSection` → potential deadlock, plus leaked prepared headers.
- **WinUWP callback lifetime races** (`RtMidi.cpp:3524, 3566-3664`): delegate bound with
  raw `this`, no `auto_revoke`; callback can touch freed state after `close()`.
- **`error()` can throw out of `noexcept`/`throw()` destructors** (`~RtMidiIn`/`~RtMidiOut`,
  `RtMidi.h:308, 452`) → `std::terminate()` if backend teardown raises a non-warning error.
- **CoreMIDI:** `CFRetain(NULL)` crash path (`RtMidi.cpp:1361`), uninitialized
  `char name[128]` if `CFStringGetCString` fails (`:1457`), ~64 KB stack VLA in sysex send (`:1688`).
- **C wrapper `ok` flag never set `true` on success** — it reflects the last error *ever*,
  so a good call after a failed one still reports `ok == false`. `rtmidi_get_port_name`
  returns `snprintf`'s would-be length (can't detect truncation) and dereferences
  `bufLen` even when it may be NULL.
- **ALSA:** free-of-garbage subscription on malloc failure (`:2510`), JACK/ALSA
  `getPortName` OOB index past the NULL terminator (`:4223, 4446`), unbounded sysex
  allocation from device-supplied length (DoS), `doInput`/sysex flags shared without atomics.

## Build / CI / Supply chain

- **CI uses `actions/checkout@v2`** (EOL Node 16) — `.github/workflows/ci.yml:18, 39, 62`.
- **CI runs almost no functional tests** — the only registered test is `apinames`
  (API-name string mapping). No native MSVC/Windows job (the `.vcxproj`/`msw/` files are
  untested), no Android/WebMIDI build, no sanitizers (no ASan/UBSan/**TSan**, notable given
  the threading bugs), no CodeQL.
- **`cmake ..` on Linux silently builds no ALSA** — `CMakeLists.txt:45` defaults the option
  to `${ALSA}`, an undefined variable (the real one is `ALSA_FOUND`, probed ~80 lines later).
  ALSA only builds because CI passes `-DRTMIDI_API_ALSA=ON` explicitly.
- **Committed `gradle-wrapper.jar` (binary) with no provenance**; `gradle-wrapper.properties`
  downloads Gradle 8.0 with no `distributionSha256Sum`. Devcontainer pipes
  `curl … ohmyzsh/master/install.sh | zsh` and uses unpinned `debian:bullseye`.
- Minor: `if(WINDOWS)` typo (should be `WIN32`) makes Windows debug-postfix dead code
  (`CMakeLists.txt:34`); duplicated `include(GNUInstallDirs)`; deprecated `exec_program`;
  `throw()` specs are non-conforming in C++20 mode (the repo ships a C++20 module).
- **`android/app/src/main/cpp/RtMidi.cpp` is a vendored duplicate** of the root file and
  will drift from any fixes made to the canonical source.

## What is genuinely solid

- ALSA shutdown via the self-pipe trick (`trigger_fds` + `poll` wakeup + guarded
  `pthread_join`) is clean and race-light.
- JACK output ringbuffer protocol correctly length-prefixes, validates read space, and
  drains oversized messages rather than corrupting.
- Compile-time enum/name-table synchronization (`StaticAssertions`, and the C wrapper's
  `StaticEnumAssert` that breaks the build if the C and C++ enums diverge — all 9 API and
  11 error values verified to match).
- Move semantics on `RtMidi` correctly null the moved-from `rtapi_`, so no double-free.
- Version numbers are fully consistent across all three build systems, with configure-time
  enforcement.

## Suggested priority order

1. `MidiQueue` atomics (#1) — most likely to cause field crashes on ARM/Apple Silicon.
2. Zero-length-event guards in JACK/Android/CoreMIDI (#2) + WinMM `dwUser` bounds (#3).
3. C-wrapper NULL/exception hardening (#4) and the duplicate Web-API line.
4. CI: `checkout@v4`, add an MSVC job and an ASan/UBSan/TSan job, fix the ALSA default option.
5. Remaining leaks/races (JACK `jack_deactivate`, WinMM cleanup, WinUWP `auto_revoke`).
