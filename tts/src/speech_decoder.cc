#include "speech_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdexcept>

#include "float16.h"
#include "rknn3_api.h"

#ifndef LOGE
#define LOGE(fmt, ...) printf("[Qwen3TTSSpeechDecoder][E] " fmt "\n", ##__VA_ARGS__)
#endif

struct Qwen3TTSSpeechDecoder::Impl {
    rknn3_context rknn_ctx = 0;
    rknn3_input_output_num io_num;
    rknn3_tensor* inputs = NULL;
    rknn3_tensor* outputs = NULL;
};

Qwen3TTSSpeechDecoder::Qwen3TTSSpeechDecoder() : impl_(NULL) {}

Qwen3TTSSpeechDecoder::~Qwen3TTSSpeechDecoder() {
    Release();
}

int Qwen3TTSSpeechDecoder::Init(const Config& config) {
    Release();

    if (config.model_dir.empty()) {
        LOGE("Init: model_dir is empty");
        return -1;
    }

    Impl* impl = new Impl();
    impl_ = impl;

    rknn3_config rknn_config;
    memset(&rknn_config, 0, sizeof(rknn_config));
    rknn_config.run_core_mask = 0xff;

    rknn3_init_extend init_extend;
    memset(&init_extend, 0, sizeof(init_extend));
    if (!config.device_id.empty()) {
        init_extend.device_id = const_cast<char*>(config.device_id.c_str());
    }

    int ret = rknn3_init(&impl->rknn_ctx, &init_extend);
    if (ret < 0) {
        LOGE("Init: rknn3_init failed");
        Release();
        return -1;
    }

    std::string model_path = config.model_dir + "/" + config.model_name + ".rknn";
    std::string weight_path = config.model_dir + "/" + config.model_name + ".weight";
    ret = rknn3_load_model_from_path(impl->rknn_ctx, model_path.c_str(), weight_path.c_str());
    if (ret < 0) {
        LOGE("Init: rknn3_load_model_from_path failed");
        Release();
        return -1;
    }

    ret = rknn3_model_init(impl->rknn_ctx, &rknn_config);
    if (ret < 0) {
        LOGE("Init: rknn3_model_init failed");
        Release();
        return -1;
    }

    ret = rknn3_query(impl->rknn_ctx, RKNN3_QUERY_IN_OUT_NUM, &impl->io_num, sizeof(impl->io_num));
    if (ret < 0) {
        LOGE("Init: rknn3_query io_num failed");
        Release();
        return -1;
    }

    impl->inputs = (rknn3_tensor*)calloc(impl->io_num.n_input, sizeof(rknn3_tensor));
    impl->outputs = (rknn3_tensor*)calloc(impl->io_num.n_output, sizeof(rknn3_tensor));
    if (impl->inputs == NULL || impl->outputs == NULL) {
        LOGE("Init: failed to allocate tensor arrays");
        Release();
        return -1;
    }

    for (uint32_t i = 0; i < impl->io_num.n_input; ++i) {
        rknn3_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        ret = rknn3_query(impl->rknn_ctx, RKNN3_QUERY_INPUT_ATTR, &attr, sizeof(attr));
        if (ret < 0) {
            LOGE("Init: query input attr failed");
            Release();
            return -1;
        }
        impl->inputs[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        memcpy(impl->inputs[i].attr, &attr, sizeof(attr));
        impl->inputs[i].mem = rknn3_create_mem(impl->rknn_ctx, attr.aligned_size, attr.core_id, RKNN3_FLAG_MEMORY_CACHEABLE);
    }

    for (uint32_t i = 0; i < impl->io_num.n_output; ++i) {
        rknn3_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        ret = rknn3_query(impl->rknn_ctx, RKNN3_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (ret < 0) {
            LOGE("Init: query output attr failed");
            Release();
            return -1;
        }
        impl->outputs[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        memcpy(impl->outputs[i].attr, &attr, sizeof(attr));
        impl->outputs[i].mem = rknn3_create_mem(impl->rknn_ctx, attr.aligned_size, attr.core_id, RKNN3_FLAG_MEMORY_CACHEABLE);
    }

    return 0;
}

int Qwen3TTSSpeechDecoder::Decode(const std::vector<int32_t>& codes, std::vector<float>* audio_values) {
    if (impl_ == NULL || audio_values == NULL) {
        LOGE("Decode: decoder is not initialized");
        return -1;
    }

    int input_size = impl_->inputs[0].attr->shape[0] * impl_->inputs[0].attr->shape[1] * impl_->inputs[0].attr->shape[2];
    if ((int)codes.size() != input_size) {
        LOGE("Decode: codes size mismatch, expect=%d actual=%zu", input_size, codes.size());
        return -1;
    }

    memcpy(impl_->inputs[0].mem->virt_addr, codes.data(), input_size * sizeof(int32_t));
    for (uint32_t i = 0; i < impl_->io_num.n_input; ++i) {
        int ret = rknn3_mem_sync(impl_->rknn_ctx, impl_->inputs[i].mem, RKNN3_MEMORY_SYNC_TO_DEVICE);
        if (ret != RKNN3_SUCCESS) {
            LOGE("Decode: rknn3_mem_sync input failed");
            return -1;
        }
    }

    int ret = rknn3_run(impl_->rknn_ctx, impl_->inputs, impl_->io_num.n_input, impl_->outputs, impl_->io_num.n_output);
    if (ret != RKNN3_SUCCESS) {
        LOGE("Decode: rknn3_run failed");
        return -1;
    }

    for (uint32_t i = 0; i < impl_->io_num.n_output; ++i) {
        ret = rknn3_mem_sync(impl_->rknn_ctx, impl_->outputs[i].mem, RKNN3_MEMORY_SYNC_FROM_DEVICE);
        if (ret != RKNN3_SUCCESS) {
            LOGE("Decode: rknn3_mem_sync output failed");
            return -1;
        }
    }

    audio_values->clear();
    audio_values->reserve(impl_->outputs[0].attr->n_elems);
    float16* float16_ptr = (float16*)impl_->outputs[0].mem->virt_addr;
    for (uint32_t i = 0; i < impl_->outputs[0].attr->n_elems; ++i) {
        audio_values->push_back(fp16_to_fp32(float16_ptr[i]));
    }
    return 0;
}

void Qwen3TTSSpeechDecoder::Release() {
    if (impl_ == NULL) {
        return;
    }

    if (impl_->inputs != NULL) {
        for (uint32_t i = 0; i < impl_->io_num.n_input; ++i) {
            if (impl_->inputs[i].mem != NULL) {
                rknn3_destroy_mem(impl_->rknn_ctx, impl_->inputs[i].mem);
            }
            free(impl_->inputs[i].attr);
        }
        free(impl_->inputs);
    }

    if (impl_->outputs != NULL) {
        for (uint32_t i = 0; i < impl_->io_num.n_output; ++i) {
            if (impl_->outputs[i].mem != NULL) {
                rknn3_destroy_mem(impl_->rknn_ctx, impl_->outputs[i].mem);
            }
            free(impl_->outputs[i].attr);
        }
        free(impl_->outputs);
    }

    if (impl_->rknn_ctx != 0) {
        rknn3_destroy(impl_->rknn_ctx);
    }

    delete impl_;
    impl_ = NULL;
}
