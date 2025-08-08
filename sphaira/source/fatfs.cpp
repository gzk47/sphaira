#include "fatfs.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ff.h"

#include <array>
#include <algorithm>

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <sys/iosupport.h>

namespace sphaira::fatfs {
namespace {

// 256-512 are the best values, anything more has serious slow down
// due to non-seq reads.
struct BufferedFileData {
	u8 data[1024 * 256];
	s64 off;
	s64 size;
};

enum BisMountType {
    BisMountType_PRODINFOF,
    BisMountType_SAFE,
    BisMountType_USER,
    BisMountType_SYSTEM,
};

struct FatStorageEntry {
    FsStorage storage;
    BufferedFileData buffered;
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

// crappy generic buffered io i wrote a while ago.
// this allows for 3-4x speed increase reading from storage.
// as it avoids reading very small chunks at a time.
// note: this works best when the file is not fragmented.
Result ReadFile(FsStorage* storage, BufferedFileData& m_buffered, void *_buffer, size_t file_off, size_t read_size) {
    auto dst = static_cast<u8*>(_buffer);
    size_t amount = 0;

    // check if we already have this data buffered.
    if (m_buffered.size) {
        // check if we can read this data into the beginning of dst.
        if (file_off < m_buffered.off + m_buffered.size && file_off >= m_buffered.off) {
            const auto off = file_off - m_buffered.off;
            const auto size = std::min<s64>(read_size, m_buffered.size - off);
            std::memcpy(dst, m_buffered.data + off, size);

            read_size -= size;
            file_off += size;
            amount += size;
            dst += size;
        }
    }

    if (read_size) {
        m_buffered.off = 0;
        m_buffered.size = 0;

        // if the dst dst is big enough, read data in place.
        if (read_size >= sizeof(m_buffered.data)) {
            if (R_SUCCEEDED(fsStorageRead(storage, file_off, dst, read_size))) {
                const auto bytes_read = read_size;
				read_size -= bytes_read;
                file_off += bytes_read;
                amount += bytes_read;
                dst += bytes_read;

                // save the last chunk of data to the m_buffered io.
                const auto max_advance = std::min(amount, sizeof(m_buffered.data));
                m_buffered.off = file_off - max_advance;
                m_buffered.size = max_advance;
                std::memcpy(m_buffered.data, dst - max_advance, max_advance);
            }
        } else if (R_SUCCEEDED(fsStorageRead(storage, file_off, m_buffered.data, sizeof(m_buffered.data)))) {
			const auto bytes_read = sizeof(m_buffered.data);
			const auto max_advance = std::min(read_size, bytes_read);
            std::memcpy(dst, m_buffered.data, max_advance);

            m_buffered.off = file_off;
            m_buffered.size = bytes_read;

            read_size -= max_advance;
            file_off += max_advance;
            amount += max_advance;
            dst += max_advance;
        }
    }

	return 0;
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
        log_write("[FAT] fat_dirreset failed\n");
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

        fat.devoptab = DEVOPTAB;
        fat.devoptab.name = bis.volume_name;
        fat.devoptab.deviceData = &fat;

        R_TRY(fsOpenBisStorage(&fat.storage, bis.id));
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
    return sphaira::fatfs::ReadFile(&fat.storage, fat.buffered, dst, offset, size);
}

} // extern "C"
