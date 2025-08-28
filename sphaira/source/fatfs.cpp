#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "fatfs.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ff.h"

#include <array>
#include <algorithm>
#include <span>
#include <memory>

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <sys/iosupport.h>

namespace sphaira::fatfs {
namespace {

auto is_archive(BYTE attr) -> bool {
    const auto archive_attr = AM_DIR | AM_ARC;
    return (attr & archive_attr) == archive_attr;
}

// todo: replace with off+size and have the data be in another struct
// in order to be more lcache efficient.
struct FsStorageSource final : yati::source::Base {
    FsStorageSource(FsStorage* s) : m_s{*s} {

    }

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        R_TRY(fsStorageRead(&m_s, off, buf, size));
        *bytes_read = size;
        R_SUCCEED();
    }

    Result GetSize(s64* size) {
        return fsStorageGetSize(&m_s, size);
    }

private:
    FsStorage m_s;
};

struct File {
    FIL* files;
    u32 file_count;
    size_t off;
    char path[256];
};

struct Dir {
    FDIR dir;
    char path[256];
};

u64 get_size_from_files(const File* file) {
    u64 size = 0;
    for (u32 i = 0; i < file->file_count; i++) {
        size += f_size(&file->files[i]);
    }
    return size;
}

FIL* get_current_file(File* file) {
    auto off = file->off;
    for (u32 i = 0; i < file->file_count; i++) {
        auto fil = &file->files[i];
        if (off <= f_size(fil)) {
            return fil;
        }
        off -= f_size(fil);
    }
    return NULL;
}

// adjusts current file pos and sets the rest of files to 0.
void set_current_file_pos(File* file) {
    s64 off = file->off;
    for (u32 i = 0; i < file->file_count; i++) {
        auto fil = &file->files[i];
        if (off >= 0 && off < f_size(fil)) {
            f_lseek(fil, off);
        } else {
            f_rewind(fil);
        }
        off -= f_size(fil);
    }
}

enum BisMountType {
    BisMountType_PRODINFOF,
    BisMountType_SAFE,
    BisMountType_USER,
    BisMountType_SYSTEM,
};

struct FatStorageEntry {
    FsStorage storage;
    std::unique_ptr<devoptab::common::LruBufferedData> buffered;
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

void fill_stat(const char* path, const FILINFO* fno, struct stat *st) {
    memset(st, 0, sizeof(*st));

    st->st_nlink = 1;

    struct tm tm{};
    tm.tm_sec = (fno->ftime & 0x1F) << 1;
    tm.tm_min = (fno->ftime >> 5) & 0x3F;
    tm.tm_hour = (fno->ftime >> 11);
    tm.tm_mday = (fno->fdate & 0x1F);
    tm.tm_mon = ((fno->fdate >> 5) & 0xF) - 1;
    tm.tm_year = (fno->fdate >> 9) + 80;

    st->st_atime = mktime(&tm);
    st->st_mtime = st->st_atime;
    st->st_ctime = st->st_atime;

    // fake file.
    if (path && is_archive(fno->fattrib)) {
        st->st_size = 0;
        char file_path[256];
        for (u16 i = 0; i < 256; i++) {
            std::snprintf(file_path, sizeof(file_path), "%s/%02u", path, i);
            FILINFO file_info;
            if (FR_OK != f_stat(file_path, &file_info)) {
                break;
            }

            st->st_size += file_info.fsize;
        }

        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    } else
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
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));

    // todo: init array
    // todo: handle dir.
    FIL fil{};
    if (FR_OK == f_open(&fil, path, FA_READ)) {
        file->file_count = 1;
        file->files = (FIL*)std::malloc(sizeof(*file->files));
        std::memcpy(file->files, &fil, sizeof(*file->files));
        // todo: check what error code is returned here.
    } else {
        FILINFO info{};
        if (FR_OK != f_stat(path, &info)) {
            return set_errno(r, ENOENT);
        }

        if (!(info.fattrib & AM_ARC)) {
            return set_errno(r, ENOENT);
        }

        char file_path[256];
        for (u16 i = 0; i < 256; i++) {
            std::memset(&fil, 0, sizeof(fil));
            std::snprintf(file_path, sizeof(file_path), "%s/%02u", path, i);

            if (FR_OK != f_open(&fil, file_path, FA_READ)) {
                break;
            }

            file->files = (FIL*)std::realloc(file->files, (i + 1) * sizeof(*file->files));
            std::memcpy(&file->files[i], &fil, sizeof(fil));
            file->file_count++;
        }
    }

    if (!file->files) {
        return set_errno(r, ENOENT);
    }

    std::snprintf(file->path, sizeof(file->path), "%s", path);
    return r->_errno = 0;
}

int fat_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);

    if (file->files) {
        for (u32 i = 0; i < file->file_count; i++) {
            f_close(&file->files[i]);
        }
        free(file->files);
    }

    return r->_errno = 0;
}

ssize_t fat_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    UINT total_bytes_read = 0;

    while (len) {
        UINT bytes_read;
        auto fil = get_current_file(file);
        if (!fil) {
            log_write("[FATFS] failed to get fil\n");
            return set_errno(r, ENOENT);
        }

        if (FR_OK != f_read(fil, ptr, len, &bytes_read)) {
            return set_errno(r, ENOENT);
        }

        if (!bytes_read) {
            break;
        }

        len -= bytes_read;
        file->off += bytes_read;
        total_bytes_read += bytes_read;
    }

    return total_bytes_read;
}

off_t fat_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    const auto size = get_size_from_files(file);

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = size;
    }

    file->off = std::clamp<u64>(pos, 0, size);
    set_current_file_pos(file);

    r->_errno = 0;
    return file->off;
}

int fat_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);

    /* Only fill the attr and size field, leaving the timestamp blank. */
    FILINFO info{};
    info.fsize = get_size_from_files(file);

    /* Fill stat info. */
    fill_stat(nullptr, &info, st);

    return r->_errno = 0;
}

DIR_ITER* fat_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));

    if (FR_OK != f_opendir(&dir->dir, path)) {
        set_errno(r, ENOENT);
        return NULL;
    }

    r->_errno = 0;
    return dirState;
}

int fat_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);

    if (FR_OK != f_rewinddir(&dir->dir)) {
        return set_errno(r, ENOENT);
    }

    return r->_errno = 0;
}

int fat_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    FILINFO fno{};

    if (FR_OK != f_readdir(&dir->dir, &fno)) {
        return set_errno(r, ENOENT);
    }

    if (!fno.fname[0]) {
        return set_errno(r, ENOENT);
    }

    strcpy(filename, fno.fname);
    fill_stat(dir->path, &fno, filestat);

    return r->_errno = 0;
}

int fat_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);

    if (FR_OK != f_closedir(&dir->dir)) {
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

    fill_stat(file, &fno, st);
    return r->_errno = 0;
}

constexpr devoptab_t DEVOPTAB = {
    .structSize   = sizeof(File),
    .open_r       = fat_open,
    .close_r      = fat_close,
    .read_r       = fat_read,
    .seek_r       = fat_seek,
    .fstat_r      = fat_fstat,
    .stat_r       = fat_lstat,
    .dirStateSize = sizeof(Dir),
    .diropen_r    = fat_diropen,
    .dirreset_r   = fat_dirreset,
    .dirnext_r    = fat_dirnext,
    .dirclose_r   = fat_dirclose,
    .statvfs_r    = fat_statvfs,
    .lstat_r      = fat_lstat,
};

Mutex g_mutex{};
bool g_is_init{};

} // namespace

Result MountAll() {
    SCOPED_MUTEX(&g_mutex);

    if (g_is_init) {
        R_SUCCEED();
    }

    for (u32 i = 0; i < FF_VOLUMES; i++) {
        auto& fat = g_fat_storage[i];
        const auto& bis = BIS_MOUNT_ENTRIES[i];

        // log_write("[FAT] %s\n", bis.volume_name);

        fat.devoptab = DEVOPTAB;
        fat.devoptab.name = bis.volume_name;
        fat.devoptab.deviceData = &fat;

        R_TRY(fsOpenBisStorage(&fat.storage, bis.id));
        auto source = std::make_shared<FsStorageSource>(&fat.storage);

        s64 size;
        R_TRY(source->GetSize(&size));
        // log_write("[FAT] BIS SUCCESS %s\n", bis.volume_name);

        fat.buffered = std::make_unique<devoptab::common::LruBufferedData>(source, size);

        R_UNLESS(FR_OK == f_mount(&fat.fs, bis.mount_name, 1), 0x1);
        // log_write("[FAT] MOUNT SUCCESS %s\n", bis.volume_name);

        R_UNLESS(AddDevice(&fat.devoptab) >= 0, 0x1);
        // log_write("[FAT] DEVICE SUCCESS %s\n", bis.volume_name);
    }

    g_is_init = true;
    R_SUCCEED();
}

void UnmountAll() {
    SCOPED_MUTEX(&g_mutex);

    if (!g_is_init) {
        return;
    }

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
    return fat.buffered->Read2(dst, offset, size);
}

} // extern "C"
