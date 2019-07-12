
// ============================================================================
//https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523
class frame_queue
{
public:
    struct cancelled {};

public:
    frame_queue();

    void push(MatMeta const& image);
    MatMeta pop();

    void cancel();

private:
    std::queue<MatMeta> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool cancelled_;
};
// ----------------------------------------------------------------------------
frame_queue::frame_queue()
    : cancelled_(false)
{
}
// ----------------------------------------------------------------------------
void frame_queue::cancel()
{
    std::unique_lock<std::mutex> mlock(mutex_);
    cancelled_ = true;
    cond_.notify_all();
}
// ----------------------------------------------------------------------------
void frame_queue::push(MatMeta const& image)
{
    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.push(image);
    cond_.notify_one();
}
// ----------------------------------------------------------------------------
MatMeta frame_queue::pop()
{
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
