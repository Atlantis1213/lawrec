// Algorithm/layout follows SDK nncase examples/image_face_detect. No identity DB.
#include "faces.h"
#include <algorithm>
#include <cerrno>
#include <cmath>

namespace demo {
Letterbox letterbox(unsigned width, unsigned height) {
    if (!width || !height) return {};
    float scale = std::min(float(model_side) / width, float(model_side) / height);
    unsigned rw = std::max(1U, unsigned(width * scale));
    unsigned rh = std::max(1U, unsigned(height * scale));
    return {width, height, rw, rh, (model_side - rw) / 2, (model_side - rh) / 2,
        float(rw) / width, float(rh) / height};
}
Anchor anchor(unsigned level, unsigned cell, unsigned variant) {
    const unsigned steps[] = {8, 16, 32};
    const unsigned sizes[][2] = {{16, 32}, {64, 128}, {256, 512}};
    if (level >= 3 || variant >= 2) return {};
    unsigned side = model_side / steps[level];
    if (cell >= side * side) return {};
    float size = float(sizes[level][variant]) / model_side;
    return {(float(cell % side) + .5f) * steps[level] / model_side,
            (float(cell / side) + .5f) * steps[level] / model_side, size, size};
}
Point portrait_point(Point p, unsigned width, unsigned height) {
    if (!width || !height || !std::isfinite(p.x) || !std::isfinite(p.y)) return {};
    // CHN0 scales to 800x480 before clockwise 90-degree VO rotation.
    return {std::clamp(480.f - p.y * 480 / height, 0.f, 479.f),
            std::clamp(p.x * 800 / width, 0.f, 799.f)};
}
Face portrait_face(const Face &face, unsigned width, unsigned height) {
    Face result = face;
    Point upper = portrait_point({face.x1, face.y2}, width, height);
    Point lower = portrait_point({face.x2, face.y1}, width, height);
    result.x1 = upper.x; result.y1 = upper.y; result.x2 = lower.x; result.y2 = lower.y;
    for (size_t i = 0; i < 5; ++i) result.landmarks[i] = portrait_point(face.landmarks[i], width, height);
    return result;
}
float face_iou(const Face &a, const Face &b) {
    float intersection = std::max(0.f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1)) *
        std::max(0.f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    float area_a = std::max(0.f, a.x2 - a.x1) * std::max(0.f, a.y2 - a.y1);
    float area_b = std::max(0.f, b.x2 - b.x1) * std::max(0.f, b.y2 - b.y1);
    float total = area_a + area_b - intersection;
    return total > 0 ? intersection / total : 0.f;
}
int decode_faces(const std::array<FaceHead, 9> &heads, unsigned width, unsigned height,
                 std::vector<Face> &faces, float confidence, float nms) {
    faces.clear();
    if (!width || !height || !std::isfinite(confidence) || confidence <= 0 || confidence >= 1 ||
        !std::isfinite(nms) || nms <= 0 || nms >= 1) return -EINVAL;
    const unsigned sides[] = {40, 20, 10};
    for (unsigned level = 0; level < 3; ++level) {
        unsigned cells = sides[level] * sides[level];
        if (!heads[level].data || heads[level].count != cells * 8 ||
            !heads[level + 3].data || heads[level + 3].count != cells * 4 ||
            !heads[level + 6].data || heads[level + 6].count != cells * 20) return -EPROTO;
    }
    auto pad = letterbox(width, height);
    auto unpad = [&](float x, float y) {
        return Point{std::clamp((x * model_side - pad.left) / pad.scale_x, 0.f, float(width)),
                     std::clamp((y * model_side - pad.top) / pad.scale_y, 0.f, float(height))};
    };
    std::vector<Face> candidates;
    candidates.reserve(64);
    for (unsigned level = 0; level < 3; ++level) {
        unsigned cells = sides[level] * sides[level];
        for (unsigned cell = 0; cell < cells; ++cell) for (unsigned v = 0; v < 2; ++v) {
            const float *conf = heads[level + 3].data;
            float bg = conf[(v * 2) * cells + cell], fg = conf[(v * 2 + 1) * cells + cell];
            if (!std::isfinite(bg) || !std::isfinite(fg)) continue;
            float score = fg >= bg ? 1.f / (1.f + std::exp(bg - fg)) :
                std::exp(fg - bg) / (1.f + std::exp(fg - bg));
            if (score < confidence) continue;
            Anchor prior = anchor(level, cell, v);
            const float *loc = heads[level].data;
            float x = loc[(v * 4) * cells + cell], y = loc[(v * 4 + 1) * cells + cell];
            float w = loc[(v * 4 + 2) * cells + cell], h = loc[(v * 4 + 3) * cells + cell];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) continue;
            float cx = prior.x + x * .1f * prior.w, cy = prior.y + y * .1f * prior.h;
            float bw = prior.w * std::exp(w * .2f), bh = prior.h * std::exp(h * .2f);
            if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(bw) || !std::isfinite(bh)) continue;
            Point first = unpad(cx - bw / 2, cy - bh / 2), last = unpad(cx + bw / 2, cy + bh / 2);
            Face face{first.x, first.y, last.x, last.y, score, {}};
            if (face.x2 <= face.x1 || face.y2 <= face.y1) continue;
            bool valid = true;
            const float *landmarks = heads[level + 6].data;
            for (unsigned p = 0; p < 5; ++p) {
                float lx = landmarks[(v * 10 + p * 2) * cells + cell];
                float ly = landmarks[(v * 10 + p * 2 + 1) * cells + cell];
                if (!std::isfinite(lx) || !std::isfinite(ly)) { valid = false; break; }
                face.landmarks[p] = unpad(prior.x + lx * .1f * prior.w, prior.y + ly * .1f * prior.h);
            }
            if (valid) candidates.push_back(face);
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Face &a, const Face &b) { return a.score > b.score; });
    // Bound quadratic NMS to the strongest 300 candidates; cap LCD overlays at 32.
    if (candidates.size() > 300) candidates.resize(300);
    for (const Face &face : candidates) {
        bool suppressed = false;
        for (const Face &kept : faces) if (face_iou(face, kept) >= nms) { suppressed = true; break; }
        if (!suppressed) faces.push_back(face);
        if (faces.size() == max_faces) break;
    }
    return 0;
}
}
