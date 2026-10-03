#include "frame.h"
#include "config.h"
#include <cerrno>

namespace demo {
namespace {
size_t prefix(const std::vector<uint8_t> &b, size_t i) {
    if (i + 3 > b.size() || b[i] || b[i + 1]) return 0;
    if (b[i + 2] == 1) return 3;
    return i + 4 <= b.size() && !b[i + 2] && b[i + 3] == 1 ? 4 : 0;
}
}
int split_h264(const std::vector<uint8_t> &bytes, std::vector<Nal> &nals) {
    nals.clear();
    if (bytes.empty() || bytes.size() > max_access_unit) return -EMSGSIZE;
    size_t cursor = 0;
    while (cursor < bytes.size()) {
        size_t head = prefix(bytes, cursor);
        if (!head || nals.size() >= 128) return -EBADMSG;
        size_t start = cursor + head, end = start;
        while (end < bytes.size() && !prefix(bytes, end)) ++end;
        cursor = end;
        while (end > start && bytes[end - 1] == 0) --end;
        if (end == start || (bytes[start] & 0x80)) return -EBADMSG;
        unsigned type = bytes[start] & 31;
        if (!type || type >= 24) return -EBADMSG;
        nals.push_back({start, end - start, type});
    }
    return 0;
}
int H264Headers::prepare(Frame &frame) {
    frame.key = false;
    std::vector<Nal> nals;
    int ret = split_h264(frame.bytes, nals);
    if (ret) return ret;
    bool vcl = false, idr = false;
    for (auto nal : nals) {
        if (nal.type >= 1 && nal.type <= 5) vcl = true;
        if (nal.type == 5) idr = true;
        if (nal.type == 7 || nal.type == 8) {
            if (nal.size > 32768 || nal.size < (nal.type == 7 ? 4U : 2U)) return -EBADMSG;
            std::vector<uint8_t> payload(frame.bytes.begin() + nal.offset,
                                         frame.bytes.begin() + nal.offset + nal.size);
            if (nal.type == 7) {
                if (payload != sps_) pps_.clear();
                sps_ = std::move(payload);
            } else pps_ = std::move(payload);
        }
    }
    if (!vcl) return 0;
    if (idr && !sps_.empty() && !pps_.empty()) {
        if (sps_.size() + pps_.size() + 8 > max_access_unit - frame.bytes.size()) return -EOVERFLOW;
        std::vector<uint8_t> complete;
        complete.reserve(frame.bytes.size() + sps_.size() + pps_.size() + 8);
        for (const auto *header : {&sps_, &pps_}) {
            complete.insert(complete.end(), {0, 0, 0, 1});
            complete.insert(complete.end(), header->begin(), header->end());
        }
        complete.insert(complete.end(), frame.bytes.begin(), frame.bytes.end());
        frame.bytes = std::move(complete); frame.key = true;
    }
    return 1;
}
}
