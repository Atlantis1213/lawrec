// Uses SDK nncase/AI2D interfaces from examples/image_face_detect; not OpenCV.
#include "detector.h"
#include "config.h"
#include <nncase/runtime/interpreter.h>
#include <nncase/functional/ai2d/ai2d_builder.h>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <cerrno>
#include "mpi_sys_api.h"
#include "mpi_vicap_api.h"

namespace demo {
namespace {
using Clock = std::chrono::steady_clock;
using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::F::k230;
template <typename T> T take(result<T> result, const char *operation) {
    if (!result.is_ok()) throw std::runtime_error(std::string(operation) + ": " + result.unwrap_err().message());
    return std::move(result.unwrap());
}
void require(result<void> result, const char *operation) {
    if (!result.is_ok()) throw std::runtime_error(std::string(operation) + ": " + result.unwrap_err().message());
}
uint32_t micros(Clock::time_point before, Clock::time_point after) {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(after - before).count());
}
}
struct Detector::Impl {
    interpreter model;
    runtime_tensor input;
    std::unique_ptr<ai2d_builder> builder;
};
Detector::Detector() = default;
Detector::~Detector() = default;

int Detector::load(const char *path) {
    if (!path || impl_) return -EINVAL;
    try {
        auto state = std::make_unique<Impl>();
        std::ifstream file(path, std::ios::binary);
        if (!file) return -ENOENT;
        require(state->model.load_model(file), "model load");
        if (state->model.inputs_size() != 1 || state->model.outputs_size() != 9 ||
            state->model.input_shape(0) != dims_t({1, 3, 320, 320}) ||
            state->model.input_desc(0).datatype != typecode_t::dt_uint8) return -EPROTO;
        const unsigned sides[] = {40, 20, 10}, channels[] = {8, 4, 20};
        for (unsigned i = 0; i < 9; ++i) {
            unsigned side = sides[i % 3], channel = channels[i / 3];
            if (state->model.output_shape(i) != dims_t({1, channel, side, side}) ||
                state->model.output_desc(i).datatype != typecode_t::dt_float32) return -EPROTO;
        }
        state->input = take(hrt::create(typecode_t::dt_uint8, {1, 3, 320, 320}, hrt::pool_shared), "model input");
        require(state->model.input_tensor(0, state->input), "bind model input");
        auto pad = letterbox(video_width, video_height);
        ai2d_datatype_t dtype{ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT, typecode_t::dt_uint8, typecode_t::dt_uint8};
        ai2d_crop_param_t crop{false, 0, 0, 0, 0};
        ai2d_shift_param_t shift{false, 0};
        ai2d_pad_param_t padding{true, {{0, 0}, {0, 0},
            {int(pad.top), int(model_side - pad.resized_height - pad.top)},
            {int(pad.left), int(model_side - pad.resized_width - pad.left)}}, ai2d_pad_mode::constant, {0, 0, 0}};
        ai2d_resize_param_t resize{true, ai2d_interp_method::tf_bilinear, ai2d_interp_mode::half_pixel};
        ai2d_affine_param_t affine{};
        dims_t source_shape{1, 3, video_height, video_width}, target_shape{1, 3, 320, 320};
        state->builder = std::make_unique<ai2d_builder>(source_shape,
            target_shape, dtype, crop, shift, padding, resize, affine);
        require(state->builder->build_schedule(), "AI2D schedule");
        std::printf("[vision-ai] MobileRetinaFace RGB NCHW uint8 1280x720 -> 320x320 top/bottom=70/70; model owns mean subtraction\n");
        impl_ = std::move(state);
        return 0;
    } catch (const std::bad_alloc &) { return -ENOMEM; }
    catch (const std::exception &error) { std::printf("[vision-ai] load error=%s\n", error.what()); return -EIO; }
}

int Detector::process(std::vector<Face> &faces, AiTiming &timing) {
    faces.clear(); timing = {};
    if (!impl_) return -EAGAIN;
    auto start = Clock::now();
    k_video_frame_info frame{};
    int error = kd_mpi_vicap_dump_frame(VICAP_DEV_ID_0, VICAP_CHN_ID_1, VICAP_DUMP_YUV, &frame, 200);
    if (error) return error < 0 ? error : -EIO;
    constexpr uint32_t plane = video_width * video_height, bytes = plane * 3;
    void *mapped = nullptr;
    if (frame.v_frame.width != video_width || frame.v_frame.height != video_height ||
        frame.v_frame.pixel_format != PIXEL_FORMAT_RGB_888_PLANAR ||
        frame.v_frame.stride[0] != video_width || !frame.v_frame.phys_addr[0]) error = -EPROTO;
    for (unsigned i = 1; !error && i < 3; ++i) {
        if ((frame.v_frame.stride[i] && frame.v_frame.stride[i] != video_width) ||
            (frame.v_frame.phys_addr[i] && frame.v_frame.phys_addr[i] != frame.v_frame.phys_addr[0] + i * plane)) error = -EPROTO;
    }
    if (!error) {
        mapped = kd_mpi_sys_mmap(frame.v_frame.phys_addr[0], bytes);
        if (!mapped) error = -ENOMEM;
    }
    if (!error) try {
        auto wrapped = take(hrt::create(typecode_t::dt_uint8, {1, 3, video_height, video_width},
            {static_cast<gsl::byte *>(mapped), bytes}, false, hrt::pool_shared, frame.v_frame.phys_addr[0]), "RGB wrap");
        require(hrt::sync(wrapped, sync_op_t::sync_write_back, true), "RGB sync");
        auto ai_start = Clock::now();
        require(impl_->builder->invoke(wrapped, impl_->input), "AI2D invoke");
        auto kpu_start = Clock::now();
        require(impl_->model.run(), "KPU run");
        auto post_start = Clock::now();
        std::array<std::vector<float>, 9> outputs;
        std::array<FaceHead, 9> heads;
        for (unsigned i = 0; i < 9; ++i) {
            auto tensor = take(impl_->model.output_tensor(i), "output tensor");
            auto host = take(tensor.to_host(), "output to host");
            auto view = take(hrt::map(host, map_access_t::map_read), "output map");
            auto data = view.buffer();
            auto desc = impl_->model.output_desc(i);
            if (data.size() != desc.size || data.size() % sizeof(float)) throw std::runtime_error("output size mismatch");
            auto ptr = reinterpret_cast<const float *>(data.data());
            outputs[i].assign(ptr, ptr + data.size() / sizeof(float));
            require(view.unmap(), "output unmap");
            heads[i] = {outputs[i].data(), outputs[i].size()};
        }
        error = decode_faces(heads, video_width, video_height, faces);
        auto finish = Clock::now();
        timing.ai2d_us = micros(ai_start, kpu_start);
        timing.kpu_us = micros(kpu_start, post_start);
        timing.post_us = micros(post_start, finish);
    } catch (const std::bad_alloc &) { error = -ENOMEM; }
    catch (const std::exception &exception) { std::printf("[vision-ai] run error=%s\n", exception.what()); error = -EIO; }
    // The wrapped tensor has died before its mapping/frame can be returned.
    if (mapped) { int ret = kd_mpi_sys_munmap(mapped, bytes); if (!error && ret) error = ret < 0 ? ret : -EIO; }
    int ret = kd_mpi_vicap_dump_release(VICAP_DEV_ID_0, VICAP_CHN_ID_1, &frame);
    if (!error && ret) error = ret < 0 ? ret : -EIO;
    timing.total_us = micros(start, Clock::now());
    return error;
}
}
