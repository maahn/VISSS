/**
 * @file frame_queue.h
 * @brief Thread-safe queue for frame metadata
 */
int const max_queue_size = 3000;

class frame_queue {
public:
  struct cancelled {};

  frame_queue();
  void push(MatMeta &&image);  // Accept by rvalue reference, not const reference
  MatMeta pop();
  void cancel();
  int size();

private:
  std::queue<MatMeta> queue_;
  std::mutex mutex_;
  std::condition_variable cond_;
  bool cancelled_;
  std::atomic<int> size_estimate_;  // Lock-free size counter
};

// Constructor
frame_queue::frame_queue() : cancelled_(false), size_estimate_(0) {}

// Cancel
void frame_queue::cancel() {
  std::unique_lock<std::mutex> mlock(mutex_);
  cancelled_ = true;
  cond_.notify_all();
}

void frame_queue::push(MatMeta &&image) {
  if (size_estimate_.load(std::memory_order_relaxed) <= max_queue_size) {
    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.push(std::move(image));  // Move instead of copy
    size_estimate_.fetch_add(1, std::memory_order_relaxed);
    mlock.unlock();
    cond_.notify_one();
  } else {
    std::cout << "FATAL ERROR | " << get_timestamp() << " | Maximum queue size "
              << max_queue_size << " reached. Discarding data!" << std::endl;
    global_error = true;
  }
}

// Pop - FIXED
MatMeta frame_queue::pop() {
  std::unique_lock<std::mutex> mlock(mutex_);
  while (queue_.empty()) {
    if (cancelled_) {
      throw cancelled();
    }
    cond_.wait(mlock);
    if (cancelled_) {
      throw cancelled();
    }
  }
  MatMeta image(std::move(queue_.front()));
  queue_.pop();
  size_estimate_.fetch_sub(1, std::memory_order_relaxed);  // Decrement after pop
  return image;
}

// Size - lock-free
int frame_queue::size() { 
  return size_estimate_.load(std::memory_order_relaxed);
}