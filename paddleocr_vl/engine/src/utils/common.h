#ifndef _RKNN_MODEL_ZOO_COMMON_H_
#define _RKNN_MODEL_ZOO_COMMON_H_

/**
 * @brief Image pixel format
 * 
 */
typedef enum {
    IMAGE_FORMAT_GRAY8,
    IMAGE_FORMAT_RGB888,
    IMAGE_FORMAT_RGBA8888,
    IMAGE_FORMAT_YUV420SP_NV21,
    IMAGE_FORMAT_YUV420SP_NV12,
} image_format_t;

/**
 * @brief Image buffer
 * 
 */
typedef struct {
    int width;
    int height;
    int width_stride;
    int height_stride;
    image_format_t format;
    unsigned char* virt_addr;
    int size;
    int fd;
} image_buffer_t;

/**
 * @brief Image rectangle
 * 
 */
typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} image_rect_t;

/**
 * @brief Image obb rectangle
 * 
 */
typedef struct {
    int x;
    int y;
    int w;
    int h;
    float angle;
} image_obb_box_t;

/**
 * @brief Audio buffer
 * 
 */
typedef struct
{
    float *data;
    int num_frames;
    int num_channels;
    int sample_rate;
} audio_buffer_t;

/**
 * @Performance Metrics 
 * 
 */
typedef struct {
    int64_t llm_start_time; // Start timestamp of the LLM inference.
    int64_t llm_end_time;   // End timestamp of the LLM inference.
    int64_t vision_latency; // Latency of the vision module inference.
    int64_t audio_latency; // Latency of the audio module inference.
    int     n_prefill_tokens; // Number of tokens processed during the prefill stage.
    int     n_decode_tokens;  // Number of tokens generated during the decode stage.
} rknn_perf_metrics_t;

#endif //_RKNN_MODEL_ZOO_COMMON_H_
