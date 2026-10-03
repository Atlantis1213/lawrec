#include "faces.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

extern float anchors320[4200][4]; // Actual SDK reference table, not a mock table.
int main() {
    auto pad = demo::letterbox(1280, 720);
    assert(pad.resized_width == 320 && pad.resized_height == 180 && pad.top == 70 && pad.left == 0);
    unsigned index = 0;
    for (unsigned level = 0; level < 3; ++level) {
        unsigned cells = (40U >> level) * (40U >> level);
        for (unsigned cell = 0; cell < cells; ++cell) for (unsigned v = 0; v < 2; ++v) {
            auto prior = demo::anchor(level, cell, v);
            const float values[] = {prior.x, prior.y, prior.w, prior.h};
            for (unsigned j = 0; j < 4; ++j) assert(std::abs(values[j] - anchors320[index][j]) < .000002f);
            ++index;
        }
    }
    assert(index == 4200);
    std::array<std::vector<float>, 9> data;
    std::array<demo::FaceHead, 9> heads;
    for (unsigned i = 0; i < 9; ++i) {
        unsigned cells = (40U >> (i % 3)) * (40U >> (i % 3));
        unsigned channels = i / 3 == 0 ? 8 : i / 3 == 1 ? 4 : 20;
        data[i].resize(cells * channels, 0);
        if (i / 3 == 1) for (unsigned v = 0; v < 2; ++v)
            for (unsigned cell = 0; cell < cells; ++cell) data[i][(v * 2 + 1) * cells + cell] = -1000;
        heads[i] = {data[i].data(), data[i].size()};
    }
    std::vector<demo::Face> faces;
    assert(demo::decode_faces(heads, 1280, 720, faces) == 0 && faces.empty());
    // First head, anchor 0, central cell. logits/coords are channel-first.
    unsigned cell = 20 * 40 + 20, cells = 1600;
    data[3][cells + cell] = 1000;
    assert(demo::decode_faces(heads, 1280, 720, faces) == 0 && faces.size() == 1);
    assert(faces[0].score == 1 && std::abs(faces[0].landmarks[0].x - 656) < .01f);
    assert(std::abs(faces[0].landmarks[0].y - 376) < .01f);
    assert(std::abs(faces[0].x1 - 624) < .01f && std::abs(faces[0].y1 - 344) < .01f);
    // Duplicate anchor in variant 1; shrink it to the first anchor size for NMS.
    data[3][3 * cells + cell] = 900;
    data[0][6 * cells + cell] = data[0][7 * cells + cell] = std::log(.5f) / .2f;
    assert(demo::decode_faces(heads, 1280, 720, faces) == 0 && faces.size() == 1);
    data[3][cells + cell] = std::numeric_limits<float>::quiet_NaN();
    assert(demo::decode_faces(heads, 1280, 720, faces) == 0 && faces.size() == 1);
    data[0][6 * cells + cell] = std::numeric_limits<float>::infinity();
    assert(demo::decode_faces(heads, 1280, 720, faces) == 0 && faces.empty());
    heads[8].count--;
    assert(demo::decode_faces(heads, 1280, 720, faces) < 0);
    assert(demo::decode_faces(heads, 0, 720, faces) < 0);
    auto top_left = demo::portrait_point({0, 0}, 1280, 720);
    auto bottom_right = demo::portrait_point({1280, 720}, 1280, 720);
    assert(top_left.x == 479 && top_left.y == 0);
    assert(bottom_right.x == 0 && bottom_right.y == 799);
    demo::Face input{128, 72, 256, 144, 1, {}};
    auto rotated = demo::portrait_face(input, 1280, 720);
    assert(rotated.x1 == 384 && rotated.x2 == 432 && rotated.y1 == 80 && rotated.y2 == 160);
    assert(demo::face_iou({}, {}) == 0 && demo::face_iou(input, input) == 1);
    std::puts("faces: all 4200 SDK anchors, NCHW/logits, padding, NMS, invalid heads/NaN/Inf, 90-degree mapping passed");
}
