#include "utils/devoptab_common.hpp"
#include "defines.hpp"
#include "log.hpp"

#include <cstring>
#include <algorithm>

namespace sphaira::devoptab::common {

Result BufferedData::Read(void* buf, s64 off, s64 size) {
    u64 bytes_read;
    return Read(buf, off, size, &bytes_read);
}

// todo: change above function to handle bytes read instead.
Result BufferedData::Read(void *_buffer, s64 file_off, s64 read_size, u64* bytes_read) {
    auto dst = static_cast<u8*>(_buffer);
    size_t amount = 0;
    *bytes_read = 0;

    R_UNLESS(file_off < capacity, FsError_UnsupportedOperateRangeForFileStorage);
    read_size = std::min<s64>(read_size, capacity - file_off);

    if (m_size) {
        // check if we can read this data into the beginning of dst.
        if (file_off < m_off + m_size && file_off >= m_off) {
            const auto off = file_off - m_off;
            const auto size = std::min<s64>(read_size, m_size - off);
            if (size) {
                std::memcpy(dst, m_data + off, size);

                read_size -= size;
                file_off += size;
                amount += size;
                dst += size;
            }
        }
    }

    if (read_size) {
        const auto alloc_size = sizeof(m_data);
        m_off = 0;
        m_size = 0;
        u64 bytes_read;

        // if the dst is big enough, read data in place.
        if (read_size > alloc_size) {
            R_TRY(source->Read(dst, file_off, read_size, &bytes_read));

            read_size -= bytes_read;
            file_off += bytes_read;
            amount += bytes_read;
            dst += bytes_read;

            // save the last chunk of data to the m_buffered io.
            const auto max_advance = std::min<u64>(amount, alloc_size);
            m_off = file_off - max_advance;
            m_size = max_advance;
            std::memcpy(m_data, dst - max_advance, max_advance);
        } else {
            R_TRY(source->Read(m_data, file_off, alloc_size, &bytes_read));
			const auto bytes_read = alloc_size;
			const auto max_advance = std::min<u64>(read_size, bytes_read);
            std::memcpy(dst, m_data, max_advance);

            m_off = file_off;
            m_size = bytes_read;

            read_size -= max_advance;
            file_off += max_advance;
            amount += max_advance;
            dst += max_advance;
        }
    }

    *bytes_read = amount;
    R_SUCCEED();
}

bool fix_path(const char* str, char* out) {
    // log_write("[SAVE] got path: %s\n", str);

    str = std::strrchr(str, ':');
    if (!str) {
        return false;
    }

    // skip over ':'
    str++;
    size_t len = 0;

    // todo: hanle utf8 paths.
    for (size_t i = 0; str[i]; i++) {
        // skip multiple slashes.
        if (i && str[i] == '/' && str[i - 1] == '/') {
            continue;
        }

        // add leading slash.
        if (!i && str[i] != '/') {
            out[len++] = '/';
        }

        // save single char.
        out[len++] = str[i];
    }

    // skip trailing slash.
    if (len > 1 && out[len - 1] == '/') {
        out[len - 1] = '\0';
    }

    // null the end.
    out[len] = '\0';

    // log_write("[SAVE] end path: %s\n", out);

    return true;
}

} // sphaira::devoptab::common
