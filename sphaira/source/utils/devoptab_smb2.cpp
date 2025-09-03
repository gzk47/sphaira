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
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <minIni.h>

namespace sphaira::devoptab {
namespace {

struct Smb2MountConfig {
    std::string name{};
    std::string url{};
    std::string user{};
    std::string pass{};
    bool read_only{};
};
using Smb2MountConfigs = std::vector<Smb2MountConfig>;

struct Device {
    smb2_context* smb2{};
    smb2_url* url{};
    Smb2MountConfig config;
    bool mounted{};
    Mutex mutex{};
};

struct File {
    Device* device;
    smb2fh* fd;
};

struct Dir {
    Device* device;
    smb2dir* dir;
};

bool mount_smb2(Device& device) {
    if (device.mounted) {
        return true;
    }

    if (!device.smb2) {
        device.smb2 = smb2_init_context();
        if (!device.smb2) {
            log_write("[SMB2] smb2_init_context() failed\n");
            return false;
        }

        smb2_set_security_mode(device.smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

        if (!device.config.user.empty()) {
            smb2_set_user(device.smb2, device.config.user.c_str());
        }

        if (!device.config.pass.empty()) {
            smb2_set_password(device.smb2, device.config.pass.c_str());
        }
    }

    if (!device.url) {
        device.url = smb2_parse_url(device.smb2, device.config.url.c_str());
        if (!device.url) {
            log_write("[SMB2] smb2_parse_url() failed: %s\n", smb2_get_error(device.smb2));
            return false;
        }
    }

    log_write("[SMB2] Connecting to %s/%s as %s\n",
              device.url->server,
              device.url->share,
              device.url->user ? device.url->user : "guest");
    const auto ret = smb2_connect_share(device.smb2, device.url->server, device.url->share, device.url->user);
    if (ret) {
        log_write("[SMB2] smb2_connect_share() failed: %s errno: %s\n", smb2_get_error(device.smb2), std::strerror(-ret));
        return false;
    }

    device.mounted = true;
    return true;
}

void fill_stat(struct stat* st, const smb2_stat_64* smb2_st) {
    if (smb2_st->smb2_type == SMB2_TYPE_FILE) {
        st->st_mode = S_IFREG;
    } else if (smb2_st->smb2_type == SMB2_TYPE_DIRECTORY) {
        st->st_mode = S_IFDIR;
    } else if (smb2_st->smb2_type == SMB2_TYPE_LINK) {
        st->st_mode = S_IFLNK;
    } else {
        log_write("[SMB2] Unknown file type: %u\n", smb2_st->smb2_type);
        st->st_mode = S_IFCHR; // will be skipped by stdio readdir wrapper.
    }

    st->st_ino = smb2_st->smb2_ino;
    st->st_nlink = smb2_st->smb2_nlink;
    st->st_size = smb2_st->smb2_size;
    st->st_atime = smb2_st->smb2_atime;
    st->st_mtime = smb2_st->smb2_mtime;
    st->st_ctime = smb2_st->smb2_ctime;
}

bool fix_path(const char* str, char* out) {
    return common::fix_path(str, out, true);
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
    if (!fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    file->fd = smb2_open(device->smb2, path, flags);
    if (!file->fd) {
        log_write("[SMB2] smb2_open() failed: %s\n", smb2_get_error(device->smb2));
        return set_errno(r, EIO);
    }

    file->device = device;
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    if (file->fd) {
        smb2_close(file->device->smb2, file->fd);
    }

    std::memset(file, 0, sizeof(*file));
    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = smb2_read(file->device->smb2, file->fd, reinterpret_cast<uint8_t*>(ptr), len);
    if (ret < 0) {
        log_write("[SMB2] smb2_read() failed: %s errno: %s\n", smb2_get_error(file->device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return ret;
}

ssize_t devoptab_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = smb2_write(file->device->smb2, file->fd, reinterpret_cast<const uint8_t*>(ptr), len);
    if (ret < 0) {
        log_write("[SMB2] smb2_write() failed: %s errno: %s\n", smb2_get_error(file->device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return ret;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = smb2_lseek(file->device->smb2, file->fd, pos, dir, nullptr);
    if (ret < 0) {
        log_write("[SMB2] smb2_lseek() failed: %s errno: %s\n", smb2_get_error(file->device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    r->_errno = 0;
    return ret;
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    smb2_stat_64 smb2_st{};
    const auto ret = smb2_fstat(file->device->smb2, file->fd, &smb2_st);
    if (ret < 0) {
        log_write("[SMB2] smb2_fstat() failed: %s errno: %s\n", smb2_get_error(file->device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    fill_stat(st, &smb2_st);
    return r->_errno = 0;
}

int devoptab_unlink(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = smb2_unlink(device->smb2, path);
    if (ret < 0) {
        log_write("[SMB2] smb2_unlink() failed: %s errno: %s\n", smb2_get_error(device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_rename(struct _reent *r, const char *_oldName, const char *_newName) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char oldName[FS_MAX_PATH]{};
    if (!fix_path(_oldName, oldName)) {
        return set_errno(r, ENOENT);
    }

    char newName[FS_MAX_PATH]{};
    if (!fix_path(_newName, newName)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = smb2_rename(device->smb2, oldName, newName);
    if (ret < 0) {
        log_write("[SMB2] smb2_rename() failed: %s errno: %s\n", smb2_get_error(device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_mkdir(struct _reent *r, const char *_path, int mode) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = smb2_mkdir(device->smb2, path);
    if (ret) {
        log_write("[SMB2] smb2_mkdir() failed: %s errno: %s\n", smb2_get_error(device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_rmdir(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    const auto ret = smb2_rmdir(device->smb2, path);
    if (ret) {
        log_write("[SMB2] smb2_rmdir() failed: %s errno: %s\n", smb2_get_error(device->smb2), std::strerror(-ret));
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
    if (!fix_path(_path, path)) {
        set_errno(r, ENOENT);
        return nullptr;
    }

    if (!mount_smb2(*device)) {
        set_errno(r, EIO);
        return nullptr;
    }

    dir->dir = smb2_opendir(device->smb2, path);
    if (!dir->dir) {
        log_write("[SMB2] smb2_opendir() failed: %s\n", smb2_get_error(device->smb2));
        set_errno(r, EIO);
        return nullptr;
    }

    dir->device = device;
    return dirState;
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_MUTEX(&dir->device->mutex);

    smb2_rewinddir(dir->device->smb2, dir->dir);
    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(filestat, 0, sizeof(*filestat));
    SCOPED_MUTEX(&dir->device->mutex);

    const auto entry = smb2_readdir(dir->device->smb2, dir->dir);
    if (!entry) {
        return set_errno(r, ENOENT);
    }

    std::strncpy(filename, entry->name, NAME_MAX);
    filename[NAME_MAX - 1] = '\0';
    fill_stat(filestat, &entry->st);

    return r->_errno = 0;
}

int devoptab_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_MUTEX(&dir->device->mutex);

    if (dir->dir) {
        smb2_closedir(dir->device->smb2, dir->dir);
    }

    std::memset(dir, 0, sizeof(*dir));
    return r->_errno = 0;
}

int devoptab_lstat(struct _reent *r, const char *_path, struct stat *st) {
    auto device = static_cast<Device*>(r->deviceData);
    std::memset(st, 0, sizeof(*st));
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    smb2_stat_64 smb2_st{};
    const auto ret = smb2_stat(device->smb2, path, &smb2_st);
    if (ret) {
        log_write("[SMB2] smb2_lstat() failed: %s errno: %s\n", smb2_get_error(device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    fill_stat(st, &smb2_st);
    return r->_errno = 0;
}

int devoptab_ftruncate(struct _reent *r, void *fd, off_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = smb2_ftruncate(file->device->smb2, file->fd, len);
    if (ret) {
        log_write("[SMB2] smb2_ftruncate() failed: %s errno: %s\n", smb2_get_error(file->device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_statvfs(struct _reent *r, const char *_path, struct statvfs *buf) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[FS_MAX_PATH]{};
    if (!fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_smb2(*device)) {
        return set_errno(r, EIO);
    }

    struct smb2_statvfs smb2_st{};
    const auto ret = smb2_statvfs(device->smb2, path, &smb2_st);
    if (ret) {
        log_write("[SMB2] smb2_statvfs() failed: %s errno: %s\n", smb2_get_error(device->smb2), std::strerror(-ret));
        return set_errno(r, -ret);
    }

    buf->f_bsize   = smb2_st.f_bsize;
    buf->f_frsize  = smb2_st.f_frsize;
    buf->f_blocks  = smb2_st.f_blocks;
    buf->f_bfree   = smb2_st.f_bfree;
    buf->f_bavail  = smb2_st.f_bavail;
    buf->f_files   = smb2_st.f_files;
    buf->f_ffree   = smb2_st.f_ffree;
    buf->f_favail  = smb2_st.f_favail;
    buf->f_fsid    = smb2_st.f_fsid;
    buf->f_flag    = smb2_st.f_flag;
    buf->f_namemax = smb2_st.f_namemax;

    return r->_errno = 0;
}

int devoptab_fsync(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = smb2_fsync(file->device->smb2, file->fd);
    if (ret) {
        log_write("[SMB2] smb2_fsync() failed: %s errno: %s\n", smb2_get_error(file->device->smb2), std::strerror(-ret));
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
};

struct Entry {
    Device device{};
    devoptab_t devoptab{};
    fs::FsPath mount{};
    char name[32]{};
    s32 ref_count{};

    ~Entry() {
        if (device.smb2) {
            if (device.mounted) {
                smb2_disconnect_share(device.smb2);
            }

            if (device.url) {
                smb2_destroy_url(device.url);
            }

            smb2_destroy_context(device.smb2);
        }

        RemoveDevice(mount);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountSmb2All() {
    SCOPED_MUTEX(&g_mutex);

    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<Smb2MountConfigs*>(UserData);
        if (!Section || !Key || !Value) {
            return 1;
        }

        // add new entry if use section changed.
        if (e->empty() || std::strcmp(Section, e->back().name.c_str())) {
            e->emplace_back(Section);
        }

        if (!std::strcmp(Key, "url")) {
            e->back().url = Value;
        } else if (!std::strcmp(Key, "name")) {
            e->back().name = Value;
        } else if (!std::strcmp(Key, "user")) {
            e->back().user = Value;
        } else if (!std::strcmp(Key, "pass")) {
            e->back().pass = Value;
        } else if (!std::strcmp(Key, "read_only")) {
            e->back().read_only = ini_parse_getbool(Value, false);
        } else {
            log_write("[SMB2] INI: Unknown key %s=%s\n", Key, Value);
        }

        return 1;
    };

    Smb2MountConfigs configs{};
    ini_browse(cb, &configs, "/config/sphaira/smb.ini");
    log_write("[SMB2] Found %zu mount configs\n", configs.size());

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
            log_write("[SMB2] Already mounted %s, skipping\n", config.name.c_str());
            continue;
        }

        // otherwise, find next free entry.
        auto itr = std::ranges::find_if(g_entries, [](auto& e){
            return !e;
        });

        if (itr == g_entries.end()) {
            log_write("[SMB2] No free entries to mount %s\n", config.name.c_str());
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
        log_write("[SMB2] DEVICE SUCCESS %s %s\n", entry->device.config.url.c_str(), entry->name);

        entry->ref_count++;
        *itr = std::move(entry);
        log_write("[SMB2] Mounted %s at /%s\n", config.user.c_str(), config.name.c_str());
    }

    R_SUCCEED();
}

void UnmountSmb2All() {
    SCOPED_MUTEX(&g_mutex);

    for (auto& entry : g_entries) {
        if (entry) {
            entry.reset();
        }
    }
}

Result GetSmb2Mounts(location::StdioEntries& out) {
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
