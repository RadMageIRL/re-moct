#include "ar_crc.h"

namespace ar {

std::pair<uint32_t,uint32_t> frame450Crcs(const int16_t* pcm, int start, int len) {
    uint32_t crcLocal = 0, crcGlobal = 0;
    for (int k = 0; k < len; ++k) {
        const uint32_t s = (uint32_t)(uint16_t)pcm[(start + k) * 2]
                         | ((uint32_t)(uint16_t)pcm[(start + k) * 2 + 1] << 16);
        crcLocal  += s * (uint32_t)(k + 1);                  // mul 1..len
        crcGlobal += s * (FRAME450_START + (uint32_t)k);     // mul 264601..
    }
    return { crcLocal, crcGlobal };
}

} // namespace ar
