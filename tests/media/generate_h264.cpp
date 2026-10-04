// Generates a short real 720p H264 stream with the SDK's test-only x264 build.
#include <cstdint>
#include <x264.h>
#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    assert(argc == 2);
    x264_param_t params;
    assert(!x264_param_default_preset(&params, "ultrafast", "zerolatency"));
    params.i_width = 1280; params.i_height = 720; params.i_csp = X264_CSP_I420;
    params.i_fps_num = 30; params.i_fps_den = 1; params.i_keyint_max = 15;
    params.i_bframe = 0; params.b_repeat_headers = 1; params.b_annexb = 1; params.b_aud = 1;
    params.i_threads = 1;
    assert(!x264_param_apply_profile(&params, "high"));
    auto *encoder = x264_encoder_open(&params); assert(encoder);
    x264_picture_t picture{}, output{};
    assert(!x264_picture_alloc(&picture, X264_CSP_I420, 1280, 720));
    std::memset(picture.img.plane[0], 16, picture.img.i_stride[0] * 720);
    std::memset(picture.img.plane[1], 128, picture.img.i_stride[1] * 360);
    std::memset(picture.img.plane[2], 128, picture.img.i_stride[2] * 360);
    FILE *file = fopen(argv[1], "wb"); assert(file);
    auto write = [&](x264_picture_t *input) {
        x264_nal_t *nals = nullptr; int count = 0;
        assert(x264_encoder_encode(encoder, &nals, &count, input, &output) >= 0);
        for (int i = 0; i < count; ++i)
            assert(fwrite(nals[i].p_payload, 1, nals[i].i_payload, file) == unsigned(nals[i].i_payload));
    };
    for (int group = 0; group < 2; ++group) {
        if (group) {
            x264_encoder_close(encoder);
            params.i_sps_id = group;
            encoder = x264_encoder_open(&params); assert(encoder);
        }
        for (int i = 0; i < 15; ++i) { picture.i_pts = i; write(&picture); }
        assert(!x264_encoder_delayed_frames(encoder));
    }
    while (x264_encoder_delayed_frames(encoder)) write(nullptr);
    assert(!fclose(file)); x264_picture_clean(&picture); x264_encoder_close(encoder);
}
