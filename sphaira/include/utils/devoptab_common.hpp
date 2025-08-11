#pragma once

#include "yati/source/file.hpp"
#include <memory>

namespace sphaira::devoptab::common {

// buffers data in 512k chunks to maximise throughput.
// not suitable if random access >= 512k is common.
// if that is needed, see the LRU cache varient used for fatfs.
struct BufferedData final : yati::source::Base {
    static constexpr inline u64 CHUNK_SIZE = 1024 * 512;

    BufferedData(std::unique_ptr<yati::source::Base>&& _source, u64 _size)
    : source{std::forward<decltype(_source)>(_source)}
    , capacity{_size} {

    }

    Result Read(void *buf, s64 off, s64 size);
    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;

private:
    std::unique_ptr<yati::source::Base> source;
    const u64 capacity;

    u64 m_off{};
    u64 m_size{};
    u8 m_data[CHUNK_SIZE]{};
};

bool fix_path(const char* str, char* out);

} // namespace sphaira::devoptab::common
