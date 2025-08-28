#include "hasher.hpp"
#include "app.hpp"
#include "threaded_file_transfer.hpp"
#include <mbedtls/md5.h>
#include <utility>

#include <zlib.h>
#include <zstd.h>

namespace sphaira::hash {
namespace {

consteval auto CalculateHashStrLen(s64 buf_size) {
    return buf_size * 2 + 1;
}

struct FileSource final : BaseSource {
    FileSource(fs::Fs* fs, const fs::FsPath& path) : m_fs{fs} {
        m_open_result = m_fs->OpenFile(path, FsOpenMode_Read, std::addressof(m_file));
        m_is_file_based_emummc = App::IsFileBaseEmummc();
    }

    Result Size(s64* out) override {
        R_TRY(m_open_result);
        return m_file.GetSize(out);
    }

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        R_TRY(m_open_result);
        const auto rc = m_file.Read(off, buf, size, 0, bytes_read);
        if (m_fs->IsNative() && m_is_file_based_emummc) {
            svcSleepThread(2e+6); // 2ms
        }
        return rc;
    }

private:
    fs::Fs* m_fs{};
    fs::File m_file{};
    Result m_open_result{};
    bool m_is_file_based_emummc{};
};

struct MemSource final : BaseSource {
    MemSource(std::span<const u8> data) : m_data{data} { }

    Result Size(s64* out) override {
        *out = m_data.size();
        R_SUCCEED();
    }

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        size = std::min<s64>(size, m_data.size() - off);
        std::memcpy(buf, m_data.data() + off, size);
        *bytes_read = size;
        R_SUCCEED();
    }

private:
    const std::span<const u8> m_data;
};

struct HashSource {
    virtual ~HashSource() = default;
    virtual void Update(const void* buf, s64 size, s64 file_size) = 0;
    virtual void Get(std::string& out) = 0;
};

struct HashNull final : HashSource {
    void Update(const void* buf, s64 size, s64 file_size) override {
        m_in_size += size;
    }

    void Get(std::string& out) override {
        char str[64];
        std::snprintf(str, sizeof(str), "%zu bytes", m_in_size);
        out = str;
    }

private:
    size_t m_in_size{};
};

// this currently crashes when freeing the pool :/
#define USE_THREAD_POOL 0
struct HashZstd final : HashSource {
    HashZstd() {
        const auto num_threads = 3;
        const auto level = ZSTD_CLEVEL_DEFAULT;

        m_ctx = ZSTD_createCCtx();
        if (!m_ctx) {
            log_write("[ZSTD] failed to create ctx\n");
        }


        #if USE_THREAD_POOL
        m_pool = ZSTD_createThreadPool(num_threads);
        if (!m_pool) {
            log_write("[ZSTD] failed to create pool\n");
        }

        if (ZSTD_isError(ZSTD_CCtx_refThreadPool(m_ctx, m_pool))) {
            log_write("[ZSTD] failed ZSTD_CCtx_refThreadPool(m_pool)\n");
        }
        #endif
        if (ZSTD_isError(ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_compressionLevel, level))) {
            log_write("[ZSTD] failed ZSTD_CCtx_setParameter(ZSTD_c_compressionLevel)\n");
        }
        if (ZSTD_isError(ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_nbWorkers, num_threads))) {
            log_write("[ZSTD] failed ZSTD_CCtx_setParameter(ZSTD_c_nbWorkers)\n");
        }

        m_out_buf.resize(ZSTD_CStreamOutSize());
    }

    ~HashZstd() {
        ZSTD_freeCCtx(m_ctx);
        #if USE_THREAD_POOL
        // crashes here during ZSTD_pthread_join()
        // ZSTD_freeThreadPool(m_pool);
        #endif
    }

    void Update(const void* buf, s64 size, s64 file_size) override {
        ZSTD_inBuffer input = { buf, (u64)size, 0 };

        const auto last_chunk = m_in_size + size >= file_size;
        const auto mode = last_chunk ? ZSTD_e_end : ZSTD_e_continue;

        while (input.pos < input.size) {
            ZSTD_outBuffer output = { m_out_buf.data(), m_out_buf.size(), 0 };
            const size_t remaining = ZSTD_compressStream2(m_ctx, &output , &input, mode);

            if (ZSTD_isError(remaining)) {
                log_write("[ZSTD] error: %zu\n", remaining);
                break;
            }

            m_out_size += output.pos;
        };

        m_in_size += size;
    }

    void Get(std::string& out) override {
        log_write("getting size: %zu vs %zu\n", m_out_size, m_in_size);
        char str[64];
        const u32 percentage = ((double)m_out_size / (double)m_in_size) * 100.0;
        std::snprintf(str, sizeof(str), "%u%%", percentage);
        out = str;
        log_write("got size: %zu vs %zu\n", m_out_size, m_in_size);
    }

private:
    ZSTD_CCtx* m_ctx{};
    ZSTD_threadPool* m_pool{};
    std::vector<u8> m_out_buf{};
    size_t m_in_size{};
    size_t m_out_size{};
};

struct HashDeflate final : HashSource {
    HashDeflate() {
        deflateInit(&m_ctx, Z_DEFAULT_COMPRESSION);
        m_out_buf.resize(deflateBound(&m_ctx, 1024*1024*16)); // max chunk size.
    }

    ~HashDeflate() {
        deflateEnd(&m_ctx);
    }

    void Update(const void* buf, s64 size, s64 file_size) override {
        m_ctx.avail_in = size;
        m_ctx.next_in = const_cast<Bytef*>((const Bytef*)buf);

        const auto last_chunk = m_in_size + size >= file_size;
        const auto mode = last_chunk ? Z_FINISH : Z_NO_FLUSH;

        while (m_ctx.avail_in != 0) {
            m_ctx.next_out = m_out_buf.data();
            m_ctx.avail_out = m_out_buf.size();

            const auto rc = deflate(&m_ctx, mode);
            if (Z_OK != rc) {
                if (Z_STREAM_END != rc) {
                    log_write("[ZLIB] deflate error: %d\n", rc);
                }
                break;
            }
        }

        m_in_size += size;
    }

    void Get(std::string& out) override {
        char str[64];
        const u32 percentage = ((double)m_ctx.total_out / (double)m_in_size) * 100.0;
        std::snprintf(str, sizeof(str), "%u%%", percentage);
        out = str;
    }

private:
    z_stream m_ctx{};
    std::vector<u8> m_out_buf{};
    size_t m_in_size{};
};

struct HashCrc32 final : HashSource {
    void Update(const void* buf, s64 size, s64 file_size) override {
        m_seed = crc32CalculateWithSeed(m_seed, buf, size);
    }

    void Get(std::string& out) override {
        char str[CalculateHashStrLen(sizeof(m_seed))];
        std::snprintf(str, sizeof(str), "%08x", m_seed);
        out = str;
    }

private:
    u32 m_seed{};
};

struct HashMd5 final : HashSource {
    HashMd5() {
        mbedtls_md5_init(&m_ctx);
        mbedtls_md5_starts_ret(&m_ctx);
    }

    ~HashMd5() {
        mbedtls_md5_free(&m_ctx);
    }

    void Update(const void* buf, s64 size, s64 file_size) override {
        mbedtls_md5_update_ret(&m_ctx, (const u8*)buf, size);
    }

    void Get(std::string& out) override {
        u8 hash[16];
        mbedtls_md5_finish_ret(&m_ctx, hash);

        char str[CalculateHashStrLen(sizeof(hash))];
        for (u32 i = 0; i < sizeof(hash); i++) {
            std::sprintf(str + i * 2, "%02x", hash[i]);
        }

        out = str;
    }

private:
    mbedtls_md5_context m_ctx{};
};

struct HashSha1 final : HashSource {
    HashSha1() {
        sha1ContextCreate(&m_ctx);
    }

    void Update(const void* buf, s64 size, s64 file_size) override {
        sha1ContextUpdate(&m_ctx, buf, size);
    }

    void Get(std::string& out) override {
        u8 hash[SHA1_HASH_SIZE];
        sha1ContextGetHash(&m_ctx, hash);

        char str[CalculateHashStrLen(sizeof(hash))];
        for (u32 i = 0; i < sizeof(hash); i++) {
            std::sprintf(str + i * 2, "%02x", hash[i]);
        }

        out = str;
    }

private:
    Sha1Context m_ctx{};
};

struct HashSha256 final : HashSource {
    HashSha256() {
        sha256ContextCreate(&m_ctx);
    }

    void Update(const void* buf, s64 size, s64 file_size) override {
        sha256ContextUpdate(&m_ctx, buf, size);
    }

    void Get(std::string& out) override {
        u8 hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&m_ctx, hash);

        char str[CalculateHashStrLen(sizeof(hash))];
        for (u32 i = 0; i < sizeof(hash); i++) {
            std::sprintf(str + i * 2, "%02x", hash[i]);
        }

        out = str;
    }

private:
    Sha256Context m_ctx{};
};

Result Hash(ui::ProgressBox* pbox, std::unique_ptr<HashSource> hash, BaseSource* source, std::string& out) {
    s64 file_size;
    R_TRY(source->Size(&file_size));

    R_TRY(thread::Transfer(pbox, file_size,
        [&](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
            return source->Read(data, off, size, bytes_read);
        },
        [&](const void* data, s64 off, s64 size) -> Result {
            hash->Update(data, size, file_size);
            R_SUCCEED();
        }
    ));

    hash->Get(out);
    R_SUCCEED();
}

} // namespace

auto GetTypeStr(Type type) -> const char* {
    switch (type) {
        case Type::Crc32: return "CRC32";
        case Type::Md5: return "MD5";
        case Type::Sha1: return "SHA1";
        case Type::Sha256: return "SHA256";
        case Type::Null: return "/dev/null (Speed Test)";
        case Type::Deflate: return "Deflate (Speed Test)";
        case Type::Zstd: return "ZSTD (Speed Test)";
    }
    return "";
}

Result Hash(ui::ProgressBox* pbox, Type type, BaseSource* source, std::string& out) {
    switch (type) {
        case Type::Crc32: return Hash(pbox, std::make_unique<HashCrc32>(), source, out);
        case Type::Md5: return Hash(pbox, std::make_unique<HashMd5>(), source, out);
        case Type::Sha1: return Hash(pbox, std::make_unique<HashSha1>(), source, out);
        case Type::Sha256: return Hash(pbox, std::make_unique<HashSha256>(), source, out);
        case Type::Null: return Hash(pbox, std::make_unique<HashNull>(), source, out);
        case Type::Deflate: return Hash(pbox, std::make_unique<HashDeflate>(), source, out);
        case Type::Zstd: return Hash(pbox, std::make_unique<HashZstd>(), source, out);
    }
    std::unreachable();
}

Result Hash(ui::ProgressBox* pbox, Type type, fs::Fs* fs, const fs::FsPath& path, std::string& out) {
    auto source = std::make_unique<FileSource>(fs, path);
    return Hash(pbox, type, source.get(), out);
}

Result Hash(ui::ProgressBox* pbox, Type type, std::span<const u8> data, std::string& out) {
    auto source = std::make_unique<MemSource>(data);
    return Hash(pbox, type, source.get(), out);
}

} // namespace sphaira::has
