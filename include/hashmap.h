#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex.h>
#include <shared_mutex>
#include <vector>

// no references into our map
//
// we should return by value

template <typename K, typename V, typename H = std::hash<K>> class Dict {
public:
  Dict() : tables(16) {}

  // disable copies
  Dict &operator=(const Dict &other) = delete;
  Dict(const Dict &other) = delete;

  // we should have moves ?
  bool insert(K key, V value) {
    size_t h = hasher(key);
    return tables[h % tables.size()].insert(h, std::move(key),
                                            std::move(value));
  }
  // void insert_or_assign(K key, V value);
  std::optional<V> find(K key) {
    size_t h = hasher(key);
    return tables[h % tables.size()].find(h, std::move(key));
  }

  bool erase(K key) {
    size_t h = hasher(key);
    return tables[h % tables.size()].erase(h, std::move(key));
  }

  std::optional<V> get_or_insert(K key, V value) {
    size_t h = hasher(key);
    return tables[h % tables.size()].get_or_insert(h, std::move(key),
                                                   std::move(value));
  }
  // size_t size();
  //
  // // atomic rmw operations
  // // insert only if not present
  // void try_emplace(K key, V value);
  // std::optional<V> get_or_insert(K key, V value);

private:
  class Table {
  public:
    Table() : data(256) {}

    bool insert(size_t h, K key, V value) {
      std::unique_lock<shared_mutex> write_lock{rw_lock};

      size_t i = 1;
      size_t deleted_candidate = data.size() + 1;
      size_t idx = h % data.size();
      bool inserted = false;

      while (i < data.size() + 1) {
        Entry &e = data.at(idx);

        // we cannot immediately insert here, the key may be a bit farther down
        // this means if we want to insert into deleted with this operation we
        // have to do a full table scan
        if (e.state == Entry::slot_state::deleted &&
            deleted_candidate == data.size() + 1)
          deleted_candidate = idx;

        // we have found a slot (either new or old one and can insert)
        if (e.state == Entry::slot_state::empty) {
          e.state = Entry::slot_state::occupied;
          e.key = key;
          e.hash = h;
          e.value = value;
          inserted = true;
          break;
        }

        // must check if this is us
        if (e.hash == h && e.key == key &&
            e.state == Entry::slot_state::occupied) {
          return false;
        }

        idx = (idx + 1) % data.size();
        i++;
      }

      if (inserted) {
        num_entries++;
        check_and_resize();
        return true;
      }

      if (deleted_candidate != data.size() + 1) {
        Entry &e = data.at(deleted_candidate);
        e.state = Entry::slot_state::occupied;
        e.key = key;
        e.hash = h;
        e.value = value;
        return true;
      }

      return false;
    }

    std::optional<V> find(size_t h, K key) {
      std::shared_lock<shared_mutex> read_lock{rw_lock};
      size_t idx = h % data.size();
      size_t i = 1;

      while (i < data.size() + 1) {
        Entry &e = data.at(idx);

        // if empty key would have been here but its not
        if (e.state == Entry::slot_state::empty)
          return std::nullopt;

        // must check if this is us
        if (e.hash == h && e.key == key &&
            e.state == Entry::slot_state::occupied)
          return e.value;

        idx = (idx + 1) % data.size();
        i++;
      }

      // the map is full
      return std::nullopt;
    }

    bool erase(size_t h, K key) {
      std::unique_lock<shared_mutex> write_lock{rw_lock};
      size_t idx = h % data.size();
      size_t i = 1;

      while (i < data.size() + 1) {
        Entry &e = data.at(idx);

        // if empty key would have been here but its not
        if (e.state == Entry::slot_state::empty)
          return false;

        // must check if this is us
        if (e.hash == h && e.key == key &&
            e.state == Entry::slot_state::occupied) {
          num_entries--;
          e.state = Entry::slot_state::deleted;
          return true;
        }

        idx = (idx + 1) % data.size();
        i++;
      }
      return false;
    }

    std::optional<V> get_or_insert(size_t h, K key, V value) {
      std::unique_lock<shared_mutex> write_lock{rw_lock};

      size_t i = 1;
      size_t deleted_candidate = data.size() + 1;
      size_t idx = h % data.size();

      bool inserted = false;
      std::optional<V> res = std::nullopt;

      while (i < data.size() + 1) {
        Entry &e = data.at(idx);

        // we cannot immediately insert here, the key may be a bit farther down
        // this means if we want to insert into deleted with this operation we
        // have to do a full table scan
        if (e.state == Entry::slot_state::deleted &&
            deleted_candidate == data.size() + 1)
          deleted_candidate = idx;

        // we have found a slot (either new or old one and can insert)
        if (e.state == Entry::slot_state::empty) {
          e.state = Entry::slot_state::occupied;
          e.key = key;
          e.hash = h;
          e.value = value;
          inserted = true;
          res = e.value;
          break;
        }

        // must check if this is us
        if (e.hash == h && e.key == key &&
            e.state == Entry::slot_state::occupied) {
          return e.value;
        }

        idx = (idx + 1) % data.size();
        i++;
      }

      if (deleted_candidate != data.size() + 1) {
        Entry &e = data.at(deleted_candidate);
        e.state = Entry::slot_state::occupied;
        e.key = key;
        e.hash = h;
        e.value = value;
        inserted = true;
        res = e.value;
      }

      if (inserted) {
        num_entries++;
        check_and_resize();
      }

      return res;
    }

  private:
    struct Entry {
      enum class slot_state { empty, occupied, deleted };
      slot_state state;
      K key;
      V value;
      size_t hash;
    };

    std::vector<Entry> data;
    shared_mutex rw_lock;
    size_t num_entries;

    // only ever call this with a write lock
    // maybe we can be explicit and pass it down here ?
    void resize() {
      std::vector<Entry> new_data(data.size() * 2);

      auto spot = [&](size_t start) -> size_t {
        // shuold never go forever
        size_t idx = start;
        while (true) {
          const auto &e = new_data.at(idx);

          if (e.state == Entry::slot_state::empty)
            return idx;

          idx = (idx + 1) % new_data.size();
        }

        return -1;
      };

      for (const auto &e : data) {
        if (e.state == Entry::slot_state::empty ||
            e.state == Entry::slot_state::deleted)
          continue;
        size_t insertion_idx = spot(e.hash % new_data.size());
        new_data[insertion_idx] = std::move(e);
      }

      data = std::move(new_data);
    }

    // should also only ever be called with write lock
    void check_and_resize() {
      if (static_cast<float>(num_entries) / data.size() > 0.50)
        resize();
    }
  };
  std::vector<Table> tables;
  H hasher;
};
