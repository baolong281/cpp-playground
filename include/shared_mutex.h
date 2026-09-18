#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

// many readers can be reading at the same time
//
// only one witer can be working at once
// is_writing cannot be true if read_count > 0
// read_count cannot be > 0 if is_writing is true

class shared_mutex {
public:
  enum class preference { read_preference, write_preference };
  using enum preference;

  // delete copies + moves
  shared_mutex(const shared_mutex &) = delete;
  shared_mutex &operator=(const shared_mutex &) = delete;
  shared_mutex(shared_mutex &&) = delete;
  shared_mutex &operator=(shared_mutex &&) = delete;

  shared_mutex()
      : read_count{0}, writes_waiting{0}, is_writing{false},
        pref{preference::write_preference} {}
  shared_mutex(preference pref)
      : read_count{0}, writes_waiting{0}, is_writing{false}, pref{pref} {}

  void lock_shared() {
    std::unique_lock<std::mutex> lck{mu};

    // wait until we are not writing
    cv_read.wait(lck, [&]() {
      return !is_writing &&
             (pref == preference::read_preference || writes_waiting == 0);
    });

    // if we are not writing, we have the lock again and can increment the read
    // count and exit
    read_count++;
  }

  // undefined behavior if we call this on a non-locked mutex
  // ignore for now
  void unlock_shared() {
    std::lock_guard<std::mutex> lck{mu};

    if (--read_count == 0)
      cv_write.notify_one();
  }

  void lock() {
    std::unique_lock<std::mutex> lck{mu};

    writes_waiting++;
    cv_write.wait(lck, [&]() {
      return read_count == 0 &&
             !is_writing; // only one writer + nobody can be reading
    });

    is_writing = true;
    writes_waiting--;
  }

  void unlock() {
    {
      std::lock_guard<std::mutex> lck{mu};
      is_writing = false;
    }

    cv_write.notify_one(); // this gets priority
    cv_read.notify_all(); // we can have multiple readers waiting, so notify all
                          // of them
  }

private:
  std::mutex mu;
  std::condition_variable cv_read;
  std::condition_variable cv_write;
  size_t read_count;
  size_t writes_waiting;
  bool is_writing;
  preference pref;
};
