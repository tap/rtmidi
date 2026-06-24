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
#include <string>

// Minimal concrete MidiInApi so we can observe the queue the base class
// constructor allocates (its pure virtuals are stubbed as no-ops).
namespace {
struct TestMidiIn : public MidiInApi {
  TestMidiIn( unsigned int n ) : MidiInApi( n ) {}
  RtMidi::Api getCurrentApi( void ) { return RtMidi::UNSPECIFIED; }
  void openPort( unsigned int, const std::string & ) {}
  void openVirtualPort( const std::string & ) {}
  void closePort( void ) {}
  void setClientName( const std::string & ) {}
  void setPortName( const std::string & ) {}
  void initialize( const std::string & ) {}
  unsigned int getPortCount( void ) { return 0; }
  std::string getPortName( unsigned int ) { return std::string(); }
  unsigned int ringSize() const { return inputData_.queue.ringSize; }
  MidiQueue &queue() { return inputData_.queue; }
};

MidiInApi::MidiMessage oneByte( unsigned char b )
{
  MidiInApi::MidiMessage m;
  m.bytes.push_back( b );
  return m;
}

// Deterministic edge cases for queue sizing. Returns 0 on success.
int checkQueueEdgeCases()
{
  int fail = 0;

  // A 0-size ring (default-constructed, no storage) must refuse pushes
  // rather than dividing by zero / dereferencing a null ring pointer.
  {
    MidiInApi::MidiQueue z;            // ringSize == 0, ring == NULL
    if ( z.push( oneByte( 0x90 ) ) ) {
      fprintf( stderr, "queue: push on empty ring should fail\n" );
      ++fail;
    }
  }

  // Usable capacity of an N-slot ring is N-1 (one slot is the sentinel).
  {
    const unsigned int N = 4;
    MidiInApi::MidiQueue q;
    q.ringSize = N;
    q.ring = new MidiInApi::MidiMessage[N];
    int pushed = 0;
    while ( q.push( oneByte( 0x90 ) ) ) ++pushed;
    if ( pushed != (int)( N - 1 ) ) {
      fprintf( stderr, "queue: capacity %d, expected %d\n", pushed, N - 1 );
      ++fail;
    }
    delete[] q.ring;
  }

  // MidiInApi(queueSizeLimit) must yield a ring that holds exactly
  // queueSizeLimit messages, and a request of 0 must not crash.
  {
    TestMidiIn ten( 10 );
    if ( ten.ringSize() != 11 ) {
      fprintf( stderr, "queue: ringSize %u, expected 11\n", ten.ringSize() );
      ++fail;
    }
    int pushed = 0;
    while ( ten.queue().push( oneByte( 0x90 ) ) ) ++pushed;
    if ( pushed != 10 ) {
      fprintf( stderr, "queue: limit 10 held %d\n", pushed );
      ++fail;
    }

    TestMidiIn zero( 0 );              // must construct and push-fail safely
    if ( zero.queue().push( oneByte( 0x90 ) ) ) {
      fprintf( stderr, "queue: limit 0 should hold nothing\n" );
      ++fail;
    }
  }

  return fail;
}
} // namespace

int main()
{
  if ( checkQueueEdgeCases() != 0 )
    return 1;

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
