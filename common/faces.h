#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace demo {
constexpr unsigned model_side = 320, max_faces = 32;
struct Point { float x = 0, y = 0; };
struct Face {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0, score = 0;
    std::array<Point, 5> landmarks{};
};
struct Anchor { float x, y, w, h; };
struct FaceHead { const float *data = nullptr; size_t count = 0; };
struct Letterbox {
    unsigned width, height, resized_width, resized_height, left, top;
    float scale_x, scale_y;
};
Letterbox letterbox(unsigned width, unsigned height);
Anchor anchor(unsigned level, unsigned cell, unsigned variant);
Point portrait_point(Point point, unsigned width, unsigned height);
Face portrait_face(const Face &face, unsigned width, unsigned height);
float face_iou(const Face &a, const Face &b);
// Heads are loc[0..2], logits[3..5], landmarks[6..8], all NCHW float32.
int decode_faces(const std::array<FaceHead, 9> &heads, unsigned width, unsigned height,
                 std::vector<Face> &faces, float confidence = 0.35f, float nms = 0.5f);
}
