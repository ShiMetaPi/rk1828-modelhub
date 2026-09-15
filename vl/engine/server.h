// server.h — Unix socket 服务端：单客户端，JSON 行协议（见设计文档第 4 节）
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "capture.h"
#include "infer.h"

namespace vl {

class Server {
public:
    Server(Capture* cap, InferEngine* infer) : cap_(cap), infer_(infer) {}
    ~Server() { Stop(); }

    bool Start(const std::string& sock_path, std::string* err);
    void Stop();
    void RunUntilStopped();  // 阻塞在 accept 循环

private:
    void WriterLoop();
    void PusherLoop();              // ~8fps 推预览帧
    void InferLoop();               // 串行处理提问队列
    void ClientLoop(int fd);        // 读命令（一次一个客户端）
    void Emit(const std::string& line);
    bool EnqueueAsk(const std::string& line);

    Capture* cap_;
    InferEngine* infer_;
    std::string sock_path_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<int> client_fd_{-1};

    std::mutex wmu_;
    std::condition_variable wcv_;
    std::deque<std::string> wq_;  // 待写行

    std::thread writer_, pusher_, infer_th_;
    int64_t next_qid_ = 1;

    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<AskRequest> asks_;
};

}  // namespace vl
