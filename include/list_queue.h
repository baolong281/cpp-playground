#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
template <typename T> class ListQueue {
private:
  struct Node {
    std::shared_ptr<T> data;
    std::unique_ptr<Node> next;
  };

  std::unique_ptr<Node> head;
  std::mutex head_mutex;
  std::mutex tail_mutex;
  Node *tail;

  Node *get_tail() {
    std::lock_guard<std::mutex> tail_lck(tail_mutex);
    return tail;
  }

  std::unique_ptr<Node> pop_head() {
    std::lock_guard<std::mutex> head_lck(head_mutex);

    if (head.get() == get_tail()) {
      return nullptr;
    }

    std::unique_ptr<Node> old_head = std::move(head);
    head = std::move(old_head->next);
    return old_head;
  }

public:
  ListQueue() : head(new Node), tail(head.get()) {}

  ListQueue(const ListQueue<T> &other) = delete;
  ListQueue &operator=(const ListQueue<T> &other) = delete;

  std::shared_ptr<T> try_pop() {
    std::unique_ptr<Node> old_head = pop_head();
    return old_head ? old_head->data : std::shared_ptr<T>();
  }

  void push(T new_value) {
    std::shared_ptr<T> new_data(std::make_shared<T>(std::move(new_value)));
    auto p = std::make_unique<Node>();
    Node *const new_tail = p.get();

    std::lock_guard<std::mutex> tail_lck(tail_mutex);
    tail->data = new_data;
    tail->next = std::move(p);
    tail = new_tail;
  }
};
