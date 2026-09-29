#include "lawrec_annexb.h"
#include <cassert>
#include <cstdio>
int main() {
    uint8_t mixed[] = {0,0,0,1,0x67,0x42,0,0,1,0x68,0xaa,0,0,0,1,0x65,0xbb};
    size_t left = sizeof(mixed), size;
    auto *p = lawrec_annexb_next(mixed, left, size);
    assert(p && size == 2 && *p == 0x67);
    p = lawrec_annexb_next(p + size, left, size);
    assert(p && size == 2 && *p == 0x68);
    p = lawrec_annexb_next(p + size, left, size);
    assert(p && size == 2 && *p == 0x65 && left == 0);
    uint8_t empty[] = {0,0,0,1}; left = sizeof(empty);
    assert(!lawrec_annexb_next(empty, left, size));
    for (size_t n=0; n<4; ++n) { left=n; assert(!lawrec_annexb_next(empty,left,size)); }
    puts("Annex-B: mixed prefixes and truncated input passed");
}
