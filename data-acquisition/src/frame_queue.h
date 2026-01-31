/**
 * @file frame_queue.h
 * @brief Thread-safe queue for frame metadata
 * 
 * This file contains the declaration and implementation of the frame_queue
 * class, which provides a thread-safe queue for passing frame metadata
 * between the capture thread and storage workers.
 */

// ============================================================================
// https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523

/**
 * @brief Maximum queue size
 */
int const max_queue_size = 3000;

/**
 * @brief Thread-safe queue for frame metadata
 * 
 * This class provides a thread-safe queue for passing frame metadata
 * between the capture thread and storage workers.
 */
class frame_queue {
public:
  /**
   * @brief Exception thrown when queue is cancelled
   */
  struct cancelled {};

public:
  /**
   * @brief Constructor
   */
  frame_queue();

  /**
   * @brief Push a frame to the queue
   * @param image Frame metadata to push
   */
  void push(MatMeta const &image);
  
  /**
   * @brief Pop a frame from the queue
   * @return Frame metadata
   */
  MatMeta pop();

  /**
   * @brief Cancel the queue
   * 
   * This method cancels the queue, causing any blocked pop operations to throw
   * the cancelled exception.
   */
  void cancel();

  /**
   * @brief Get the current queue size
   * @return Current number of elements in the queue
   */
  int size();

private:
  std::queue<MatMeta> queue_;
  std::mutex mutex_;
  std::condition_variable cond_;
  bool cancelled_;
};
// ----------------------------------------------------------------------------
/**
 * @brief Constructor implementation
 */
frame_queue::frame_queue() : cancelled_(false) {}
// ----------------------------------------------------------------------------
/**
 * @brief Cancel implementation
 * 
 * Sets the cancelled flag and notifies all waiting threads.
 */
void frame_queue::cancel() {
  std::unique_lock<std::mutex> mlock(mutex_);
  cancelled_ = true;
  cond_.notify_all();
}
// ----------------------------------------------------------------------------
/**
 * @brief Push implementation
 * 
 * Adds a frame to the queue if it's not full. If the queue is full, it logs
 * an error and sets the global error flag.
 * @param image Frame metadata to add
 */
void frame_queue::push(MatMeta const &image) {
  if (queue_.size() <= max_queue_size) {
    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.push(image);
    cond_.notify_one();
  } else {
    std::cout << "FATAL ERROR | " << get_timestamp() << " | Maximum queue size "
              << max_queue_size << " reached. Discarding data!" << std::endl;
    global_error = true;
  }
}
// ----------------------------------------------------------------------------
/**
 * @brief Pop implementation
 * 
 * Removes and returns a frame from the queue. If the queue is empty, it waits
 * until a frame is available or until the queue is cancelled.
 * @return Frame metadata
 * @throws cancelled if the queue has been cancelled
 */
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

  MatMeta image(queue_.front());
  queue_.pop();
  return image;
}
// ============================================================================
/**
 * @brief Size implementation
 * @return Current queue size
 */
int frame_queue::size() { return queue_.size(); }
