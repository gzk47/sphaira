
#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "defines.hpp"
#include "log.hpp"

#include "yati/container/nsp.hpp"
#include "yati/container/xci.hpp"
#include "yati/source/file.hpp"

#include <cstring>
#include <cerrno>
#include <array>
#include <memory>
#include <algorithm>
#include <sys/iosupport.h>

namespace sphaira::devoptab {
namespace {

struct Device {
    std::unique_ptr<common::LruBufferedData> source;
    yati::container::Collections collections;
};

struct File {
    Device* device;
    const yati::container::CollectionEntry* collection;
    size_t off;
};

struct Dir {
    Device* device;
    u32 index;
};

int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int devoptab_open(struct _reent *r, void *fileStruct, const char *_path, int flags, int mode) {
    auto device = (Device*)r->deviceData;
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));

    char path[FS_MAX_PATH];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    for (const auto& collection : device->collections) {
        if (path == "/" + collection.name) {
            file->device = device;
            file->collection = &collection;
            return r->_errno = 0;
        }
    }

    return set_errno(r, ENOENT);
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    std::memset(file, 0, sizeof(*file));

    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);

    const auto& collection = file->collection;
    len = std::min(len, collection->size - file->off);

    u64 bytes_read;
    if (R_FAILED(file->device->source->Read(ptr, collection->offset + file->off, len, &bytes_read))) {
        return set_errno(r, ENOENT);
    }

    file->off += bytes_read;
    return bytes_read;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    const auto& collection = file->collection;

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = collection->size;
    }

    r->_errno = 0;
    return file->off = std::clamp<u64>(pos, 0, collection->size);
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    const auto& collection = file->collection;

    std::memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_size = collection->size;
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    return r->_errno = 0;
}

DIR_ITER* devoptab_diropen(struct _reent *r, DIR_ITER *dirState, const char *_path) {
    auto device = (Device*)r->deviceData;
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));

    char path[FS_MAX_PATH];
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

    if (dir->index >= dir->device->collections.size()) {
        return set_errno(r, ENOENT);
    }

    const auto& collection = dir->device->collections[dir->index];
    filestat->st_nlink = 1;
    filestat->st_size = collection.size;
    filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    std::strcpy(filename, collection.name.c_str());

    dir->index++;
    return r->_errno = 0;
}

int devoptab_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));

    return r->_errno = 0;
}

int devoptab_lstat(struct _reent *r, const char *_path, struct stat *st) {
    auto device = (Device*)r->deviceData;

    char path[FS_MAX_PATH];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    std::memset(st, 0, sizeof(*st));

    if (!std::strcmp(path, "/")) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        const auto it = std::ranges::find_if(device->collections, [path](auto& e){
            return path == "/" + e.name;
        });

        if (it == device->collections.end()) {
            return set_errno(r, ENOENT);
        }

        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_size = it->size;
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
        RemoveDevice(mount);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountNsp(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path) {
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
    auto source = std::make_shared<yati::source::File>(fs, path);

    s64 size;
    R_TRY(source->GetSize(&size));
    auto buffered = std::make_unique<common::LruBufferedData>(source, size);

    yati::container::Nsp nsp{buffered.get()};
    yati::container::Collections collections;
    R_TRY(nsp.GetCollections(collections));

    auto entry = std::make_unique<Entry>();
    entry->path = path;
    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    entry->device.source = std::move(buffered);
    entry->device.collections = collections;
    std::snprintf(entry->name, sizeof(entry->name), "nsp_%zu", index);
    std::snprintf(entry->mount, sizeof(entry->mount), "nsp_%zu:/", index);

    R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
    log_write("[NSP] DEVICE SUCCESS %s %s\n", path.s, entry->name);

    out_path = entry->mount;
    entry->ref_count++;
    *itr = std::move(entry);

    R_SUCCEED();
}

void UmountNsp(const fs::FsPath& mount) {
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
