#include "ftpsrv_helper.hpp"

#include "app.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "utils/thread.hpp"

#include <algorithm>
#include <minIni.h>
#include <ftpsrv.h>
#include <ftpsrv_vfs.h>
#include <nx/vfs_nx.h>
#include <nx/utils.h>
#include <unistd.h>
#include <fcntl.h>

namespace sphaira::ftpsrv {
namespace {

struct InstallSharedData {
    Mutex mutex;
    std::deque<std::string> queued_files;

    void* user;
    OnInstallStart on_start;
    OnInstallWrite on_write;
    OnInstallClose on_close;

    bool in_progress;
    bool enabled;
};

const char* INI_PATH = "/config/ftpsrv/config.ini";
FtpSrvConfig g_ftpsrv_config = {0};
std::atomic_bool g_should_exit = false;
bool g_is_running{false};
Thread g_thread;
Mutex g_mutex{};

void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    log_write("[FTPSRV] %s\n", msg);
    sphaira::App::NotifyFlashLed();
}

void ftp_progress_callback(void) {
    sphaira::App::NotifyFlashLed();
}

InstallSharedData g_shared_data{};

const char* SUPPORTED_EXT[] = {
    ".nsp", ".xci", ".nsz", ".xcz",
};

struct VfsUserData {
    char* path;
    int valid;
};

// ive given up with good names.
void on_thing() {
    log_write("[FTP] doing on_thing\n");
    SCOPED_MUTEX(&g_shared_data.mutex);
    log_write("[FTP] locked on_thing\n");

    if (!g_shared_data.in_progress) {
        if (!g_shared_data.queued_files.empty()) {
            log_write("[FTP] pushing new file data\n");
            if (!g_shared_data.on_start || !g_shared_data.on_start(g_shared_data.queued_files[0].c_str())) {
                g_shared_data.queued_files.clear();
            } else {
                log_write("[FTP] success on new file push\n");
                g_shared_data.in_progress = true;
            }
        }
    }
}

int vfs_install_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    {
        SCOPED_MUTEX(&g_shared_data.mutex);
        auto data = static_cast<VfsUserData*>(user);
        data->valid = 0;

        if (mode != FtpVfsOpenMode_WRITE) {
            errno = EACCES;
            return -1;
        }

        if (!g_shared_data.enabled) {
            errno = EACCES;
            return -1;
        }

        const char* ext = strrchr(path, '.');
        if (!ext) {
            errno = EACCES;
            return -1;
        }

        bool found = false;
        for (size_t i = 0; i < std::size(SUPPORTED_EXT); i++) {
            if (!strcasecmp(ext, SUPPORTED_EXT[i])) {
                found = true;
                break;
            }
        }

        if (!found) {
            errno = EINVAL;
            return -1;
        }

        // check if we already have this file queued.
        auto it = std::find(g_shared_data.queued_files.cbegin(), g_shared_data.queued_files.cend(), path);
        if (it != g_shared_data.queued_files.cend()) {
            errno = EEXIST;
            return -1;
        }

        g_shared_data.queued_files.push_back(path);
        data->path = strdup(path);
        data->valid = true;
    }

    on_thing();
    log_write("[FTP] got file: %s\n", path);
    return 0;
}

int vfs_install_read(void* user, void* buf, size_t size) {
    errno = EACCES;
    return -1;
}

int vfs_install_write(void* user, const void* buf, size_t size) {
    SCOPED_MUTEX(&g_shared_data.mutex);
    if (!g_shared_data.enabled) {
        errno = EACCES;
        return -1;
    }

    auto data = static_cast<VfsUserData*>(user);
    if (!data->valid) {
        errno = EACCES;
        return -1;
    }

    if (!g_shared_data.on_write || !g_shared_data.on_write(buf, size)) {
        errno = EIO;
        return -1;
    }

    return size;
}

int vfs_install_seek(void* user, const void* buf, size_t size, size_t off) {
    errno = ESPIPE;
    return -1;
}

int vfs_install_isfile_open(void* user) {
    SCOPED_MUTEX(&g_shared_data.mutex);
    auto data = static_cast<VfsUserData*>(user);
    return data->valid;
}

int vfs_install_isfile_ready(void* user) {
    SCOPED_MUTEX(&g_shared_data.mutex);
    auto data = static_cast<VfsUserData*>(user);
    const auto ready = !g_shared_data.queued_files.empty() && data->path == g_shared_data.queued_files[0];
    return ready;
}

int vfs_install_close(void* user) {
    {
        log_write("[FTP] closing file\n");
        SCOPED_MUTEX(&g_shared_data.mutex);
        auto data = static_cast<VfsUserData*>(user);
        if (data->valid) {
            log_write("[FTP] closing valid file\n");

            auto it = std::find(g_shared_data.queued_files.cbegin(), g_shared_data.queued_files.cend(), data->path);
            if (it != g_shared_data.queued_files.cend()) {
                if (it == g_shared_data.queued_files.cbegin()) {
                    log_write("[FTP] closing current file\n");
                    if (g_shared_data.on_close) {
                        g_shared_data.on_close();
                    }

                    g_shared_data.in_progress = false;
                } else {
                    log_write("[FTP] closing other file...\n");
                }

                g_shared_data.queued_files.erase(it);
            } else {
                log_write("[FTP] could not find file in queue...\n");
            }

            if (data->path) {
                free(data->path);
            }

            data->valid = 0;
        }

        memset(data, 0, sizeof(*data));
    }

    on_thing();
    return 0;
}

int vfs_install_opendir(void* user, const char* path) {
    return 0;
}

const char* vfs_install_readdir(void* user, void* user_entry) {
    return NULL;
}

int vfs_install_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    st->st_nlink = 1;
    st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    return 0;
}

int vfs_install_isdir_open(void* user) {
    return 1;
}

int vfs_install_closedir(void* user) {
    return 0;
}

int vfs_install_stat(const char* path, struct stat* st) {
    st->st_nlink = 1;
    st->st_mode = S_IFDIR | S_IWUSR | S_IWGRP | S_IWOTH;
    return 0;
}

int vfs_install_mkdir(const char* path) {
    return -1;
}

int vfs_install_unlink(const char* path) {
    return -1;
}

int vfs_install_rmdir(const char* path) {
    return -1;
}

int vfs_install_rename(const char* src, const char* dst) {
    return -1;
}

FtpVfs g_vfs_install = {
    .open = vfs_install_open,
    .read = vfs_install_read,
    .write = vfs_install_write,
    .seek = vfs_install_seek,
    .close = vfs_install_close,
    .isfile_open = vfs_install_isfile_open,
    .isfile_ready = vfs_install_isfile_ready,
    .opendir = vfs_install_opendir,
    .readdir = vfs_install_readdir,
    .dirlstat = vfs_install_dirlstat,
    .closedir = vfs_install_closedir,
    .isdir_open = vfs_install_isdir_open,
    .stat = vfs_install_stat,
    .lstat = vfs_install_stat,
    .mkdir = vfs_install_mkdir,
    .unlink = vfs_install_unlink,
    .rmdir = vfs_install_rmdir,
    .rename = vfs_install_rename,
};

struct FtpVfsFile {
    int fd;
    int valid;
};

struct FtpVfsDir {
    DIR* fd;
};

struct FtpVfsDirEntry {
    struct dirent* buf;
};

auto vfs_stdio_fix_path(const char* str) -> fs::FsPath {
    while (*str == '/') {
        str++;
    }

    fs::FsPath out = str;
    if (out.ends_with(":")) {
        out += '/';
    }

    return out;
}

int vfs_stdio_open(void* user, const char* _path, enum FtpVfsOpenMode mode) {
    auto f = static_cast<FtpVfsFile*>(user);
    const auto path = vfs_stdio_fix_path(_path);

    int flags = 0, args = 0;
    switch (mode) {
        case FtpVfsOpenMode_READ:
            flags = O_RDONLY;
            args = 0;
            break;
        case FtpVfsOpenMode_WRITE:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            args = 0666;
            break;
        case FtpVfsOpenMode_APPEND:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            args = 0666;
            break;
    }

    f->fd = open(path, flags, args);
    if (f->fd >= 0) {
        f->valid = 1;
    }

    return f->fd;
}

int vfs_stdio_read(void* user, void* buf, size_t size) {
    auto f = static_cast<FtpVfsFile*>(user);
    return read(f->fd, buf, size);
}

int vfs_stdio_write(void* user, const void* buf, size_t size) {
    auto f = static_cast<FtpVfsFile*>(user);
    return write(f->fd, buf, size);
}

int vfs_stdio_seek(void* user, const void* buf, size_t size, size_t off) {
    auto f = static_cast<FtpVfsFile*>(user);
    const auto pos = lseek(f->fd, off, SEEK_SET);
    if (pos < 0) {
        return -1;
    }
    return 0;
}

int vfs_stdio_isfile_open(void* user) {
    auto f = static_cast<FtpVfsFile*>(user);
    return f->valid && f->fd >= 0;
}

int vfs_stdio_close(void* user) {
    auto f = static_cast<FtpVfsFile*>(user);
    int rc = 0;
    if (vfs_stdio_isfile_open(f)) {
        rc = close(f->fd);
        f->fd = -1;
        f->valid = 0;
    }
    return rc;
}

int vfs_stdio_opendir(void* user, const char* _path) {
    auto f = static_cast<FtpVfsDir*>(user);
    const auto path = vfs_stdio_fix_path(_path);

    f->fd = opendir(path);
    if (!f->fd) {
        return -1;
    }
    return 0;
}

const char* vfs_stdio_readdir(void* user, void* user_entry) {
    auto f = static_cast<FtpVfsDir*>(user);
    auto entry = static_cast<FtpVfsDirEntry*>(user_entry);

    entry->buf = readdir(f->fd);
    if (!entry->buf) {
        return NULL;
    }
    return entry->buf->d_name;
}

int vfs_stdio_dirlstat(void* user, const void* user_entry, const char* _path, struct stat* st) {
    const auto path = vfs_stdio_fix_path(_path);
    return lstat(path, st);
}

int vfs_stdio_isdir_open(void* user) {
    auto f = static_cast<FtpVfsDir*>(user);
    return f->fd != NULL;
}

int vfs_stdio_closedir(void* user) {
    auto f = static_cast<FtpVfsDir*>(user);
    int rc = 0;
    if (vfs_stdio_isdir_open(f)) {
        rc = closedir(f->fd);
        f->fd = NULL;
    }
    return rc;
}

int vfs_stdio_stat(const char* _path, struct stat* st) {
    const auto path = vfs_stdio_fix_path(_path);
    return stat(path, st);
}

int vfs_stdio_mkdir(const char* _path) {
    const auto path = vfs_stdio_fix_path(_path);
    return mkdir(path, 0777);
}

int vfs_stdio_unlink(const char* _path) {
    const auto path = vfs_stdio_fix_path(_path);
    return unlink(path);
}

int vfs_stdio_rmdir(const char* _path) {
    const auto path = vfs_stdio_fix_path(_path);
    return rmdir(path);
}

int vfs_stdio_rename(const char* _src, const char* _dst) {
    const auto src = vfs_stdio_fix_path(_src);
    const auto dst = vfs_stdio_fix_path(_dst);
    return rename(src, dst);
}

FtpVfs g_vfs_stdio = {
    .open = vfs_stdio_open,
    .read = vfs_stdio_read,
    .write = vfs_stdio_write,
    .seek = vfs_stdio_seek,
    .close = vfs_stdio_close,
    .isfile_open = vfs_stdio_isfile_open,
    .opendir = vfs_stdio_opendir,
    .readdir = vfs_stdio_readdir,
    .dirlstat = vfs_stdio_dirlstat,
    .closedir = vfs_stdio_closedir,
    .isdir_open = vfs_stdio_isdir_open,
    .stat = vfs_stdio_stat,
    .lstat = vfs_stdio_stat,
    .mkdir = vfs_stdio_mkdir,
    .unlink = vfs_stdio_unlink,
    .rmdir = vfs_stdio_rmdir,
    .rename = vfs_stdio_rename,
};

void loop(void* arg) {
    log_write("[FTP] loop entered\n");

    // load config.
    {
        SCOPED_MUTEX(&g_mutex);

        g_ftpsrv_config.log_callback = ftp_log_callback;
        g_ftpsrv_config.progress_callback = ftp_progress_callback;
        g_ftpsrv_config.anon = ini_getbool("Login", "anon", 0, INI_PATH);
        int user_len = ini_gets("Login", "user", "", g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
        int pass_len = ini_gets("Login", "pass", "", g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
        g_ftpsrv_config.port = ini_getl("Network", "port", 5000, INI_PATH); // 5000 to keep compat with older sphaira
        g_ftpsrv_config.timeout = ini_getl("Network", "timeout", 0, INI_PATH);
        g_ftpsrv_config.use_localtime = ini_getbool("Misc", "use_localtime", 0, INI_PATH);
        bool log_enabled = ini_getbool("Log", "log", 0, INI_PATH);

        // get nx config
        bool mount_devices = ini_getbool("Nx", "mount_devices", 1, INI_PATH);
        bool mount_bis = ini_getbool("Nx", "mount_bis", 0, INI_PATH);
        bool save_writable = ini_getbool("Nx", "save_writable", 0, INI_PATH);
        g_ftpsrv_config.port = ini_getl("Nx", "app_port", g_ftpsrv_config.port, INI_PATH); // compat

        // get Nx-App overrides
        g_ftpsrv_config.anon = ini_getbool("Nx-App", "anon", g_ftpsrv_config.anon, INI_PATH);
        user_len = ini_gets("Nx-App", "user", g_ftpsrv_config.user, g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
        pass_len = ini_gets("Nx-App", "pass", g_ftpsrv_config.pass, g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
        g_ftpsrv_config.port = ini_getl("Nx-App", "port", g_ftpsrv_config.port, INI_PATH);
        g_ftpsrv_config.timeout = ini_getl("Nx-App", "timeout", g_ftpsrv_config.timeout, INI_PATH);
        g_ftpsrv_config.use_localtime = ini_getbool("Nx-App", "use_localtime", g_ftpsrv_config.use_localtime, INI_PATH);
        log_enabled = ini_getbool("Nx-App", "log", log_enabled, INI_PATH);
        mount_devices = ini_getbool("Nx-App", "mount_devices", mount_devices, INI_PATH);
        mount_bis = ini_getbool("Nx-App", "mount_bis", mount_bis, INI_PATH);
        save_writable = ini_getbool("Nx-App", "save_writable", save_writable, INI_PATH);

        g_should_exit = false;
        mount_devices = true;
        g_ftpsrv_config.timeout = 0;

        if (!g_ftpsrv_config.port) {
            g_ftpsrv_config.port = 5000;
            log_write("[FTP] no port config, defaulting to 5000\n");
        }

        // keep compat with older sphaira
        if (!user_len && !pass_len) {
            g_ftpsrv_config.anon = true;
            log_write("[FTP] no user pass, defaulting to anon\n");
        }

        fsdev_wrapMountSdmc();

        static const VfsNxCustomPath custom_vfs[] = {
            {
                .name = "games",
                .user = NULL,
                .func = &g_vfs_stdio,
            },
            {
                .name = "install",
                .user = NULL,
                .func = &g_vfs_install,
            },
        };

        vfs_nx_init(custom_vfs, std::size(custom_vfs), mount_devices, save_writable, mount_bis, false);
    }

    ON_SCOPE_EXIT(
        vfs_nx_exit();
        fsdev_wrapUnmountAll();
    );

    while (!g_should_exit) {
        ftpsrv_init(&g_ftpsrv_config);
        while (!g_should_exit) {
            if (ftpsrv_loop(100) != FTP_API_LOOP_ERROR_OK) {
                svcSleepThread(1e+6);
                break;
            }
        }
        ftpsrv_exit();
    }

    log_write("[FTP] loop exitied\n");
}

} // namespace

bool Init() {
    SCOPED_MUTEX(&g_mutex);
    if (g_is_running) {
        log_write("[FTP] already enabled, cannot open\n");
        return false;
    }

    // if (R_FAILED(fsdev_wrapMountSdmc())) {
    //     log_write("[FTP] cannot mount sdmc\n");
    //     return false;
    // }

    // todo: replace everything with ini_browse for faster loading.
    // or load everything in the init thread.

    Result rc;
    if (R_FAILED(rc = utils::CreateThread(&g_thread, loop, nullptr, 1024*16))) {
        log_write("[FTP] failed to create nxlink thread: 0x%X\n", rc);
        return false;
    }

    if (R_FAILED(rc = threadStart(&g_thread))) {
        log_write("[FTP] failed to start nxlink thread: 0x%X\n", rc);
        threadClose(&g_thread);
        return false;
    }

    log_write("[FTP] started\n");
    return g_is_running = true;
}

void Exit() {
    SCOPED_MUTEX(&g_mutex);
    if (!g_is_running) {
        return;
    }

    g_is_running = false;
    g_should_exit = true;

    threadWaitForExit(&g_thread);
    threadClose(&g_thread);

    memset(&g_ftpsrv_config, 0, sizeof(g_ftpsrv_config));

    log_write("[FTP] exitied\n");
}

void ExitSignal() {
    SCOPED_MUTEX(&g_mutex);
    g_should_exit = true;
}

void InitInstallMode(const OnInstallStart& on_start, const OnInstallWrite& on_write, const OnInstallClose& on_close) {
    SCOPED_MUTEX(&g_shared_data.mutex);
    g_shared_data.on_start = on_start;
    g_shared_data.on_write = on_write;
    g_shared_data.on_close = on_close;
    g_shared_data.enabled = true;
}

void DisableInstallMode() {
    SCOPED_MUTEX(&g_shared_data.mutex);
    g_shared_data.enabled = false;
}

unsigned GetPort() {
    SCOPED_MUTEX(&g_mutex);
    return g_ftpsrv_config.port;
}

bool IsAnon() {
    SCOPED_MUTEX(&g_mutex);
    return g_ftpsrv_config.anon;
}

const char* GetUser() {
    SCOPED_MUTEX(&g_mutex);
    return g_ftpsrv_config.user;
}

const char* GetPass() {
    SCOPED_MUTEX(&g_mutex);
    return g_ftpsrv_config.pass;
}

} // namespace sphaira::ftpsrv

extern "C" {

void log_file_write(const char* msg) {
    log_write("%s", msg);
}

void log_file_fwrite(const char* fmt, ...) {
    va_list v{};
    va_start(v, fmt);
    log_write_arg(fmt, &v);
    va_end(v);
}

} // extern "C"
