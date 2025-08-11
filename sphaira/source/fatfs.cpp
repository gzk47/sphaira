#include "fatfs.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ff.h"

#include <array>
#include <algorithm>
#include <span>

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <sys/iosupport.h>

namespace sphaira::fatfs {
namespace {

// todo: replace with off+size and have the data be in another struct
// in order to be more lcache efficient.
struct BufferedFileData {
    u8* data{};
    u64 off{};
    u64 size{};

    ~BufferedFileData() {
        if (data) {
            free(data);
        }
    }

    void Allocate(u64 new_size) {
        data = (u8*)realloc(data, new_size * sizeof(*data));
        off = 0;
        size = 0;
    }
};

template<typename T>
struct LinkedList {
    T* data;
    LinkedList* next;
    LinkedList* prev;
};

constexpr u64 CACHE_LARGE_ALLOC_SIZE = 1024 * 512;
constexpr u64 CACHE_LARGE_SIZE = 1024 * 16;

template<typename T>
struct Lru {
    using ListEntry = LinkedList<T>;

    // pass span of the data.
    void Init(std::span<T> data) {
        list_flat_array.clear();
        list_flat_array.resize(data.size());

        auto list_entry = list_head = list_flat_array.data();

        for (size_t i = 0; i < data.size(); i++) {
            list_entry = list_flat_array.data() + i;
            list_entry->data = data.data() + i;

            if (i + 1 < data.size()) {
                list_entry->next = &list_flat_array[i + 1];
            }
            if (i) {
                list_entry->prev = &list_flat_array[i - 1];
            }
        }

        list_tail = list_entry->prev->next;
    }

    // moves entry to the front of the list.
    void Update(ListEntry* entry) {
        // only update position if we are not the head.
        if (list_head != entry) {
            entry->prev->next = entry->next;
            if (entry->next) {
                entry->next->prev = entry->prev;
            } else {
                list_tail = entry->prev;
            }

            // update head.
            auto head_temp = list_head;
            list_head = entry;
            list_head->prev = nullptr;
            list_head->next = head_temp;
            head_temp->prev = list_head;
        }
    }

    // moves last entry (tail) to the front of the list.
    auto GetNextFree() {
        Update(list_tail);
        return list_head->data;
    }

    auto begin() const { return list_head; }
    auto end() const { return list_tail; }

private:
    ListEntry* list_head{};
    ListEntry* list_tail{};
    std::vector<ListEntry> list_flat_array{};
};

using LruBufferedData = Lru<BufferedFileData>;

enum BisMountType {
    BisMountType_PRODINFOF,
    BisMountType_SAFE,
    BisMountType_USER,
    BisMountType_SYSTEM,
};

struct FatStorageEntry {
    FsStorage storage;
    s64 storage_size;
    LruBufferedData lru_cache[2];
    BufferedFileData buffered_small[1024]; // 1MiB (usually).
    BufferedFileData buffered_large[2];    // 1MiB
    FATFS fs;
    devoptab_t devoptab;
};

struct BisMountEntry {
    const FsBisPartitionId id;
    const char* volume_name;
    const char* mount_name;
};

constexpr BisMountEntry BIS_MOUNT_ENTRIES[] {
    [BisMountType_PRODINFOF] = { FsBisPartitionId_CalibrationFile, "PRODINFOF", "PRODINFOF:/" },
    [BisMountType_SAFE] = { FsBisPartitionId_SafeMode, "SAFE", "SAFE:/" },
    [BisMountType_USER] = { FsBisPartitionId_User, "USER", "USER:/" },
    [BisMountType_SYSTEM] = { FsBisPartitionId_System, "SYSTEM", "SYSTEM:/" },
};
static_assert(std::size(BIS_MOUNT_ENTRIES) == FF_VOLUMES);

FatStorageEntry g_fat_storage[FF_VOLUMES];

Result ReadStorage(FsStorage* storage, std::span<LruBufferedData> lru_cache, void *_buffer, u64 file_off, u64 read_size, u64 capacity) {
    // log_write("[FATFS] read offset: %zu size: %zu\n", file_off, read_size);
    auto dst = static_cast<u8*>(_buffer);
    size_t amount = 0;

    R_UNLESS(file_off < capacity, FsError_UnsupportedOperateRangeForFileStorage);
    read_size = std::min(read_size, capacity - file_off);

    // fatfs reads in max 16k chunks.
    // knowing this, it's possible to detect large file reads by simply checking if
    // the read size is 16k (or more, maybe in the furter).
    // however this would destroy random access performance, such as fetching 512 bytes.
    // the fix was to have 2 LRU caches, one for large data and the other for small (anything below 16k).
    // the results in file reads 32MB -> 184MB and directory listing is instant.
    const auto large_read = read_size >= 1024 * 16;
    auto& lru = large_read ? lru_cache[1] : lru_cache[0];

    for (auto list = lru.begin(); list; list = list->next) {
        const auto& m_buffered = list->data;
        if (m_buffered->size) {
            // check if we can read this data into the beginning of dst.
            if (file_off < m_buffered->off + m_buffered->size && file_off >= m_buffered->off) {
                const auto off = file_off - m_buffered->off;
                const auto size = std::min<s64>(read_size, m_buffered->size - off);
                if (size) {
                    // log_write("[FAT] cache HIT at: %zu\n", file_off);
                    std::memcpy(dst, m_buffered->data + off, size);

                    read_size -= size;
                    file_off += size;
                    amount += size;
                    dst += size;

                    lru.Update(list);
                    break;
                }
            }
        }
    }

    if (read_size) {
        // log_write("[FAT] cache miss at: %zu %zu\n", file_off, read_size);

        auto alloc_size = large_read ? CACHE_LARGE_ALLOC_SIZE : std::max<u64>(read_size, 512);
        alloc_size = std::min(alloc_size, capacity - file_off);

        auto m_buffered = lru.GetNextFree();
        m_buffered->Allocate(alloc_size);

        // if the dst is big enough, read data in place.
        if (read_size > alloc_size) {
            R_TRY(fsStorageRead(storage, file_off, dst, read_size));
            const auto bytes_read = read_size;
            read_size -= bytes_read;
            file_off += bytes_read;
            amount += bytes_read;
            dst += bytes_read;

            // save the last chunk of data to the m_buffered io.
            const auto max_advance = std::min<u64>(amount, alloc_size);
            m_buffered->off = file_off - max_advance;
            m_buffered->size = max_advance;
            std::memcpy(m_buffered->data, dst - max_advance, max_advance);
        } else {
            R_TRY(fsStorageRead(storage, file_off, m_buffered->data, alloc_size));
			const auto bytes_read = alloc_size;
			const auto max_advance = std::min<u64>(read_size, bytes_read);
            std::memcpy(dst, m_buffered->data, max_advance);

            m_buffered->off = file_off;
            m_buffered->size = bytes_read;

            read_size -= max_advance;
            file_off += max_advance;
            amount += max_advance;
            dst += max_advance;
        }
    }

    R_SUCCEED();
}

void fill_stat(const FILINFO* fno, struct stat *st) {
    memset(st, 0, sizeof(*st));

    st->st_nlink = 1;

    if (fno->fattrib & AM_DIR) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        st->st_size = fno->fsize;
        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    }
}

static int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int fat_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    memset(fileStruct, 0, sizeof(FIL));

    if (FR_OK != f_open((FIL*)fileStruct, path, FA_READ)) {
        return set_errno(r, ENOENT);
    }
    return r->_errno = 0;
}

int fat_close(struct _reent *r, void *fd) {
    if (FR_OK != f_close((FIL*)fd)) {
        return set_errno(r, ENOENT);
    }
    return r->_errno = 0;
}

ssize_t fat_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    UINT bytes_read;
    if (FR_OK != f_read((FIL*)fd, ptr, len, &bytes_read)) {
        return set_errno(r, ENOENT);
    }

    return bytes_read;
}

off_t fat_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    if (dir == SEEK_CUR) {
        pos += f_tell((FIL*)fd);
    } else if (dir == SEEK_END) {
        pos = f_size((FIL*)fd);
    }

    if (FR_OK != f_lseek((FIL*)fd, pos)) {
        set_errno(r, ENOENT);
        return 0;
    }

    r->_errno = 0;
    return f_tell((FIL*)fd);
}

int fat_fstat(struct _reent *r, void *fd, struct stat *st) {
    const FIL* file = (FIL*)fd;

    /* Only fill the attr and size field, leaving the timestamp blank. */
    FILINFO info = {0};
    info.fattrib = file->obj.attr;
    info.fsize = file->obj.objsize;

    /* Fill stat info. */
    fill_stat(&info, st);

    return r->_errno = 0;
}

DIR_ITER* fat_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    memset(dirState->dirStruct, 0, sizeof(FDIR));

    if (FR_OK != f_opendir((FDIR*)dirState->dirStruct, path)) {
        set_errno(r, ENOENT);
        return NULL;
    }

    r->_errno = 0;
    return dirState;
}

int fat_dirreset(struct _reent *r, DIR_ITER *dirState) {
    if (FR_OK != f_rewinddir((FDIR*)dirState->dirStruct)) {
        return set_errno(r, ENOENT);
    }
    return r->_errno = 0;
}

int fat_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    FILINFO fno{};
    if (FR_OK != f_readdir((FDIR*)dirState->dirStruct, &fno)) {
        return set_errno(r, ENOENT);
    }

    if (!fno.fname[0]) {
        return set_errno(r, ENOENT);
    }

    strcpy(filename, fno.fname);
    fill_stat(&fno, filestat);

    return r->_errno = 0;
}

int fat_dirclose(struct _reent *r, DIR_ITER *dirState) {
    if (FR_OK != f_closedir((FDIR*)dirState->dirStruct)) {
        return set_errno(r, ENOENT);
    }
    return r->_errno = 0;
}

int fat_statvfs(struct _reent *r, const char *path, struct statvfs *buf) {
    memset(buf, 0, sizeof(*buf));

    // todo: find out how to calculate free size in read only.
    const auto fat = (FatStorageEntry*)r->deviceData;
    buf->f_bsize = FF_MAX_SS;
    buf->f_frsize = FF_MAX_SS;
    buf->f_blocks = ((fat->fs.n_fatent - 2) * (DWORD)fat->fs.csize);
    buf->f_namemax = FF_LFN_BUF;

    return r->_errno = 0;
}

int fat_lstat(struct _reent *r, const char *file, struct stat *st) {
    FILINFO fno;
    if (FR_OK != f_stat(file, &fno)) {
        return set_errno(r, ENOENT);
    }

    fill_stat(&fno, st);
    return r->_errno = 0;
}

constexpr devoptab_t DEVOPTAB = {
    .structSize   = sizeof(FIL),
    .open_r       = fat_open,
    .close_r      = fat_close,
    .read_r       = fat_read,
    .seek_r       = fat_seek,
    .fstat_r      = fat_fstat,
    .stat_r       = fat_lstat,
    .dirStateSize = sizeof(FDIR),
    .diropen_r    = fat_diropen,
    .dirreset_r   = fat_dirreset,
    .dirnext_r    = fat_dirnext,
    .dirclose_r   = fat_dirclose,
    .statvfs_r    = fat_statvfs,
    .lstat_r      = fat_lstat,
};

} // namespace

Result MountAll() {
    for (u32 i = 0; i < FF_VOLUMES; i++) {
        auto& fat = g_fat_storage[i];
        const auto& bis = BIS_MOUNT_ENTRIES[i];

        log_write("[FAT] %s\n", bis.volume_name);

        fat.lru_cache[0].Init(fat.buffered_small);
        fat.lru_cache[1].Init(fat.buffered_large);

        fat.devoptab = DEVOPTAB;
        fat.devoptab.name = bis.volume_name;
        fat.devoptab.deviceData = &fat;

        R_TRY(fsOpenBisStorage(&fat.storage, bis.id));
        R_TRY(fsStorageGetSize(&fat.storage, &fat.storage_size));
        log_write("[FAT] BIS SUCCESS %s\n", bis.volume_name);

        R_UNLESS(FR_OK == f_mount(&fat.fs, bis.mount_name, 1), 0x1);
        log_write("[FAT] MOUNT SUCCESS %s\n", bis.volume_name);

        R_UNLESS(AddDevice(&fat.devoptab) >= 0, 0x1);
        log_write("[FAT] DEVICE SUCCESS %s\n", bis.volume_name);
    }

    R_SUCCEED();
}

void UnmountAll() {
    for (u32 i = 0; i < FF_VOLUMES; i++) {
        auto& fat = g_fat_storage[i];
        const auto& bis = BIS_MOUNT_ENTRIES[i];

        RemoveDevice(bis.mount_name);
        f_unmount(bis.mount_name);
        fsStorageClose(&fat.storage);
    }
}

} // namespace sphaira::fatfs

extern "C" {

const char* VolumeStr[] {
    sphaira::fatfs::BIS_MOUNT_ENTRIES[0].volume_name,
    sphaira::fatfs::BIS_MOUNT_ENTRIES[1].volume_name,
    sphaira::fatfs::BIS_MOUNT_ENTRIES[2].volume_name,
    sphaira::fatfs::BIS_MOUNT_ENTRIES[3].volume_name,
};

Result fatfs_read(u8 num, void* dst, u64 offset, u64 size) {
    // log_write("[FAT] num: %u\n", num);
    auto& fat = sphaira::fatfs::g_fat_storage[num];
    return sphaira::fatfs::ReadStorage(&fat.storage, fat.lru_cache, dst, offset, size, fat.storage_size);
}

} // extern "C"
