// main.cc — 引擎入口。
// 服务模式: vl_engine [--sock /tmp/vl_engine.sock] [--device JSK-RGB] [--mock]
// CLI 模式:  vl_engine --cli --image frame.jpg --prompt "画面里有什么" [--mock]
// --mock 用假推理点亮链路（默认）；真实后端等模型包修复后接入（--real 预留）。
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <string>

#include "capture.h"
#include "infer.h"
#include "infer_mock.h"
#include "infer_real.h"
#include "server.h"

namespace {

void Usage(const char* argv0) {
    fprintf(stderr,
            "用法:\n"
            "  %s [--sock PATH] [--device CARD] [--mock | --real]     服务模式\n"
            "  %s --cli --image FILE --prompt TEXT [--mock]           单次推理\n"
            "默认: --sock /tmp/vl_engine.sock --mock --device auto\n",
            argv0, argv0);
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    std::string sock = "/tmp/vl_engine.sock";
    std::string device, prompt, image;
    std::string model = "/userdata/models/qwen2.5-vl-3b";
    bool cli = false, mock = false, real = false;

    static option longopts[] = {
        {"sock",   required_argument, nullptr, 's'},
        {"device", required_argument, nullptr, 'd'},
        {"cli",    no_argument,       nullptr, 'c'},
        {"image",  required_argument, nullptr, 'i'},
        {"prompt", required_argument, nullptr, 'p'},
        {"mock",   no_argument,       nullptr, 'm'},
        {"real",   no_argument,       nullptr, 'r'},
        {"model",  required_argument, nullptr, 'M'},
        {nullptr, 0, nullptr, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "", longopts, nullptr)) != -1) {
        switch (ch) {
            case 's': sock = optarg; break;
            case 'd': device = optarg; break;
            case 'c': cli = true; break;
            case 'i': image = optarg; break;
            case 'p': prompt = optarg; break;
            case 'm': mock = true; break;
            case 'r': real = true; break;
            case 'M': model = optarg; break;
            default: Usage(argv[0]); return 2;
        }
    }
    if (!mock && !real) mock = true;  // 默认 mock（链路联调用）
    if (cli && prompt.empty()) {
        fprintf(stderr, "CLI 模式需要 --prompt\n");
        return 2;
    }

    vl::InferEngine* infer = nullptr;
    if (real) {
        vl::RealInfer* ri = new vl::RealInfer(model);
        if (!cli) ri->SetFlip180(true);  // 服务模式：摄像头物理倒置；CLI 正立图不翻
        infer = ri;
    } else {
        infer = new vl::MockInfer();
    }
    std::string err;
    if (!infer->Init(&err)) {
        fprintf(stderr, "推理后端初始化失败: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[engine] 后端: %s\n", infer->name().c_str());

    if (cli) {
        vl::AskRequest req;
        req.qid = 1;
        req.text = prompt;
        if (!image.empty()) {
            req.jpeg = ReadFile(image);
            if (req.jpeg.empty()) {
                fprintf(stderr, "读不到图片: %s\n", image.c_str());
                return 1;
            }
        }
        vl::InferStats st;
        bool ok = infer->Run(req, [](const std::string& s) {
            fputs(s.c_str(), stdout);
            fflush(stdout);
        }, &st, &err);
        printf("\n");
        if (!ok) {
            fprintf(stderr, "推理失败: %s\n", err.c_str());
            return 1;
        }
        fprintf(stderr, "[stats] vit=%.1fms prefill=%.1fms decode=%.1fms tokens=%d\n",
                st.vit_ms, st.prefill_ms, st.decode_ms, st.tokens);
        return 0;
    }

    // 服务模式
    vl::Capture cap;
    cap.Start(device, &err);
    vl::Server server(&cap, infer);
    if (!server.Start(sock, &err)) {
        fprintf(stderr, "服务启动失败: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[engine] 服务就绪: %s（Ctrl-C 退出）\n", sock.c_str());
    server.RunUntilStopped();
    server.Stop();
    cap.Stop();
    delete infer;
    return 0;
}
