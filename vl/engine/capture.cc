// capture.cc — V4L2 采集实现。参考板上实测：USB 相机是 /dev/video20（节点号重启会变，
// 所以按 card 名发现而不是写死）。板载 ISP 节点(video0~19)不带 MJPG UVC 采集能力，
// 枚举时用 VIDIOC_QUERYCAP 过滤 V4L2_CAP_VIDEO_CAPTURE 且非 MPLANE 的节点。
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <linux/videodev2.h>

namespace vl {
namespace {

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

std::string ErrNo(const char* what) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s", what, strerror(errno));
    return buf;
}

// 查询节点：是普通(单平面)视频采集设备则填 card 并返回 true
bool QueryCaptureNode(const std::string& dev, std::string* card) {
    int fd = open(dev.c_str(), O_RDWR);
    if (fd < 0) return false;
    v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool ok = false;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        uint32_t need = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        if ((cap.device_caps & need) == need) {  // 单平面采集（UVC 都是）
            *card = (const char*)cap.card;
            ok = true;
        }
    }
    close(fd);
    return ok;
}

struct MmapBuf { void* start; size_t len; };

}  // namespace

std::string Capture::Discover(const std::string& want) {
    DIR* d = opendir("/dev");
    if (!d) return "";
    std::string found, found_card;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strncmp(e->d_name, "video", 5) != 0) continue;
        std::string dev = std::string("/dev/") + e->d_name;
        std::string card;
        if (!QueryCaptureNode(dev, &card)) continue;
        if (!want.empty() && card.find(want) == std::string::npos) continue;
        // want 为空时跳过板载 ISP（rkisp/rkisp 虚拟设备名里带 "rkisp" 或 "rkcif"）
        if (want.empty() && (card.find("rkisp") != std::string::npos ||
                             card.find("rkcif") != std::string::npos)) continue;
        found = dev; found_card = card;
        break;  // 取第一个匹配
    }
    closedir(d);
    if (!found.empty()) {
        fprintf(stderr, "[capture] 使用设备 %s（%s）\n", found.c_str(), found_card.c_str());
    }
    return found;
}

bool Capture::Start(const std::string& want_card, std::string* err) {
    want_card_ = want_card;
    if (Discover(want_card_).empty()) {
        char buf[128];
        snprintf(buf, sizeof(buf), "未找到采集设备%s%s",
                 want_card_.empty() ? "" : "（匹配 ", want_card_.empty() ? "" : "）");
        // 不算致命：线程里会持续重试（支持热插）
        fprintf(stderr, "[capture] %s，进入重试\n", buf);
    }
    run_ = true;
    th_ = std::thread(&Capture::Loop, this);
    return true;
}

void Capture::Stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
}

void Capture::SetFrame(const std::vector<uint8_t>& jpg) {
    {
        std::lock_guard<std::mutex> l(mu_);
        latest_ = jpg;
        has_frame_ = true;
        ++seq_;
    }
    cv_.notify_all();
}

void Capture::SetOnline(bool up, const std::string& card) {
    bool changed;
    {
        std::lock_guard<std::mutex> l(mu_);
        changed = (online_ != up);
        online_ = up;
        card_ = card;
    }
    if (changed && on_status_) {
        on_status_(up ? ("摄像头已连接：" + card) : "摄像头离线");
    }
}

bool Capture::Latest(std::vector<uint8_t>* jpg) {
    std::lock_guard<std::mutex> l(mu_);
    if (!has_frame_) return false;
    *jpg = latest_;
    return true;
}

bool Capture::WaitNext(std::vector<uint8_t>* jpg, uint64_t* seq, int timeout_ms) {
    std::unique_lock<std::mutex> l(mu_);
    if (!cv_.wait_for(l, std::chrono::milliseconds(timeout_ms),
                      [this, seq] { return has_frame_ && seq_ != *seq; })) {
        return false;  // 超时：期间没有新帧
    }
    *jpg = latest_;
    *seq = seq_;
    return true;
}

void Capture::Loop() {
    while (run_) {
        std::string dev = Discover(want_card_);
        if (dev.empty()) {
            SetOnline(false, "");
            for (int i = 0; i < 100 && run_; ++i) usleep(100 * 1000);  // 10s 重试
            continue;
        }
        int fd = open(dev.c_str(), O_RDWR);
        if (fd < 0) {
            SetOnline(false, "");
            for (int i = 0; i < 100 && run_; ++i) usleep(100 * 1000);
            continue;
        }
        if (!OpenAndStream(fd)) {
            close(fd);
            SetOnline(false, "");
            for (int i = 0; i < 100 && run_; ++i) usleep(100 * 1000);
            continue;
        }
    }
}

bool Capture::OpenAndStream(int fd) {
    // MJPG 1080p（设备不支持时让驱动挑一个最接近的）
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 1920;
    fmt.fmt.pix.height = 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[capture] S_FMT: %s\n", ErrNo("VIDIOC_S_FMT").c_str());
        return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "[capture] 设备不给 MJPG（fourcc=0x%08x），换 YUYV 也先收着\n",
                fmt.fmt.pix.pixelformat);
    }
    v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    xioctl(fd, VIDIOC_S_PARM, &parm);

    // 4 个 MMap 缓冲
    v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        fprintf(stderr, "[capture] %s\n", ErrNo("VIDIOC_REQBUFS").c_str());
        return false;
    }
    std::vector<MmapBuf> bufs(rb.count);
    for (uint32_t i = 0; i < rb.count; ++i) {
        v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.index = i;
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            fprintf(stderr, "[capture] %s\n", ErrNo("VIDIOC_QUERYBUF").c_str());
            return false;
        }
        bufs[i].len = b.length;
        bufs[i].start = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                             b.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            fprintf(stderr, "[capture] mmap 失败\n");
            return false;
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            fprintf(stderr, "[capture] %s\n", ErrNo("VIDIOC_QBUF").c_str());
            return false;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[capture] %s\n", ErrNo("VIDIOC_STREAMON").c_str());
        return false;
    }
    v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    std::string card = (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
                           ? std::string((const char*)cap.card)
                           : std::string("camera");
    SetOnline(true, card);
    fprintf(stderr, "[capture] 开始采集 %ux%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // DQBUF 主循环（select 2s 超时 = 掉线探测）
    int consecutive_err = 0;
    while (run_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv{2, 0};
        int r = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (r == 0) {
            fprintf(stderr, "[capture] 2 秒无帧，视为掉线\n");
            break;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            fprintf(stderr, "[capture] %s\n", ErrNo("VIDIOC_DQBUF").c_str());
            if (++consecutive_err > 5) break;
            continue;
        }
        consecutive_err = 0;
        const uint8_t* p = (const uint8_t*)bufs[b.index].start;
        // 校验 JPEG 魔数，避免把坏帧塞给下游
        if (b.bytesused > 2 && p[0] == 0xFF && p[1] == 0xD8) {
            SetFrame(std::vector<uint8_t>(p, p + b.bytesused));
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) break;
    }

    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& buf : bufs) munmap(buf.start, buf.len);
    SetOnline(false, "");
    return true;  // 返回 true 只表示"这轮结束了"，外层决定重连
}

}  // namespace vl
