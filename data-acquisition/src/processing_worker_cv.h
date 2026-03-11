/**
 * @file processing_worker_cv.h
 * @brief Processing worker class for OpenCV video processing
 * 
 * This file contains the declaration and implementation of the processing_worker_cv
 * class, which handles processing video frames before storage.
 */

// ============================================================================
// https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523

// ============================================================================

/**
 * @brief Processing worker class for OpenCV video processing
 * 
 * This class handles processing video frames before storage, including
 * preprocessing steps and coordination with storage workers.
 */
class processing_worker_cv {
public:
  /**
   * @brief Constructor
   * @param queue Reference to the frame queue
   * @param id Worker ID
   * @param path Output path
   * @param fourcc Four-character code for video codec
   * @param fps Frames per second
   * @param frame_size Size of frames
   * @param is_color Whether frames are color
   * @param t_reset Time reset point
   */
  processing_worker_cv(
      frame_queue &queue, int32_t id, std::string const &path, int32_t fourcc,
      double fps, cv::Size frame_size,
      bool is_color
      //        , double quality
      //        , double preset
      ,
      std::chrono::time_point<std::chrono::system_clock> t_reset);
  /**
   * @brief Run method - main execution loop
   */
  void run();

  /**
   * @brief Get total time in milliseconds
   * @return Total time spent in processing operations
   */
  double total_time_ms() const { return total_time_ / 1000.0; }

  // Let's create our storage workers -- let's have two, to simulate your
  // scenario and to keep it interesting, have each one write a different format
  std::vector<storage_worker_cv> storage;
  std::vector<std::thread> storage_thread;

private:
  frame_queue &queue_;

  int32_t id_;
  std::ofstream fMeta_;

  std::string path_;
  std::string filename_;
  int32_t fourcc_;
  double fps_;
  cv::Size frame_size_;
  bool is_color_;
  //    double quality_;
  //    double preset_;
  double total_time_;
  std::chrono::time_point<std::chrono::system_clock> t_reset_;
  unsigned long t_reset_uint_;
  char hostname_[HOST_NAME_MAX];

  cv::VideoWriter writer_;

  MatMeta processedImage;

  /**
   * @brief Add metadata to output file
   */
  void add_meta_data();
  /**
   * @brief Close output files
   */
  void close_files();
  /**
   * @brief Open output files
   */
  void open_files();
  /**
   * @brief Create filename for output
   */
  void create_filename();
};
// ----------------------------------------------------------------------------
/**
 * @brief Constructor implementation
 * @param queue Reference to the frame queue
 * @param id Worker ID
 * @param path Output path
 * @param fourcc Four-character code for video codec
 * @param fps Frames per second
 * @param frame_size Size of frames
 * @param is_color Whether frames are color
 * @param t_reset Time reset point
 */
processing_worker_cv::processing_worker_cv(
    frame_queue &queue, int32_t id, std::string const &path, int32_t fourcc,
    double fps, cv::Size frame_size,
    bool is_color
    //    , double quality
    //    , double preset
    ,
    std::chrono::time_point<std::chrono::system_clock> t_reset)
    : queue_(queue), id_(id), path_(path), fourcc_(fourcc), fps_(fps),
      frame_size_(frame_size), is_color_(is_color), total_time_(0.0)
      //    , quality_(quality)
      //    , preset_(preset)
      ,
      t_reset_(t_reset) {}
// //
// ----------------------------------------------------------------------------

/**
 * @brief Run method - main execution loop
 * 
 * This method runs in a separate thread and processes frames from the queue,
 * performing preprocessing before passing them to storage workers.
 */
void processing_worker_cv::run()

{
  nice(-15);
  // The synchronized queues, one per video source/storage worker pair
  std::vector<frame_queue> queue_writer((int)1);
  int fps_int = cvCeil(fps_);

  storage.emplace_back(std::ref(queue_writer[0]), 100, path_, fourcc_, fps_,
                       frame_size_,
                       is_color_
                       //, captureContext->quality
                       //, captureContext->preset
                       ,
                       t_reset_);

  // And start the worker threads for each storage worker
  for (auto &s : storage) {
    storage_thread.emplace_back(&storage_worker_cv::run, &s);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  try {
    int32_t frame_count(0);
    for (;;) {
      MatMeta image(queue_.pop());
      if (!image.MatImage.empty()) {
        high_resolution_clock::time_point t1(high_resolution_clock::now());

        ++frame_count;
        processedImage.MatImage = image.MatImage.clone();
        processedImage.timestamp = image.timestamp;
        processedImage.id = image.id;

        // Insert a copy into all queues
        for (auto &q : queue_writer) {
          q.push(processedImage);
        }

        if (frame_count % fps_int == 0) {
          std::cout << "STATUS | " << get_timestamp()
                    << " | Proc. & Stor queues: " << queue_.size() << " & "
                    << queue_writer[0].size()
                    << " | Mean img: " << cv::mean(image.MatImage)[0] << "\r"
                    << std::flush;
        }

        high_resolution_clock::time_point t2(high_resolution_clock::now());
        double dt_us(
            static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
        total_time_ += dt_us;

        // to do : move to thread?
        // https://stackoverflow.com/questions/21126950/asynchronously-writing-to-a-file-in-c-unix

        // std::cout << "Worker " << id_ << " stored image.MatImage #" <<
        // frame_count
        //     << " in " << (dt_us / 1000.0) << " ms" << std::endl;
      }
    }
  } catch (frame_queue::cancelled & /*e*/) {
    // Nothing more to process, we're done

    queue_writer[0].cancel();
    std::cout << "Processing Queue " << id_ << " cancelled, worker finished."
              << std::endl;

    // And join all the worker threads, waiting for them to finish
    storage_thread[0].join();
  }
}
