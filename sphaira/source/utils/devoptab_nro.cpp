#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "utils/devoptab_romfs.hpp"

#include "defines.hpp"
#include "log.hpp"
#include "nro.hpp"

#include "yati/nx/es.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/keys.hpp"
#include "yati/nx/crypto.hpp"
#include "yati/container/nsp.hpp"
#include "yati/source/file.hpp"

#include <cstring>
#include <cerrno>
#include <array>
#include <memory>
#include <algorithm>
#include <sys/iosupport.h>

namespace sphaira::devoptab {
namespace {

struct AssetCollection {
    u64 offset;
    u64 size;
};

struct NamedCollection {
    std::string name; // exeFS, RomFS, Logo.
    bool is_romfs;
    AssetCollection asset_collection;
    romfs::RomfsCollection romfs_collections;
};

struct FileEntry {
    bool is_romfs;
    romfs::FileEntry romfs;
    const AssetCollection* asset;
    u64 offset;
    u64 size;
};

struct DirEntry {
    bool is_romfs;
    romfs::DirEntry romfs;
};

struct Device {
    std::unique_ptr<yati::source::Base> source;
    std::vector<NamedCollection> collections;
    FsTimeStampRaw timestamp;
};

struct File {
    Device* device;
    FileEntry entry;
    size_t off;
};

struct Dir {
    Device* device;
    DirEntry entry;
    u32 index;
    bool is_root;
};

bool find_file(std::span<NamedCollection> named, std::string_view path, FileEntry& out) {
    for (auto& e : named) {
        if (path.starts_with("/" + e.name)) {
            out.is_romfs = e.is_romfs;

            const auto rel_name = path.substr(e.name.length() + 1);

            if (out.is_romfs) {
                if (!romfs::find_file(e.romfs_collections, rel_name, out.romfs)) {
                    return false;
                }

                out.offset = out.romfs.offset;
                out.size = out.romfs.size;
                return true;
            } else {
                out.offset = e.asset_collection.offset;
                out.size = e.asset_collection.size;
                return true;
            }
        }
    }

    return false;
}

bool find_dir(std::span<const NamedCollection> named, std::string_view path, DirEntry& out) {
    for (auto& e : named) {
        if (path.starts_with("/" + e.name)) {
            out.is_romfs = e.is_romfs;

            const auto rel_name = path.substr(e.name.length() + 1);

            if (out.is_romfs) {
                return romfs::find_dir(e.romfs_collections, rel_name, out.romfs);
            } else {
                log_write("[NROFS] invalid fs type in find file\n");
                return false;
            }
        }
    }

    return false;
}

void fill_timestamp_from_device(const Device* device, struct stat *st) {
    st->st_atime = device->timestamp.accessed;
    st->st_ctime = device->timestamp.created;
    st->st_mtime = device->timestamp.modified;
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

    FileEntry entry;
    if (!find_file(device->collections, path, entry)) {
        log_write("[NROFS] failed to find file entry\n");
        return set_errno(r, ENOENT);
    }

    file->device = device;
    file->entry = entry;

    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    std::memset(file, 0, sizeof(*file));

    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    const auto& entry = file->entry;

    u64 bytes_read;
    len = std::min(len, entry.size - file->off);
    if (R_FAILED(file->device->source->Read(ptr, entry.offset + file->off, len, &bytes_read))) {
        return set_errno(r, ENOENT);
    }

    file->off += bytes_read;
    return bytes_read;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    const auto& entry = file->entry;

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = entry.size;
    }

    r->_errno = 0;
    return file->off = std::clamp<u64>(pos, 0, entry.size);
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    const auto& entry = file->entry;

    std::memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_size = entry.size;
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    fill_timestamp_from_device(file->device, st);

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
        dir->is_root = true;
        r->_errno = 0;
        return dirState;
    } else {
        DirEntry entry;
        if (!find_dir(device->collections, path, entry)) {
            set_errno(r, ENOENT);
            return NULL;
        }

        dir->device = device;
        dir->entry = entry;

        r->_errno = 0;
        return dirState;
    }
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    auto& entry = dir->entry;

    if (dir->is_root) {
        dir->index = 0;
    } else {
        if (entry.is_romfs) {
            romfs::dirreset(entry.romfs);
        }
    }

    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    auto& entry = dir->entry;
    std::memset(filestat, 0, sizeof(*filestat));

    if (dir->is_root) {
        if (dir->index >= dir->device->collections.size()) {
            return set_errno(r, ENOENT);
        }

        const auto& e = dir->device->collections[dir->index];
        if (e.is_romfs) {
            filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        } else {
            filestat->st_size = e.asset_collection.size;
            filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        }

        filestat->st_nlink = 1;
        std::strcpy(filename, e.name.c_str());
    } else {
        if (entry.is_romfs) {
            if (!romfs::dirnext(entry.romfs, filename, filestat)) {
                return set_errno(r, ENOENT);
            }
        }
    }

    fill_timestamp_from_device(dir->device, filestat);
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

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    std::memset(st, 0, sizeof(*st));
    st->st_nlink = 1;

    if (!std::strcmp(path, "/")) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        // can be optimised for romfs.
        FileEntry file_entry;
        DirEntry dir_entry;
        if (find_file(device->collections, path, file_entry)) {
            st->st_size = file_entry.size;
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        } else if (find_dir(device->collections, path, dir_entry)) {
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        } else {
            return set_errno(r, ENOENT);
        }
    }

    fill_timestamp_from_device(device, st);
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

bool IsAlreadyMounted(const fs::FsPath& path, fs::FsPath& out_path) {
    // check if we already have the save mounted.
    for (auto& e : g_entries) {
        if (e && e->path == path) {
            e->ref_count++;
            out_path = e->mount;
            return true;
        }
    }

    return false;
}

} // namespace

Result MountNro(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path) {
    SCOPED_MUTEX(&g_mutex);

    if (IsAlreadyMounted(path, out_path)) {
        R_SUCCEED();
    }

    // otherwise, find next free entry.
    auto itr = std::ranges::find_if(g_entries, [](auto& e){
        return !e;
    });
    R_UNLESS(itr != g_entries.end(), 0x1);

    const auto index = std::distance(g_entries.begin(), itr);
    auto source = std::make_unique<yati::source::File>(fs, path);

    NroData data;
    R_TRY(source->Read2(&data, 0, sizeof(data)));
    R_UNLESS(data.header.magic == NROHEADER_MAGIC, Result_NroBadMagic);

    NroAssetHeader asset;
    R_TRY(source->Read2(&asset, data.header.size, sizeof(asset)));
    R_UNLESS(asset.magic == NROASSETHEADER_MAGIC, Result_NroBadMagic);

    std::vector<NamedCollection> collections;

    if (asset.icon.size) {
        NamedCollection collection{"icon.jpg", false, AssetCollection{data.header.size + asset.icon.offset, asset.icon.size}};
        collections.emplace_back(collection);
    }
    if (asset.nacp.size) {
        NamedCollection collection{"control.nacp", false, AssetCollection{data.header.size + asset.nacp.offset, asset.nacp.size}};
        collections.emplace_back(collection);
    }
    if (asset.romfs.size) {
        NamedCollection collection{"RomFS", true};
        if (R_SUCCEEDED(romfs::LoadRomfsCollection(source.get(), data.header.size + asset.romfs.offset, collection.romfs_collections))) {
            collections.emplace_back(collection);
        }
    }

    R_UNLESS(!collections.empty(), 0x9);

    auto entry = std::make_unique<Entry>();
    entry->path = path;
    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    entry->device.source = std::move(source);
    entry->device.collections = collections;
    fs->GetFileTimeStampRaw(path, &entry->device.timestamp);
    std::snprintf(entry->name, sizeof(entry->name), "nro_%zu", index);
    std::snprintf(entry->mount, sizeof(entry->mount), "nro_%zu:/", index);

    R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
    log_write("[NRO] DEVICE SUCCESS %s %s\n", path.s, entry->name);

    out_path = entry->mount;
    entry->ref_count++;
    *itr = std::move(entry);

    R_SUCCEED();
}

void UmountNro(const fs::FsPath& mount) {
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
