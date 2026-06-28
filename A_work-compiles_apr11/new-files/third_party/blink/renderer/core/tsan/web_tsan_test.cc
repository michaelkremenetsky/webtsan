// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Standalone test for the WebTSan core algorithm.
// This tests the vector clock / shadow memory logic WITHOUT needing
// Chromium's build system. It uses only the data structures from web_tsan.h.
//
// Compile standalone (outside chromium build) with:
//   c++ -std=c++17 -DSTANDALONE_TEST -I. web_tsan_test.cc -o web_tsan_test
//
// Inside chromium build, this would be a gtest.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ---- Inline the core data structures for standalone testing ----
// In real chromium build, #include "web_tsan.h" and link web_tsan.cc.

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>

namespace blink {
namespace tsan {

static constexpr uint32_t kMaxThreads = 64;
static constexpr uint32_t kShadowCellsPerByte = 2;

class VClock {
 public:
  VClock() { clk_.fill(0); }
  uint64_t Get(uint32_t tid) const { return clk_[tid]; }
  void Set(uint32_t tid, uint64_t val) { clk_[tid] = val; }
  void Increment(uint32_t tid) { clk_[tid]++; }

  bool HappensBefore(const VClock& other) const {
    for (uint32_t i = 0; i < kMaxThreads; i++) {
      if (clk_[i] > other.clk_[i]) return false;
    }
    return true;
  }

  void Join(const VClock& other) {
    for (uint32_t i = 0; i < kMaxThreads; i++) {
      if (other.clk_[i] > clk_[i]) clk_[i] = other.clk_[i];
    }
  }
 private:
  std::array<uint64_t, kMaxThreads> clk_;
};

struct ShadowCell {
  uint32_t tid = 0;
  uint64_t epoch = 0;
  bool is_write = false;
  bool valid = false;
};

struct RaceReport {
  size_t sab_offset;
  void* sab_backing_store;
  size_t sab_byte_length;
  uint32_t tid1;
  bool is_write1;
  uint64_t epoch1;
  uint32_t tid2;
  bool is_write2;
  uint64_t epoch2;
};

class ShadowBuffer {
 public:
  ShadowBuffer(void* bs, size_t len)
      : backing_store_(bs), byte_length_(len),
        shadow_(len * kShadowCellsPerByte),
        stripe_mutexes_((len + 63) / 64) {}

  bool RecordAccess(size_t offset, size_t size, uint32_t tid,
                    bool is_write, const VClock& thread_clock,
                    RaceReport* out_report) {
    for (size_t i = 0; i < size && (offset + i) < byte_length_; i++) {
      size_t byte = offset + i;
      size_t stripe = byte / 64;
      std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe]);

      for (uint32_t c = 0; c < kShadowCellsPerByte; c++) {
        size_t idx = byte * kShadowCellsPerByte + c;
        ShadowCell& cell = shadow_[idx];
        if (!cell.valid || cell.tid == tid) continue;
        if (!cell.is_write && !is_write) continue;
        if (thread_clock.Get(cell.tid) >= cell.epoch) continue;

        // Race!
        if (out_report) {
          out_report->sab_offset = byte;
          out_report->sab_backing_store = backing_store_;
          out_report->sab_byte_length = byte_length_;
          out_report->tid1 = cell.tid;
          out_report->is_write1 = cell.is_write;
          out_report->epoch1 = cell.epoch;
          out_report->tid2 = tid;
          out_report->is_write2 = is_write;
          out_report->epoch2 = thread_clock.Get(tid);
        }

        // Still store the access
        uint32_t slot = tid % kShadowCellsPerByte;
        size_t si = byte * kShadowCellsPerByte + slot;
        shadow_[si] = {tid, thread_clock.Get(tid), is_write, true};
        return true;
      }

      uint32_t slot = tid % kShadowCellsPerByte;
      size_t si = byte * kShadowCellsPerByte + slot;
      shadow_[si] = {tid, thread_clock.Get(tid), is_write, true};
    }
    return false;
  }

 private:
  void* backing_store_;
  size_t byte_length_;
  std::vector<ShadowCell> shadow_;
  std::vector<std::mutex> stripe_mutexes_;
};

}  // namespace tsan
}  // namespace blink

// ---- Tests ----

using namespace blink::tsan;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK_TRUE(expr, msg) do { \
  if (!(expr)) { \
    printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    tests_failed++; \
  } else { \
    printf("PASS: %s\n", msg); \
    tests_passed++; \
  } \
} while(0)

// Test 1: Two unsynchronized writes to the same location = race
void test_basic_write_write_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock_t0;
  clock_t0.Increment(0);  // t0 epoch=1

  VClock clock_t1;
  clock_t1.Increment(1);  // t1 epoch=1

  RaceReport report;

  // Thread 0 writes to offset 10
  bool race = shadow.RecordAccess(10, 4, 0, true, clock_t0, &report);
  CHECK_TRUE(!race, "first write should not race");

  // Thread 1 writes to same offset without synchronization
  race = shadow.RecordAccess(10, 4, 1, true, clock_t1, &report);
  CHECK_TRUE(race, "unsynchronized write-write should race");
  CHECK_TRUE(report.tid1 == 0 && report.tid2 == 1,
             "race report has correct tids");
}

// Test 2: Two unsynchronized read-write = race
void test_read_write_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock_t0;
  clock_t0.Increment(0);
  VClock clock_t1;
  clock_t1.Increment(1);

  RaceReport report;

  // Thread 0 reads offset 20
  bool race = shadow.RecordAccess(20, 4, 0, false, clock_t0, &report);
  CHECK_TRUE(!race, "first read should not race");

  // Thread 1 writes same offset
  race = shadow.RecordAccess(20, 4, 1, true, clock_t1, &report);
  CHECK_TRUE(race, "unsynchronized read-write should race");
}

// Test 3: Two reads = no race
void test_read_read_no_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock_t0;
  clock_t0.Increment(0);
  VClock clock_t1;
  clock_t1.Increment(1);

  RaceReport report;

  bool race = shadow.RecordAccess(30, 4, 0, false, clock_t0, &report);
  CHECK_TRUE(!race, "first read ok");

  race = shadow.RecordAccess(30, 4, 1, false, clock_t1, &report);
  CHECK_TRUE(!race, "two reads should never race");
}

// Test 4: Synchronized write-write via happens-before = no race
void test_synchronized_no_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock_t0;
  clock_t0.Increment(0);  // t0 epoch=1

  RaceReport report;

  // Thread 0 writes at epoch 1
  bool race = shadow.RecordAccess(40, 4, 0, true, clock_t0, &report);
  CHECK_TRUE(!race, "first write ok");

  // Simulate synchronization: thread 1's clock knows about t0's epoch 1
  VClock clock_t1;
  clock_t1.Increment(1);  // t1 epoch=1
  clock_t1.Set(0, 1);     // t1 has seen t0 up to epoch 1 (synchronized!)

  race = shadow.RecordAccess(40, 4, 1, true, clock_t1, &report);
  CHECK_TRUE(!race, "synchronized write-write should NOT race");
}

// Test 5: Acquire/Release via vector clocks
void test_acquire_release_pattern() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  // Simulate: T0 writes, then releases (stores clock into sync variable).
  //           T1 acquires (joins sync variable's clock), then reads.
  // This should NOT race.

  VClock clock_t0;
  clock_t0.Increment(0);  // epoch=1
  clock_t0.Increment(0);  // epoch=2 (one for the write, one for the release)

  RaceReport report;
  bool race = shadow.RecordAccess(50, 4, 0, true, clock_t0, &report);
  CHECK_TRUE(!race, "T0 write ok");

  // Release: sync_clock = t0's clock
  VClock sync_clock;
  sync_clock.Join(clock_t0);

  // T1 acquires: join sync_clock into t1's clock
  VClock clock_t1;
  clock_t1.Increment(1);
  clock_t1.Join(sync_clock);  // Now t1 knows t0 epoch=2

  race = shadow.RecordAccess(50, 4, 1, false, clock_t1, &report);
  CHECK_TRUE(!race, "read after acquire-release should NOT race");
}

// Test 6: Same thread = no race
void test_same_thread_no_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock;
  clock.Increment(0);

  RaceReport report;
  bool race = shadow.RecordAccess(60, 4, 0, true, clock, &report);
  CHECK_TRUE(!race, "first access ok");

  clock.Increment(0);
  race = shadow.RecordAccess(60, 4, 0, true, clock, &report);
  CHECK_TRUE(!race, "same thread should never race with itself");
}

// Test 7: Different offsets = no race
void test_different_offsets_no_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock_t0;
  clock_t0.Increment(0);
  VClock clock_t1;
  clock_t1.Increment(1);

  RaceReport report;
  bool race = shadow.RecordAccess(70, 4, 0, true, clock_t0, &report);
  CHECK_TRUE(!race, "write to offset 70 ok");

  race = shadow.RecordAccess(80, 4, 1, true, clock_t1, &report);
  CHECK_TRUE(!race, "write to different offset should not race");
}

// Test 8: Partial overlap should race
void test_partial_overlap_race() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  VClock clock_t0;
  clock_t0.Increment(0);
  VClock clock_t1;
  clock_t1.Increment(1);

  RaceReport report;
  // T0 writes 4 bytes at offset 10 (bytes 10,11,12,13)
  bool race = shadow.RecordAccess(10, 4, 0, true, clock_t0, &report);
  CHECK_TRUE(!race, "first write ok");

  // T1 writes 4 bytes at offset 12 (bytes 12,13,14,15) — overlaps at 12,13
  race = shadow.RecordAccess(12, 4, 1, true, clock_t1, &report);
  CHECK_TRUE(race, "partially overlapping writes should race");
}

// Test 9: VClock happens-before transitivity
void test_vclock_transitivity() {
  VClock a, b, c;
  a.Increment(0);  // a = [1,0,0]
  b.Increment(1);  // b = [0,1,0]

  // b synchronizes with a
  b.Join(a);  // b = [1,1,0]
  CHECK_TRUE(a.HappensBefore(b), "a happens-before b after join");

  // c synchronizes with b
  c.Increment(2);  // c = [0,0,1]
  c.Join(b);       // c = [1,1,1]
  CHECK_TRUE(a.HappensBefore(c), "a happens-before c (transitive)");
  CHECK_TRUE(b.HappensBefore(c), "b happens-before c");
}

// Test 10: postMessage pattern (HB edge)
void test_postmessage_pattern() {
  uint8_t fake_sab[128] = {};
  ShadowBuffer shadow(fake_sab, 128);

  // Simulate: main thread writes, then postMessage to worker.
  // Worker receives message, then reads. Should not race.

  VClock clock_main;
  clock_main.Increment(0);  // epoch=1

  RaceReport report;
  bool race = shadow.RecordAccess(90, 4, 0, true, clock_main, &report);
  CHECK_TRUE(!race, "main thread write ok");

  // postMessage: snapshot main's clock
  VClock edge_clock = clock_main;

  // Worker: join edge into its clock before reading
  VClock clock_worker;
  clock_worker.Increment(1);
  clock_worker.Join(edge_clock);

  race = shadow.RecordAccess(90, 4, 1, false, clock_worker, &report);
  CHECK_TRUE(!race, "read after postMessage sync should NOT race");
}

int main() {
  printf("=== WebTSan Core Algorithm Tests ===\n\n");

  test_basic_write_write_race();
  test_read_write_race();
  test_read_read_no_race();
  test_synchronized_no_race();
  test_acquire_release_pattern();
  test_same_thread_no_race();
  test_different_offsets_no_race();
  test_partial_overlap_race();
  test_vclock_transitivity();
  test_postmessage_pattern();

  printf("\n=== Results: %d passed, %d failed ===\n",
         tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
