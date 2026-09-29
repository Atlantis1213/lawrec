#pragma once
#include <stddef.h>
#include <stdint.h>

/* Return one Annex-B NAL without its 3/4-byte prefix. remaining includes
   the next prefix. Reject empty or unframed data without reading past input. */
inline uint8_t *lawrec_annexb_next(uint8_t *data, size_t &remaining, size_t &nal_size)
{
    nal_size = 0;
    if (!data || remaining < 4 || data[0] || data[1]) return nullptr;
    size_t prefix = data[2] == 1 ? 3 :
                    (remaining >= 5 && data[2] == 0 && data[3] == 1 ? 4 : 0);
    if (!prefix || prefix >= remaining) return nullptr;
    size_t end = prefix;
    while (end < remaining) {
        if (end + 2 < remaining && !data[end] && !data[end+1] &&
            (data[end+2] == 1 || (end+3 < remaining && !data[end+2] && data[end+3] == 1))) break;
        ++end;
    }
    nal_size = end - prefix;
    remaining -= end;
    return nal_size ? data + prefix : nullptr;
}
