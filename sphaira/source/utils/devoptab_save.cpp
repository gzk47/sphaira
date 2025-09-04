
#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "defines.hpp"
#include "log.hpp"

#include "yati/nx/nxdumptool/defines.h"
#include "yati/nx/nxdumptool/core/save.h"

#include <cstring>
#include <cerrno>
#include <array>
#include <memory>
#include <algorithm>
#include <sys/iosupport.h>

namespace sphaira::devoptab {
namespace {

struct Device {
    save_ctx_t* ctx;
    hierarchical_save_file_table_ctx_t* file_table;
};

struct File {
    Device* device;
    save_fs_list_entry_t entry;
    allocation_table_storage_ctx_t storage;
    size_t off;
};

struct DirNext {
    u32 next_directory;
    u32 next_file;
};

struct Dir {
    Device* device;
    save_fs_list_entry_t entry;
    u32 next_directory;
    u32 next_file;
};

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

    if (!save_hierarchical_file_table_get_file_entry_by_path(device->file_table, path, &file->entry)) {
        return set_errno(r, ENOENT);
    }

    if (!save_open_fat_storage(&device->ctx->save_filesystem_core, &file->storage, file->entry.value.save_file_info.start_block)) {
        return set_errno(r, ENOENT);
    }

    file->device = device;
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    std::memset(file, 0, sizeof(*file));

    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);

    // todo: maybe eof here?
    const auto bytes_read = save_allocation_table_storage_read(&file->storage, ptr, file->off, len);
    if (!bytes_read) {
        return set_errno(r, ENOENT);
    }

    file->off += bytes_read;
    return bytes_read;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = file->storage._length;
    }

    r->_errno = 0;
    return file->off = std::clamp<u64>(pos, 0, file->storage._length);
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);

    log_write("[\t\tDEV] fstat\n");
    std::memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_size = file->storage._length;
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
        save_entry_key_t key{};
        const auto idx = save_fs_list_get_index_from_key(&device->file_table->directory_table, &key, NULL);
        if (idx == 0xFFFFFFFF) {
            set_errno(r, ENOENT);
            return NULL;
        }

        if (!save_fs_list_get_value(&device->file_table->directory_table, idx, &dir->entry)) {
            set_errno(r, ENOENT);
            return NULL;
        }
    } else if (!save_hierarchical_directory_table_get_file_entry_by_path(device->file_table, path, &dir->entry)) {
        set_errno(r, ENOENT);
        return NULL;
    }

    dir->device = device;
    dir->next_file = dir->entry.value.save_find_position.next_file;
    dir->next_directory = dir->entry.value.save_find_position.next_directory;

    r->_errno = 0;
    return dirState;
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);

    dir->next_file = dir->entry.value.save_find_position.next_file;
    dir->next_directory = dir->entry.value.save_find_position.next_directory;

    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);

    std::memset(filestat, 0, sizeof(*filestat));
    save_fs_list_entry_t entry{};

    if (dir->next_directory) {
        // todo: use save_allocation_table_storage_read for faster reads
        if (!save_fs_list_get_value(&dir->device->file_table->directory_table, dir->next_directory, &entry)) {
            return set_errno(r, ENOENT);
        }

        filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        dir->next_directory = entry.value.next_sibling;
    }
    else if (dir->next_file) {
        // todo: use save_allocation_table_storage_read for faster reads
        if (!save_fs_list_get_value(&dir->device->file_table->file_table, dir->next_file, &entry)) {
            return set_errno(r, ENOENT);
        }

        filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        // todo: confirm this.
        filestat->st_size = entry.value.save_file_info.length;
        // filestat->st_size = file->storage.block_size;
        dir->next_file = entry.value.next_sibling;
    }
    else {
        return set_errno(r, ENOENT);
    }

    filestat->st_nlink = 1;
    strcpy(filename, entry.name);

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
    save_fs_list_entry_t entry{};

    // NOTE: this is very slow.
    if (save_hierarchical_file_table_get_file_entry_by_path(device->file_table, path, &entry)) {
        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_size = entry.value.save_file_info.length;
    } else if (save_hierarchical_directory_table_get_file_entry_by_path(device->file_table, path, &entry)) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        return set_errno(r, ENOENT);
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
    u64 id{};
    fs::FsPath mount{};
    char name[32]{};
    s32 ref_count{};

    ~Entry() {
        RemoveDevice(mount);
        save_close_savefile(&device.ctx);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountSaveSystem(u64 id, fs::FsPath& out_path) {
    SCOPED_MUTEX(&g_mutex);

    // check if we already have the save mounted.
    for (auto& e : g_entries) {
        if (e && e->id == id) {
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

    char path[256];
    std::snprintf(path, sizeof(path), "SYSTEM:/save/%016lx", id);

    auto ctx = save_open_savefile(path, 0);
    if (!ctx) {
        R_THROW(0x1);
    }

    log_write("[SAVE] OPEN SUCCESS %s\n", path);

    auto entry = std::make_unique<Entry>();
    entry->id = id;
    entry->device.ctx = ctx;
    entry->device.file_table = &ctx->save_filesystem_core.file_table;
    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    std::snprintf(entry->name, sizeof(entry->name), "%016lx", id);
    std::snprintf(entry->mount, sizeof(entry->mount), "%016lx:/", id);

    R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
    log_write("[SAVE] DEVICE SUCCESS %s %s\n", path, entry->name);

    out_path = entry->mount;
    entry->ref_count++;
    *itr = std::move(entry);

    R_SUCCEED();
}

void UnmountSave(u64 id) {
    SCOPED_MUTEX(&g_mutex);

    auto itr = std::ranges::find_if(g_entries, [id](auto& e){
        return e && e->id == id;
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
