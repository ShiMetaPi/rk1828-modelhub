// capture.h — V4L2 MJPG 采集线程：单槽缓冲（丢旧保新），掉线自动重连
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vl {

class Capture {
public:
    // on_status 用于上报"摄像头离线/恢复"这类状态变化（线程安全）。
    void SetStatusCallback(std::function<void(const std::string&)> cb) { on_status_ = cb; }
    // 启动线程。want_card 非空时按设备名匹配（如 "JSK-RGB"），为空取第一个视频采集设备。
    bool Start(const std::string& want_card, std::string* err);
    void Stop();
    // 拷出最新一帧 JPEG（可能返回 false：还没有任何帧）。
    bool Latest(std::vector<uint8_t>* jpg);
    // 阻塞等"下一帧"（seq 变化才返回）。timeout_ms 内没等到返回 false。
    // 调用方自己保存 seq 跨调用传递，实现帧驱动消费（不重复不丢帧）。
    bool WaitNext(std::vector<uint8_t>* jpg, uint64_t* seq, int timeout_ms);
    std::string card() { std::lock_guard<std::mutex> l(mu_); return card_; }
    bool online() { std::lock_guard<std::mutex> l(mu_); return online_; }

private:
    void Loop();                       // 主循环：打开→采集→出错重试（10s）
    std::string Discover(const std::string& want_card);  // 扫 /dev/video* 找采集设备
    bool OpenAndStream(int fd);        // 设置格式+MMap+STREAMON，成功后持续 DQBUF
    void SetFrame(const std::vector<uint8_t>& jpg);
    void SetOnline(bool up, const std::string& card);

    std::thread th_;
    std::atomic<bool> run_{false};
    std::string want_card_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<uint8_t> latest_;   // 单槽最新帧
    uint64_t seq_ = 0;             // 帧序号，每写一帧 +1
    bool has_frame_ = false;
    bool online_ = false;
    std::string card_;
    std::function<void(const std::string&)> on_status_;
};

}  // namespace vl
