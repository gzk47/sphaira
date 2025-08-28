#pragma once

#include "ui/types.hpp"

namespace sphaira::utils {

struct HashStr {
    char str[0x21];
};

HashStr hexIdToStr(FsRightsId id);
HashStr hexIdToStr(NcmRightsId id);
HashStr hexIdToStr(NcmContentId id);

template<typename T>
constexpr inline T AlignUp(T value, T align) {
    return (value + (align - 1)) &~ (align - 1);
}

template<typename T>
constexpr inline T AlignDown(T value, T align) {
    return value &~ (align - 1);
}

} // namespace sphaira::utils
