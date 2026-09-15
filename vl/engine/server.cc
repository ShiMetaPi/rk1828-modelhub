// server.cc — socket 服务、事件广播、命令分发
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <json.h>
#include <base64.h>

namespace vl {

bool Server::Start(const std::string& sock_path, std::string* err) {
    sock_path_ = sock_path;
    ::unlink(sock_path_.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { *err = "socket() 失败"; return false; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        *err = std::string("bind 失败: ") + strerror(errno);
        return false;
    }
    if (::listen(listen_fd_, 1) < 0) { *err = "listen 失败"; return false; }

    cap_->SetStatusCallback([this](const std::string& s) {
        Emit(std::string("{\"ev\":\"error\",\"qid\":0,\"msg\":") + json_escape(s) + "}");
    });

    writer_ = std::thread(&Server::WriterLoop, this);
    pusher_ = std::thread(&Server::PusherLoop, this);
    infer_th_ = std::thread(&Server::InferLoop, this);
    return true;
}

void Server::Stop() {
    if (stop_.exchange(true)) return;
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    int fd = client_fd_.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    wcv_.notify_all();
    qcv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (pusher_.joinable()) pusher_.join();
    if (infer_th_.joinable()) infer_th_.join();
    ::unlink(sock_path_.c_str());
}

void Server::RunUntilStopped() {
    while (!stop_) {
        pollfd p{listen_fd_, POLLIN, 0};
        int r = ::poll(&p, 1, 500);
        if (r <= 0) continue;
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        int old = client_fd_.exchange(fd);
        if (old >= 0) ::close(old);  // 新客户端顶掉旧的
        fprintf(stderr, "[server] 客户端已连接 fd=%d\n", fd);
        ClientLoop(fd);  // 当前线程服务这个客户端直到断开
        fprintf(stderr, "[server] 客户端断开\n");
        if (client_fd_.exchange(-1) == fd) ::close(fd);
    }
}

void Server::ClientLoop(int fd) {
    std::string buf;
    char tmp[4096];
    while (!stop_) {
        pollfd p{fd, POLLIN, 0};
        int r = ::poll(&p, 1, 500);
        if (r < 0) break;
        if (r == 0) {
            if (client_fd_.load() != fd) break;  // 被新客户端顶掉
            continue;
        }
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string cmd;
            if (!json_str_field(line, "cmd", &cmd)) continue;
            if (cmd == "ask") {
                EnqueueAsk(line);
            } else if (cmd == "reset") {
                infer_->Reset();
                Emit("{\"ev\":\"error\",\"qid\":0,\"msg\":\"上下文已重置\"}");
            }
        }
    }
}

bool Server::EnqueueAsk(const std::string& line) {
    AskRequest req;
    req.qid = json_int_field(line, "qid", 0);
    if (!json_str_field(line, "text", &req.text) || req.text.empty()) {
        Emit(std::string("{\"ev\":\"error\",\"qid\":") + std::to_string(req.qid) +
             ",\"msg\":\"ask 缺少 text 字段\"}");
        return false;
    }
    {
        std::lock_guard<std::mutex> l(qmu_);
        asks_.push_back(req);
    }
    qcv_.notify_one();
    return true;
}

void Server::InferLoop() {
    while (!stop_) {
        AskRequest req;
        {
            std::unique_lock<std::mutex> l(qmu_);
            qcv_.wait(l, [this] { return stop_ || !asks_.empty(); });
            if (stop_) return;
            req = asks_.front();
            asks_.pop_front();
        }
        // 取"开始推理时刻"的最新帧（提问排队期间画面可能已变化）
        cap_->Latest(&req.jpeg);
        fprintf(stderr, "[infer] qid=%lld 处理（%zu 字节帧）\n",
                (long long)req.qid, req.jpeg.size());

        InferStats st;
        std::string err;
        bool ok = infer_->Run(
            req,
            [this, &req](const std::string& piece) {
                Emit(std::string("{\"ev\":\"token\",\"qid\":") +
                     std::to_string(req.qid) + ",\"text\":" + json_escape(piece) + "}");
            },
            &st, &err,
            [this, &req](const std::string& note) {  // 系统提示（如上下文自动清空）
                Emit(std::string("{\"ev\":\"notice\",\"qid\":") +
                     std::to_string(req.qid) + ",\"text\":" + json_escape(note) + "}");
            });

        char tail[256];
        if (ok) {
            snprintf(tail, sizeof(tail),
                     "{\"ev\":\"done\",\"qid\":%lld,"
                     "\"stats\":{\"vit_ms\":%.1f,\"prefill_ms\":%.1f,"
                     "\"decode_ms\":%.1f,\"tokens\":%d}}",
                     (long long)req.qid, st.vit_ms, st.prefill_ms, st.decode_ms, st.tokens);
            Emit(tail);
        } else {
            Emit(std::string("{\"ev\":\"error\",\"qid\":") + std::to_string(req.qid) +
                 ",\"msg\":" + json_escape(err) + "}");
        }
    }
}

void Server::PusherLoop() {
    // 帧驱动：摄像头每出一帧就推一帧（推理在 RK1828 上，RK3588 只做采转，
    // 用户已确认放开到满帧率 ~25fps）
    std::vector<uint8_t> jpg;
    uint64_t seq = 0;
    while (!stop_) {
        if (!cap_->WaitNext(&jpg, &seq, 500)) continue;  // 500ms 无新帧（摄像头离线）→ 重试
        if (jpg.empty()) continue;
        Emit(std::string("{\"ev\":\"frame\",\"jpeg\":\"") + b64_encode(jpg) + "\"}");
    }
}

void Server::WriterLoop() {
    while (!stop_) {
        std::string batch;
        {
            std::unique_lock<std::mutex> l(wmu_);
            wcv_.wait(l, [this] { return stop_ || !wq_.empty(); });
            if (stop_) return;
            while (!wq_.empty()) {
                batch += wq_.front();
                batch += '\n';
                wq_.pop_front();
            }
        }
        int fd = client_fd_.load();
        if (fd < 0 || batch.empty()) continue;
        size_t off = 0;
        while (off < batch.size()) {
            ssize_t n = ::send(fd, batch.data() + off, batch.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return;  // 客户端断开，丢弃待写
            off += (size_t)n;
        }
    }
}

void Server::Emit(const std::string& line) {
    {
        std::lock_guard<std::mutex> l(wmu_);
        wq_.push_back(line);
        // 防爆队列：只保留最近 64 条（frame 大；客户端慢时丢旧帧没关系）
        while (wq_.size() > 64) wq_.pop_front();
    }
    wcv_.notify_one();
}

}  // namespace vl
