#include "hashmap.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *expr, const char *file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", expr, file, line);
    g_failures++;
  }
}

} // namespace

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

// Hash that funnels every key into the same outer table and the same
// starting slot, forcing the quadratic probing loop to actually walk
// multiple slots instead of always landing on an empty one immediately.
// Table capacity is 256; empirically, quadratic probing (h + i*i) from a
// fixed start on this power-of-two table size stops finding empty slots
// after ~44 inserts (the probe sequence cycles through a limited residue
// set instead of covering the table) - confirmed by bisecting insert
// counts against a wall-clock timeout outside this suite. Tests below that
// use this hash stay well under that (kInserts = 32) to keep a margin.
struct AllCollideHash {
  size_t operator()(int) const { return 0; }
};

// ---------------------------------------------------------------------------
// insert()
// ---------------------------------------------------------------------------

void test_insert_new_key_returns_true() {
  Dict<int, int> d;
  CHECK(d.insert(1, 100) == true);
}

void test_insert_duplicate_key_returns_false() {
  Dict<int, int> d;
  CHECK(d.insert(1, 100) == true);
  CHECK(d.insert(1, 200) == false);
}

void test_insert_many_distinct_keys() {
  Dict<int, int> d;
  for (int i = 0; i < 1000; i++) {
    CHECK(d.insert(i, i * 2) == true);
  }
}

void test_insert_zero_and_negative_keys() {
  Dict<int, int> d;
  CHECK(d.insert(0, 0) == true);
  CHECK(d.insert(-1, -1) == true);
  CHECK(d.insert(0, 999) == false);
}

void test_insert_forces_probing_within_one_bucket() {
  Dict<int, int, AllCollideHash> d;
  constexpr int kInserts = 32;
  for (int i = 0; i < kInserts; i++) {
    CHECK(d.insert(i, i) == true);
  }
  // every key is distinct, so re-inserting any of them must be rejected
  CHECK(d.insert(0, -1) == false);
  CHECK(d.insert(kInserts - 1, -1) == false);
}

void test_insert_reuses_slot_after_erase() {
  Dict<int, int> d;
  CHECK(d.insert(1, 100) == true);
  CHECK(d.erase(1) == true);
  CHECK(d.insert(1, 200) == true);
  auto v = d.find(1);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 200);
}

// Regression target: insert() treats the first non-occupied slot in the
// probe chain (empty OR deleted) as "free to use" and inserts there
// immediately, without checking whether the key already exists further
// along the same chain. If a tombstone sits in front of the key's real
// entry, insert() currently creates a second, duplicate entry for a key
// that's already present, instead of returning false.
void test_insert_does_not_duplicate_key_past_a_tombstone() {
  Dict<int, int, AllCollideHash> d;
  CHECK(d.insert(1, 100) == true); // lands at probe position 0
  CHECK(d.insert(2, 200) == true); // collides, lands at probe position 1
  CHECK(d.erase(1) == true);       // probe position 0 is now a tombstone
  // key 2 already exists at probe position 1; re-inserting it must fail
  CHECK(d.insert(2, 999) == false);
}

// ---------------------------------------------------------------------------
// find()
// ---------------------------------------------------------------------------

void test_find_on_empty_map_returns_nullopt() {
  Dict<int, int> d;
  CHECK(d.find(1).has_value() == false);
}

void test_find_returns_inserted_value() {
  Dict<int, int> d;
  d.insert(1, 100);
  auto v = d.find(1);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 100);
}

void test_find_multiple_keys_returns_correct_values() {
  Dict<int, int> d;
  for (int i = 0; i < 50; i++)
    d.insert(i, i * 10);
  for (int i = 0; i < 50; i++) {
    auto v = d.find(i);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i * 10);
  }
}

void test_find_missing_key_in_nonempty_map_returns_nullopt() {
  Dict<int, int> d;
  d.insert(1, 100);
  d.insert(2, 200);
  CHECK(d.find(999).has_value() == false);
}

void test_find_ignores_rejected_duplicate_insert() {
  Dict<int, int> d;
  CHECK(d.insert(1, 100) == true);
  CHECK(d.insert(1, 200) == false); // rejected, must not overwrite
  auto v = d.find(1);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 100);
}

void test_find_with_collisions_locates_correct_key() {
  // Every key shares a hash, so find() must walk past other occupied slots
  // (via the same quadratic probe sequence insert used) to land on the
  // right one.
  Dict<int, int, AllCollideHash> d;
  constexpr int kInserts = 32;
  for (int i = 0; i < kInserts; i++)
    d.insert(i, i * 100);

  for (int i = 0; i < kInserts; i++) {
    auto v = d.find(i);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i * 100);
  }
  // never inserted, but hashes into the same crowded bucket
  CHECK(d.find(kInserts).has_value() == false);
}

void test_find_returns_nullopt_after_erase() {
  Dict<int, int> d;
  d.insert(1, 100);
  CHECK(d.erase(1) == true);
  CHECK(d.find(1).has_value() == false);
}

// Positive counterpart to the insert()/tombstone regression above: find()
// must skip past a deleted slot to reach a key that comes later in the
// same probe chain.
void test_find_skips_tombstone_to_reach_later_key() {
  Dict<int, int, AllCollideHash> d;
  CHECK(d.insert(1, 100) == true); // probe position 0
  CHECK(d.insert(2, 200) == true); // probe position 1
  CHECK(d.erase(1) == true);       // probe position 0 is now a tombstone
  auto v = d.find(2);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 200);
}

// ---------------------------------------------------------------------------
// erase()
// ---------------------------------------------------------------------------

void test_erase_on_empty_map_returns_false() {
  Dict<int, int> d;
  CHECK(d.erase(1) == false);
}

void test_erase_existing_key_returns_true() {
  Dict<int, int> d;
  d.insert(1, 100);
  CHECK(d.erase(1) == true);
}

void test_erase_missing_key_in_nonempty_map_returns_false() {
  Dict<int, int> d;
  d.insert(1, 100);
  CHECK(d.erase(999) == false);
}

void test_erase_same_key_twice_second_call_returns_false() {
  Dict<int, int> d;
  d.insert(1, 100);
  CHECK(d.erase(1) == true);
  CHECK(d.erase(1) == false);
}

void test_erase_one_key_leaves_others_intact() {
  Dict<int, int> d;
  d.insert(1, 100);
  d.insert(2, 200);
  CHECK(d.erase(1) == true);
  CHECK(d.find(1).has_value() == false);
  auto v = d.find(2);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 200);
}

// ---------------------------------------------------------------------------
// get_or_insert()
// ---------------------------------------------------------------------------

void test_get_or_insert_new_key_stores_and_returns_value() {
  Dict<int, int> d;
  auto v = d.get_or_insert(1, 100);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 100);
  auto found = d.find(1);
  CHECK(found.has_value() == true);
  if (found.has_value())
    CHECK(found.value() == 100);
}

void test_get_or_insert_existing_key_returns_existing_value() {
  Dict<int, int> d;
  d.insert(1, 100);
  auto v = d.get_or_insert(1, 999); // must not overwrite
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 100);
  auto found = d.find(1);
  CHECK(found.has_value() == true);
  if (found.has_value())
    CHECK(found.value() == 100);
}

void test_get_or_insert_is_stable_across_repeated_calls() {
  Dict<int, int> d;
  auto first = d.get_or_insert(1, 100);
  auto second = d.get_or_insert(1, 200);
  auto third = d.get_or_insert(1, 300);
  CHECK(first.has_value() && first.value() == 100);
  CHECK(second.has_value() && second.value() == 100);
  CHECK(third.has_value() && third.value() == 100);
}

// Same tombstone-navigation flaw as insert(): get_or_insert() stops at the
// first non-occupied slot in the probe chain without checking whether the
// key already exists further along it. With a tombstone in front of the
// real entry, this currently creates a duplicate and returns the newly
// given value instead of the pre-existing one.
void test_get_or_insert_does_not_duplicate_key_past_a_tombstone() {
  Dict<int, int, AllCollideHash> d;
  CHECK(d.insert(1, 100) == true); // probe position 0
  CHECK(d.insert(2, 200) == true); // probe position 1
  CHECK(d.erase(1) == true);       // probe position 0 is now a tombstone
  auto v = d.get_or_insert(2, 999);
  CHECK(v.has_value() == true);
  if (v.has_value())
    CHECK(v.value() == 200); // existing value, not the 999 just passed in
}

// ---------------------------------------------------------------------------
// resize()
// ---------------------------------------------------------------------------
// A Table starts with 256 slots. These force growth by inserting past that,
// using AllCollideHash where the point is specifically to fill *one*
// Table (resize is per-bucket, not global), and Dict's default hash where
// the point is to exercise resize happening independently across many
// buckets at once under a realistic key distribution.

void test_resize_allows_growth_past_initial_capacity() {
  Dict<int, int, AllCollideHash> d;
  constexpr int kInserts = 300; // > 256, cannot fit without at least one resize
  for (int i = 0; i < kInserts; i++)
    CHECK(d.insert(i, i * 3) == true);

  for (int i = 0; i < kInserts; i++) {
    auto v = d.find(i);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i * 3);
  }
}

void test_resize_survives_multiple_growths() {
  Dict<int, int, AllCollideHash> d;
  constexpr int kInserts = 900; // forces several 2x growths (256->512->1024->...)
  for (int i = 0; i < kInserts; i++)
    CHECK(d.insert(i, i) == true);

  for (int i = 0; i < kInserts; i++) {
    auto v = d.find(i);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i);
  }
}

void test_resize_across_realistic_key_distribution() {
  // Default hash spreads keys over all 16 outer buckets, so this exercises
  // several buckets independently resizing rather than just one.
  Dict<int, int> d;
  constexpr int kInserts = 4000;
  for (int i = 0; i < kInserts; i++)
    CHECK(d.insert(i, i * 2) == true);

  for (int i = 0; i < kInserts; i++) {
    auto v = d.find(i);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i * 2);
  }
}

void test_get_or_insert_participates_in_resize() {
  Dict<int, int, AllCollideHash> d;
  constexpr int kInserts = 300;
  for (int i = 0; i < kInserts; i++) {
    auto v = d.get_or_insert(i, i * 5);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i * 5);
  }
  for (int i = 0; i < kInserts; i++) {
    auto v = d.get_or_insert(i, -1); // key exists, must return the original
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i * 5);
  }
}

// Resize decisions are driven by num_entries, which erase() decrements -
// so a slot erase() claims to have freed needs to actually become
// available again (whether reused directly or accounted for by a later
// resize). This inserts past the original 256-slot capacity purely via a
// churn pattern (insert then immediately erase the same bucket, repeated
// past the table size), then checks that capacity is still usable
// afterward for a fresh batch of distinct keys.
void test_capacity_freed_by_erase_remains_usable() {
  Dict<int, int, AllCollideHash> d;
  constexpr int kCapacity = 256; // Table's initial `data(256)`

  for (int i = 0; i < kCapacity; i++) {
    CHECK(d.insert(i, i) == true);
    CHECK(d.erase(i) == true);
  }

  for (int i = 0; i < kCapacity; i++) {
    auto v = d.find(i);
    CHECK(v.has_value() == false); // erased, must not still be findable
  }

  for (int i = 0; i < kCapacity; i++) {
    CHECK(d.insert(kCapacity + i, i) == true);
  }
  for (int i = 0; i < kCapacity; i++) {
    auto v = d.find(kCapacity + i);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == i);
  }
}

// ---------------------------------------------------------------------------
// concurrency
// ---------------------------------------------------------------------------
// insert()/erase()/get_or_insert() each hold a single unique_lock for their
// whole scan-then-mutate body, so - tombstone-navigation bug above aside -
// there's no window between deciding a slot is free and claiming it. These
// tests check that holds up under contention; when built with
// -fsanitize=thread they also check no data race is reported (run the
// `hashmap_tests_tsan` target for that).

void test_concurrent_insert_disjoint_keys() {
  Dict<int, int> d;
  constexpr int kThreads = 8;
  // Kept well under 1000 total inserts: a *single-threaded* sequential
  // sweep of this same Dict type was confirmed (outside this suite, by
  // bisecting insert counts against a wall-clock timeout) to complete at
  // 1000 keys but return false past that on this hash/table-size combo -
  // a capacity/probing-coverage limit with no resize implemented yet,
  // unrelated to threading. Keeping the total here below that line keeps
  // this test about concurrency, not capacity.
  constexpr int kPerThread = 80;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; i++) {
        int key = t * kPerThread + i; // disjoint per thread, no contention
        if (d.insert(key, key))
          success_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &th : threads)
    th.join();

  CHECK(success_count.load() == kThreads * kPerThread);
}

void test_concurrent_insert_same_key_only_one_wins() {
  Dict<int, int> d;
  constexpr int kThreads = 16;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t]() {
      if (d.insert(42, t))
        success_count.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto &th : threads)
    th.join();

  CHECK(success_count.load() == 1);
}

void test_concurrent_insert_same_key_repeated() {
  // Same race as above, hammered repeatedly to raise the odds of hitting
  // any timing-dependent window on a given run.
  constexpr int kRounds = 200;
  constexpr int kThreads = 8;
  for (int r = 0; r < kRounds; r++) {
    Dict<int, int> d;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&, t]() {
        if (d.insert(7, t))
          success_count.fetch_add(1, std::memory_order_relaxed);
      });
    }
    for (auto &th : threads)
      th.join();
    CHECK(success_count.load() == 1);
  }
}

// Writers insert disjoint keys concurrently; once they've all joined, verify
// through find() that every key landed with its correct value. This is a
// handoff check (concurrent writes, then a single-threaded read pass), not a
// concurrent-find stress test.
void test_concurrent_insert_then_find_sees_all_values() {
  Dict<int, int> d;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 80; // see cap note on the disjoint-insert test above
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; i++) {
        int key = t * kPerThread + i;
        d.insert(key, key * 2);
      }
    });
  }
  for (auto &th : threads)
    th.join();

  for (int key = 0; key < kThreads * kPerThread; key++) {
    auto v = d.find(key);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == key * 2);
  }
}

// Readers call find() while a writer is still inserting, on a mix of keys
// that may or may not exist yet. Can't assert much about individual answers
// (a "not found yet" is legitimate mid-insert), so this only checks for
// crashes/hangs and that any value returned is one the writer actually
// wrote - i.e. find() never fabricates a value for a live key.
void test_concurrent_find_while_inserting_does_not_crash_or_fabricate() {
  Dict<int, int> d;
  constexpr int kKeys = 200;
  std::atomic<bool> stop{false};
  std::atomic<int> bad_reads{0};

  std::thread writer([&]() {
    for (int i = 0; i < kKeys; i++)
      d.insert(i, i * 2);
    stop.store(true);
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; r++) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        for (int i = 0; i < kKeys; i++) {
          auto v = d.find(i);
          if (v.has_value() && v.value() != i * 2)
            bad_reads.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  writer.join();
  for (auto &r : readers)
    r.join();

  CHECK(bad_reads.load() == 0);
}

void test_concurrent_erase_same_key_only_one_wins() {
  Dict<int, int> d;
  d.insert(42, 100);
  constexpr int kThreads = 16;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&]() {
      if (d.erase(42))
        success_count.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto &th : threads)
    th.join();

  CHECK(success_count.load() == 1);
  CHECK(d.find(42).has_value() == false);
}

// All threads race to be the one that creates a brand-new key via
// get_or_insert(); every thread - winner or not - must observe the same
// (winning) value, and exactly one insertion should have actually happened.
void test_concurrent_get_or_insert_same_new_key_agrees_on_winner() {
  constexpr int kRounds = 100;
  constexpr int kThreads = 8;
  for (int r = 0; r < kRounds; r++) {
    Dict<int, int> d;
    std::vector<std::thread> threads;
    std::vector<std::optional<int>> results(kThreads);

    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back(
          [&, t]() { results[t] = d.get_or_insert(7, t); });
    }
    for (auto &th : threads)
      th.join();

    for (auto &res : results)
      CHECK(res.has_value() == true);
    if (results[0].has_value())
      for (int t = 1; t < kThreads; t++)
        if (results[t].has_value())
          CHECK(results[t].value() == results[0].value());
  }
}

// resize() is only ever invoked from inside insert()/get_or_insert() while
// already holding the unique_lock, so it should be exclusive with every
// other operation on the same Table by construction. This forces several
// resizes (one bucket, past its 256-slot capacity) while under real thread
// contention, to check that holds up in practice and not just on paper.
void test_concurrent_insert_triggers_resize_safely() {
  Dict<int, int, AllCollideHash> d;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 50; // 400 total, forces at least one resize
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; i++) {
        int key = t * kPerThread + i; // disjoint per thread
        if (d.insert(key, key * 7))
          success_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &th : threads)
    th.join();

  CHECK(success_count.load() == kThreads * kPerThread);
  for (int key = 0; key < kThreads * kPerThread; key++) {
    auto v = d.find(key);
    CHECK(v.has_value() == true);
    if (v.has_value())
      CHECK(v.value() == key * 7);
  }
}

// ---------------------------------------------------------------------------
// watchdog: the shared_mutex backing Table is hand-rolled, so a locking bug
// could deadlock a test instead of failing it. Bound total run time so a
// hang shows up as a loud timeout instead of a stuck CI job.
// ---------------------------------------------------------------------------

std::atomic<bool> g_done{false};

void watchdog(int timeout_seconds) {
  for (int i = 0; i < timeout_seconds * 10; i++) {
    if (g_done.load())
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::fprintf(stderr,
               "TIMEOUT: tests did not finish within %ds - likely deadlock\n",
               timeout_seconds);
  std::quick_exit(124);
}

#define RUN(test)                                                             \
  do {                                                                       \
    std::fprintf(stderr, "RUNNING: %s\n", #test);                           \
    test();                                                                  \
  } while (0)

int main() {
  std::thread watchdog_thread(watchdog, 20);
  watchdog_thread.detach();

  RUN(test_insert_new_key_returns_true);
  RUN(test_insert_duplicate_key_returns_false);
  RUN(test_insert_many_distinct_keys);
  RUN(test_insert_zero_and_negative_keys);
  RUN(test_insert_forces_probing_within_one_bucket);
  RUN(test_insert_reuses_slot_after_erase);
  RUN(test_insert_does_not_duplicate_key_past_a_tombstone);

  RUN(test_find_on_empty_map_returns_nullopt);
  RUN(test_find_returns_inserted_value);
  RUN(test_find_multiple_keys_returns_correct_values);
  RUN(test_find_missing_key_in_nonempty_map_returns_nullopt);
  RUN(test_find_ignores_rejected_duplicate_insert);
  RUN(test_find_with_collisions_locates_correct_key);
  RUN(test_find_returns_nullopt_after_erase);
  RUN(test_find_skips_tombstone_to_reach_later_key);

  RUN(test_erase_on_empty_map_returns_false);
  RUN(test_erase_existing_key_returns_true);
  RUN(test_erase_missing_key_in_nonempty_map_returns_false);
  RUN(test_erase_same_key_twice_second_call_returns_false);
  RUN(test_erase_one_key_leaves_others_intact);

  RUN(test_get_or_insert_new_key_stores_and_returns_value);
  RUN(test_get_or_insert_existing_key_returns_existing_value);
  RUN(test_get_or_insert_is_stable_across_repeated_calls);
  RUN(test_get_or_insert_does_not_duplicate_key_past_a_tombstone);

  RUN(test_resize_allows_growth_past_initial_capacity);
  RUN(test_resize_survives_multiple_growths);
  RUN(test_resize_across_realistic_key_distribution);
  RUN(test_get_or_insert_participates_in_resize);
  RUN(test_capacity_freed_by_erase_remains_usable);

  RUN(test_concurrent_insert_disjoint_keys);
  RUN(test_concurrent_insert_same_key_only_one_wins);
  RUN(test_concurrent_insert_same_key_repeated);
  RUN(test_concurrent_insert_then_find_sees_all_values);
  RUN(test_concurrent_find_while_inserting_does_not_crash_or_fabricate);
  RUN(test_concurrent_erase_same_key_only_one_wins);
  RUN(test_concurrent_get_or_insert_same_new_key_agrees_on_winner);
  RUN(test_concurrent_insert_triggers_resize_safely);

  g_done.store(true);

  if (g_failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", g_failures);
  return 1;
}
