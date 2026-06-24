// queuestress.cpp
//
// Regression test for the lock-free single-producer/single-consumer ring
// buffer used to hand MIDI input messages from the API callback thread to
// the user thread (MidiInApi::MidiQueue).
//
// It drives the REAL queue code: a producer thread pushes a long sequence of
// tagged messages through a deliberately tiny ring (forcing constant
// wraparound and full/empty transitions) while a consumer thread pops them
// and verifies they arrive in order and intact. If the queue's index
// publication is not correctly synchronized, this races -- which a
// ThreadSanitizer build flags, and which can otherwise surface as a torn
// std::vector read (corrupted payload) caught by the integrity checks below.
//
// Requires no MIDI hardware or backend, so it runs anywhere.

#include "RtMidi.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdint>

int main()
{
  const unsigned int RING = 8;       // small -> lots of wraparound / full
  const uint32_t COUNT = 500000;     // messages pushed through the ring

  MidiInApi::MidiQueue q;
  q.ringSize = RING;
  q.ring = new MidiInApi::MidiMessage[RING];

  // Set by the consumer if it ever sees an out-of-order or corrupted
  // message. Atomic so the producer can also observe it and stop spinning
  // (otherwise a failing consumer would leave the producer wedged on a full
  // ring), and so the flag itself is not a data race.
  std::atomic<int> failed(0);

  std::thread producer([&]{
    for (uint32_t i = 0; i < COUNT; ++i) {
      MidiInApi::MidiMessage m;
      m.timeStamp = (double)i;
      m.bytes.push_back((unsigned char)(i & 0xff));
      m.bytes.push_back((unsigned char)((i >> 8) & 0xff));
      m.bytes.push_back((unsigned char)((i >> 16) & 0xff));
      while (!q.push(m)) {
        if (failed.load()) return;   // consumer gave up; don't wedge here
      }
    }
  });

  std::thread consumer([&]{
    std::vector<unsigned char> bytes;
    double ts = 0.0;
    for (uint32_t i = 0; i < COUNT; ++i) {
      while (!q.pop(&bytes, &ts)) { /* ring empty: wait for producer */ }
      uint32_t got = (uint32_t)ts;
      uint32_t payload = bytes.size() == 3
        ? (uint32_t)(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16))
        : 0xffffffffu;
      if (got != i || bytes.size() != 3 || payload != i) {
        fprintf(stderr,
          "queuestress: corruption at %u: ts=%u size=%zu payload=%u\n",
          i, got, bytes.size(), payload);
        failed.store(1);
        break;
      }
    }
  });

  producer.join();
  consumer.join();
  delete[] q.ring;

  if (failed.load()) {
    fprintf(stderr, "queuestress: FAILED\n");
    return 1;
  }
  printf("queuestress: OK, %u messages in order and intact through ring of %u\n",
         COUNT, RING);
  return 0;
}
