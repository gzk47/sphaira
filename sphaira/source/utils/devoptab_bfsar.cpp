
#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "defines.hpp"
#include "log.hpp"

#include "yati/container/nsp.hpp"
#include "yati/container/xci.hpp"
#include "yati/source/file.hpp"

#include <pulsar.h>

#include <cstring>
#include <cerrno>
#include <array>
#include <memory>
#include <algorithm>
#include <sys/iosupport.h>

namespace sphaira::devoptab {
namespace {

struct Device {
    PLSR_BFSAR bfsar;
    std::FILE* file; // points to archive file.
};

struct File {
    Device* device;
    PLSR_BFWARFileInfo info;
    size_t off;
};

struct Dir {
    Device* device;
    u32 index;
};

PLSR_RC GetFileInfo(const PLSR_BFSAR *bfsar, std::string_view path, PLSR_BFWARFileInfo& out) {
    if (path.starts_with('/')) {
        path = path.substr(1);
    }
    path = path.substr(0, path.find_last_of('.'));
    char buf123[255];
    std::snprintf(buf123, sizeof(buf123), "%.*s", (int)path.length(), path.data());

    PLSR_BFSARStringSearchInfo searchInfo;
    R_TRY(plsrBFSARStringSearch(bfsar, buf123, &searchInfo));

    PLSR_BFSARSoundInfo soundInfo;
	R_TRY(plsrBFSARSoundGet(bfsar, searchInfo.itemId.index, &soundInfo));

    PLSR_BFSARFileInfo soundFileInfo;
    R_TRY(plsrBFSARFileScan(bfsar, soundInfo.fileIndex, &soundFileInfo));
    R_TRY(plsrBFSARFileInfoNormalize(bfsar, &soundFileInfo));

    PLSR_BFWSD bfwsd;
    R_TRY(plsrBFWSDOpenInside(&bfsar->ar, soundFileInfo.internal.offset, &bfwsd));
    ON_SCOPE_EXIT(plsrBFWSDClose(&bfwsd));

    PLSR_BFWSDSoundDataInfo soundDataInfo;
    R_TRY(plsrBFWSDSoundDataGet(&bfwsd, soundInfo.wave.index, &soundDataInfo));

    PLSR_BFWSDNoteInfo noteInfo;
	R_TRY(plsrBFWSDSoundDataNoteGet(&bfwsd, &soundDataInfo.noteInfoTable, 0, &noteInfo));

	PLSR_BFWSDWaveId waveId;
	R_TRY(plsrBFWSDWaveIdListGetEntry(&bfwsd, noteInfo.waveIdIndex, &waveId));

    PLSR_BFSARWaveArchiveInfo waveArchiveInfo;
	R_TRY(plsrBFSARWaveArchiveGet(bfsar, waveId.archiveItemId.index, &waveArchiveInfo));

    PLSR_BFWAR bfwar;
	R_TRY(plsrBFSARWaveArchiveOpen(bfsar, &waveArchiveInfo, &bfwar));
    ON_SCOPE_EXIT(plsrBFWARClose(&bfwar));

	R_TRY(plsrBFWARFileGet(&bfwar, waveId.index, &out));

    // adjust offset.
    out.offset += bfwar.ar.offset;
    R_SUCCEED();

}

int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int devoptab_open(struct _reent *r, void *fileStruct, const char *_path, int flags, int mode) {
    auto device = (Device*)r->deviceData;
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    PLSR_BFWARFileInfo info;
    if (R_FAILED(GetFileInfo(&device->bfsar, path, info))) {
        return set_errno(r, ENOENT);
    }

    file->device = device;
    file->info = info;
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    std::memset(file, 0, sizeof(*file));

    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    const auto& info = file->info;

    // const auto real_len = len;
    // plsr seems to read oob, so allow for some tollerance.
    const auto oob_allowed = 64;
    len = std::min(len, info.size + oob_allowed - file->off);
    std::fseek(file->device->file, file->info.offset + file->off, SEEK_SET);
    const auto bytes_read = std::fread(ptr, 1, len, file->device->file);

    // log_write("bytes read: %zu len: %zu real_len: %zu off: %zu size: %u\n", bytes_read, len, real_len, file->off, info.size);

    file->off += bytes_read;
    return bytes_read;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    const auto& info = file->info;

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = info.size;
    }

    r->_errno = 0;
    return file->off = std::clamp<u64>(pos, 0, info.size);
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    const auto& info = file->info;

    std::memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_size = info.size;
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    return r->_errno = 0;
}

DIR_ITER* devoptab_diropen(struct _reent *r, DIR_ITER *dirState, const char *_path) {
    auto device = (Device*)r->deviceData;
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        set_errno(r, ENOENT);
        return NULL;
    }

    if (!std::strcmp(path, "/")) {
        dir->device = device;
    } else {
        set_errno(r, ENOENT);
        return NULL;
    }

    r->_errno = 0;
    return dirState;
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);

    dir->index = 0;

    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(filestat, 0, sizeof(*filestat));

    do {
        if (dir->index >= plsrBFSARSoundCount(&dir->device->bfsar)) {
            log_write("finished getting call entries: %u vs %u\n", dir->index, plsrBFSARSoundCount(&dir->device->bfsar));
            return set_errno(r, ENOENT);
        }

        PLSR_BFSARSoundInfo info{};
        if (R_FAILED(plsrBFSARSoundGet(&dir->device->bfsar, dir->index, &info))) {
            continue;
        }

        // todo: skip this entry.
        if (!info.hasStringIndex) {
            continue;
        }

        if (R_FAILED(plsrBFSARStringGet(&dir->device->bfsar, info.stringIndex, filename, NAME_MAX))) {
            continue;
        }

        switch (info.type) {
            case PLSR_BFSARSoundType_Wave:
                std::strcat(filename, ".bfwav");
                break;
            case PLSR_BFSARSoundType_Sequence:
                // std::strcat(filename, ".bfseq");
                continue;
            case PLSR_BFSARSoundType_Stream:
                // std::strcat(filename, ".bfstm");
                continue;
            default:
                continue;
        }

        filestat->st_nlink = 1;
        filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        dir->index++;
        break;
    } while (dir->index++);

    return r->_errno = 0;
}

int devoptab_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));

    log_write("[BFSAR] devoptab_dirclose\n");
    return r->_errno = 0;
}

int devoptab_lstat(struct _reent *r, const char *_path, struct stat *st) {
    auto device = (Device*)r->deviceData;

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    std::memset(st, 0, sizeof(*st));

    if (!std::strcmp(path, "/")) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        PLSR_BFWARFileInfo info{};
        if (R_FAILED(GetFileInfo(&device->bfsar, path, info))) {
            return set_errno(r, ENOENT);
        }

        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_size = info.size;
    }

    st->st_nlink = 1;

    return r->_errno = 0;
}

constexpr devoptab_t DEVOPTAB = {
    .structSize   = sizeof(File),
    .open_r       = devoptab_open,
    .close_r      = devoptab_close,
    .read_r       = devoptab_read,
    .seek_r       = devoptab_seek,
    .fstat_r      = devoptab_fstat,
    .stat_r       = devoptab_lstat,
    .dirStateSize = sizeof(Dir),
    .diropen_r    = devoptab_diropen,
    .dirreset_r   = devoptab_dirreset,
    .dirnext_r    = devoptab_dirnext,
    .dirclose_r   = devoptab_dirclose,
    .lstat_r      = devoptab_lstat,
};

struct Entry {
    Device device{};
    devoptab_t devoptab{};
    fs::FsPath path{};
    fs::FsPath mount{};
    char name[32]{};
    s32 ref_count{};

    ~Entry() {
        log_write("[BFSAR] entry called\n");
        RemoveDevice(mount);
        plsrBFSARClose(&device.bfsar);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountBfsar(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path) {
    SCOPED_MUTEX(&g_mutex);

    // check if we already have the save mounted.
    for (auto& e : g_entries) {
        if (e && e->path == path) {
            e->ref_count++;
            out_path = e->mount;
            R_SUCCEED();
        }
    }

    // otherwise, find next free entry.
    auto itr = std::ranges::find_if(g_entries, [](auto& e){
        return !e;
    });
    R_UNLESS(itr != g_entries.end(), 0x1);

    const auto index = std::distance(g_entries.begin(), itr);

    auto entry = std::make_unique<Entry>();
    entry->path = path;
    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    std::snprintf(entry->name, sizeof(entry->name), "BFSAR_%zu", index);
    std::snprintf(entry->mount, sizeof(entry->mount), "BFSAR_%zu:/", index);

    PLSR_RC_TRY(plsrBFSAROpen(path, &entry->device.bfsar));
    entry->device.file = entry->device.bfsar.ar.handle->f;

    R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
    log_write("[BFSAR] DEVICE SUCCESS %s %s\n", path.s, entry->name);

    out_path = entry->mount;
    entry->ref_count++;
    *itr = std::move(entry);

    R_SUCCEED();
}

void UmountBfsar(const fs::FsPath& mount) {
    SCOPED_MUTEX(&g_mutex);

    auto itr = std::ranges::find_if(g_entries, [&mount](auto& e){
        return e && e->mount == mount;
    });

    if (itr == g_entries.end()) {
        return;
    }

    if ((*itr)->ref_count) {
        (*itr)->ref_count--;
    }

    if (!(*itr)->ref_count) {
        itr->reset();
    }
}

} // namespace sphaira::devoptab
