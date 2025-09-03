#include "utils/devoptab_common.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "location.hpp"

#include <sys/iosupport.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <cstring>
#include <libnfs.h>
#include <minIni.h>

namespace sphaira::devoptab {
namespace {

struct NfsMountConfig {
    std::string name{};
    std::string url{};
    std::string path{};
    bool read_only{};
};
using NfsMountConfigs = std::vector<NfsMountConfig>;

struct Device {
    nfs_context* nfs{};
    NfsMountConfig config;
    bool mounted{};
    Mutex mutex{};
};

struct File {
    Device* device;
    nfsfh* fd;
};

struct Dir {
    Device* device;
    nfsdir* dir;
};

bool mount_nfs(Device& device) {
    if (device.mounted) {
        return true;
    }

    if (!device.nfs) {
        device.nfs = nfs_init_context();
        if (!device.nfs) {
            log_write("[NFS] nfs_init_context() failed\n");
            return false;
        }
    }

    const auto ret = nfs_mount(device.nfs, device.config.url.c_str(), device.config.path.c_str());
    if (ret) {
        log_write("[NFS] nfs_mount() failed: %s errno: %s\n", nfs_get_error(device.nfs), std::strerror(-ret));
        return false;
    }

    device.mounted = true;
    return true;
}

int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int devoptab_open(struct _reent *r, void *fileStruct, const char *_path, int flags, int mode) {
    auto device = static_cast<Device*>(r->deviceData);
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));
    SCOPED_MUTEX(&device->mutex);

    if (device->config.read_only && (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))) {
        return set_errno(r, EROFS);
    }

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_open(device->nfs, path, flags, &file->fd);
    if (ret) {
        log_write("[NFS] nfs_open() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    file->device = device;
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    if (file->fd) {
        nfs_close(file->device->nfs, file->fd);
    }

    std::memset(file, 0, sizeof(*file));
    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    // todo: uncomment this when it's fixed upstream.
    #if 0
    const auto ret = nfs_read(file->device->nfs, file->fd, ptr, len);
    if (ret < 0) {
        log_write("[NFS] nfs_read() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return ret;

    #else
    // work around for bug upsteam.
    const auto max_read = nfs_get_readmax(file->device->nfs);
    size_t bytes_read = 0;

    while (bytes_read < len) {
        const auto to_read = std::min<size_t>(len - bytes_read, max_read);
        const auto ret = nfs_read(file->device->nfs, file->fd, ptr, to_read);

        if (ret < 0) {
            log_write("[NFS] nfs_read() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
            return set_errno(r, -ret);
        }

        ptr += ret;
        bytes_read += ret;

        if (ret < to_read) {
            break;
        }
    }

    return bytes_read;
    #endif
}

ssize_t devoptab_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    // unlike read, writing the max size seems to work fine.
    const auto max_write = nfs_get_writemax(file->device->nfs) - 1;
    size_t written = 0;

    while (written < len) {
        const auto to_write = std::min<size_t>(len - written, max_write);
        const auto ret = nfs_write(file->device->nfs, file->fd, ptr, to_write);

        if (ret < 0) {
            log_write("[NFS] nfs_write() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
            return set_errno(r, -ret);
        }

        ptr += ret;
        written += ret;

        if (ret < to_write) {
            break;
        }
    }

    return written;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    u64 current_off;
    const auto ret = nfs_lseek(file->device->nfs, file->fd, pos, dir, &current_off);
    if (ret < 0) {
        log_write("[NFS] nfs_lseek() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    r->_errno = 0;
    return current_off;
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = nfs_fstat(file->device->nfs, file->fd, st);
    if (ret < 0) {
        log_write("[NFS] nfs_fstat() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_unlink(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_unlink(device->nfs, path);
    if (ret < 0) {
        log_write("[NFS] nfs_unlink() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_rename(struct _reent *r, const char *_oldName, const char *_newName) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char oldName[FS_MAX_PATH]{};
    if (!common::fix_path(_oldName, oldName)) {
        return set_errno(r, ENOENT);
    }

    char newName[FS_MAX_PATH]{};
    if (!common::fix_path(_newName, newName)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_rename(device->nfs, oldName, newName);
    if (ret < 0) {
        log_write("[NFS] nfs_rename() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_mkdir(struct _reent *r, const char *_path, int mode) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_mkdir(device->nfs, path);
    if (ret) {
        log_write("[NFS] nfs_mkdir() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_rmdir(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_rmdir(device->nfs, path);
    if (ret) {
        log_write("[NFS] nfs_rmdir() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

DIR_ITER* devoptab_diropen(struct _reent *r, DIR_ITER *dirState, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        set_errno(r, ENOENT);
        return nullptr;
    }

    if (!mount_nfs(*device)) {
        set_errno(r, EIO);
        return nullptr;
    }

    const auto ret = nfs_opendir(device->nfs, path, &dir->dir);
    if (ret) {
        log_write("[NFS] nfs_opendir() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        set_errno(r, -ret);
        return nullptr;
    }

    dir->device = device;
    return dirState;
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_MUTEX(&dir->device->mutex);

    nfs_rewinddir(dir->device->nfs, dir->dir);
    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(filestat, 0, sizeof(*filestat));
    SCOPED_MUTEX(&dir->device->mutex);

    const auto entry = nfs_readdir(dir->device->nfs, dir->dir);
    if (!entry) {
        return set_errno(r, ENOENT);
    }

    std::strncpy(filename, entry->name, NAME_MAX);
    filename[NAME_MAX - 1] = '\0';

    // not everything is needed, however we may as well fill it all in.
    filestat->st_dev = entry->dev;
    filestat->st_ino = entry->inode;
    filestat->st_mode = entry->mode;
    filestat->st_nlink = entry->nlink;
    filestat->st_uid = entry->uid;
    filestat->st_gid = entry->gid;
    filestat->st_size = entry->size;
    filestat->st_atime = entry->atime.tv_sec;
    filestat->st_mtime = entry->mtime.tv_sec;
    filestat->st_ctime = entry->ctime.tv_sec;
    filestat->st_blksize = entry->blksize;
    filestat->st_blocks = entry->blocks;

    return r->_errno = 0;
}

int devoptab_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_MUTEX(&dir->device->mutex);

    if (dir->dir) {
        nfs_closedir(dir->device->nfs, dir->dir);
    }

    std::memset(dir, 0, sizeof(*dir));
    return r->_errno = 0;
}

int devoptab_lstat(struct _reent *r, const char *_path, struct stat *st) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_stat(device->nfs, path, st);
    if (ret) {
        log_write("[NFS] nfs_lstat() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_ftruncate(struct _reent *r, void *fd, off_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = nfs_ftruncate(file->device->nfs, file->fd, len);
    if (ret) {
        log_write("[NFS] nfs_ftruncate() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_statvfs(struct _reent *r, const char *_path, struct statvfs *buf) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = nfs_statvfs(device->nfs, path, buf);
    if (ret) {
        log_write("[NFS] nfs_statvfs() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_fsync(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = nfs_fsync(file->device->nfs, file->fd);
    if (ret) {
        log_write("[NFS] nfs_fsync() failed: %s errno: %s\n", nfs_get_error(file->device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_utimes(struct _reent *r, const char *_path, const struct timeval times[2]) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    if (!times) {
        log_write("[NFS] devoptab_utimes() times is null\n");
        return set_errno(r, EINVAL);
    }

    char path[FS_MAX_PATH]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_nfs(*device)) {
        return set_errno(r, EIO);
    }

    // todo: nfs should accept const times, pr the fix.
    struct timeval times_copy[2];
    std::memcpy(times_copy, times, sizeof(times_copy));

    const auto ret = nfs_utimes(device->nfs, path, times_copy);
    if (ret) {
        log_write("[NFS] nfs_utimes() failed: %s errno: %s\n", nfs_get_error(device->nfs), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

constexpr devoptab_t DEVOPTAB = {
    .structSize   = sizeof(File),
    .open_r       = devoptab_open,
    .close_r      = devoptab_close,
    .write_r      = devoptab_write,
    .read_r       = devoptab_read,
    .seek_r       = devoptab_seek,
    .fstat_r      = devoptab_fstat,
    .stat_r       = devoptab_lstat,
    .unlink_r     = devoptab_unlink,
    .rename_r     = devoptab_rename,
    .mkdir_r      = devoptab_mkdir,
    .dirStateSize = sizeof(Dir),
    .diropen_r    = devoptab_diropen,
    .dirreset_r   = devoptab_dirreset,
    .dirnext_r    = devoptab_dirnext,
    .dirclose_r   = devoptab_dirclose,
    .statvfs_r    = devoptab_statvfs,
    .ftruncate_r  = devoptab_ftruncate,
    .fsync_r      = devoptab_fsync,
    .rmdir_r      = devoptab_rmdir,
    .lstat_r      = devoptab_lstat,
    .utimes_r     = devoptab_utimes,
};

struct Entry {
    Device device{};
    devoptab_t devoptab{};
    fs::FsPath mount{};
    char name[32]{};
    s32 ref_count{};

    ~Entry() {
        if (device.nfs) {
            if (device.mounted) {
                nfs_umount(device.nfs);
            }
            nfs_destroy_context(device.nfs);
        }

        RemoveDevice(mount);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountNfsAll() {
    SCOPED_MUTEX(&g_mutex);

    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<NfsMountConfigs*>(UserData);
        if (!Section || !Key || !Value) {
            return 1;
        }

        // add new entry if use section changed.
        if (e->empty() || std::strcmp(Section, e->back().name.c_str())) {
            e->emplace_back(Section);
        }

        if (!std::strcmp(Key, "url")) {
            e->back().url = Value;
        } else if (!std::strcmp(Key, "path")) {
            e->back().path = Value;
        } else if (!std::strcmp(Key, "name")) {
            e->back().name = Value;
        } else if (!std::strcmp(Key, "read_only")) {
            e->back().read_only = ini_parse_getbool(Value, false);
        } else {
            log_write("[NFS] INI: Unknown key %s=%s\n", Key, Value);
        }

        return 1;
    };

    NfsMountConfigs configs;
    ini_browse(cb, &configs, "/config/sphaira/nfs.ini");
    log_write("[NFS] Found %zu mount configs\n", configs.size());

    for (const auto& config : configs) {
        // check if we already have the http mounted.
        bool already_mounted = false;
        for (const auto& entry : g_entries) {
            if (entry && entry->mount == config.name) {
                already_mounted = true;
                break;
            }
        }

        if (already_mounted) {
            log_write("[NFS] Already mounted %s, skipping\n", config.name.c_str());
            continue;
        }

        // otherwise, find next free entry.
        auto itr = std::ranges::find_if(g_entries, [](auto& e){
            return !e;
        });

        if (itr == g_entries.end()) {
            log_write("[NFS] No free entries to mount %s\n", config.name.c_str());
            break;
        }

        auto entry = std::make_unique<Entry>();

        entry->devoptab = DEVOPTAB;
        entry->devoptab.name = entry->name;
        entry->devoptab.deviceData = &entry->device;
        entry->device.config = config;
        std::snprintf(entry->name, sizeof(entry->name), "%s", config.name.c_str());
        std::snprintf(entry->mount, sizeof(entry->mount), "%s:/", config.name.c_str());
        common::update_devoptab_for_read_only(&entry->devoptab, config.read_only);

        R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
        log_write("[NFS] DEVICE SUCCESS %s %s\n", entry->device.config.url.c_str(), entry->name);

        entry->ref_count++;
        *itr = std::move(entry);
        log_write("[NFS] Mounted %s at /%s\n", config.url.c_str(), config.name.c_str());
    }

    R_SUCCEED();
}

void UnmountNfsAll() {
    SCOPED_MUTEX(&g_mutex);

    for (auto& entry : g_entries) {
        if (entry) {
            entry.reset();
        }
    }
}

Result GetNfsMounts(location::StdioEntries& out) {
    SCOPED_MUTEX(&g_mutex);
    out.clear();

    for (const auto& entry : g_entries) {
        if (entry) {
            out.emplace_back(entry->mount, entry->name, entry->device.config.read_only);
        }
    }

    R_SUCCEED();
}

} // namespace sphaira::devoptab
