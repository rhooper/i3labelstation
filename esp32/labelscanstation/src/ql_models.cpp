#include "ql_models.h"
#include <cstddef>

static const ql_model_t s_models[] = {
    // Standard-width models (90 bytes/row)
    //                                       bpr  inv   comp   mode   exp    cut    2col
    { 0x2015, "QL-500",     90, 200, false, false, false, false, false },
    { 0x2016, "QL-550",     90, 200, false, false,  true,  true, false },
    { 0x2027, "QL-560",     90, 200, false, false,  true,  true, false },
    { 0x2028, "QL-570",     90, 200, false, false,  true,  true, false },
    { 0x2029, "QL-580N",    90, 200,  true,  true,  true,  true, false },
    { 0x201B, "QL-650TD",   90, 200,  true,  true,  true,  true, false },
    { 0x2042, "QL-700",     90, 200, false, false,  true,  true, false },
    { 0x2043, "QL-710W",    90, 200,  true,  true,  true,  true, false },
    { 0x2044, "QL-720NW",   90, 200,  true,  true,  true,  true, false },
    { 0x209B, "QL-800",     90, 400, false,  true,  true,  true,  true },
    { 0x209C, "QL-810W",    90, 400,  true,  true,  true,  true,  true },
    { 0x209D, "QL-820NWB",  90, 400,  true,  true,  true,  true,  true },
};

const ql_model_t *ql_model_lookup(uint16_t pid) {
    for (size_t i = 0; i < sizeof(s_models) / sizeof(s_models[0]); i++) {
        if (s_models[i].usb_pid == pid)
            return &s_models[i];
    }
    return nullptr;
}
