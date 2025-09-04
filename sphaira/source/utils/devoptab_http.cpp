#include "utils/devoptab_common.hpp"
#include "utils/profile.hpp"

#include "location.hpp"
#include "log.hpp"
#include "defines.hpp"
#include <sys/iosupport.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <minIni.h>

#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <optional>
#include <sys/stat.h>

namespace sphaira::devoptab {
namespace {

constexpr int DEFAULT_HTTP_TIMEOUT = 3000; // 3 seconds.

#define CURL_EASY_SETOPT_LOG(handle, opt, v) \
    if (auto r = curl_easy_setopt(handle, opt, v); r != CURLE_OK) { \
        log_write("curl_easy_setopt(%s, %s) msg: %s\n", #opt, #v, curl_easy_strerror(r)); \
    } \

#define CURL_EASY_GETINFO_LOG(handle, opt, v) \
    if (auto r = curl_easy_getinfo(handle, opt, v); r != CURLE_OK) { \
        log_write("curl_easy_getinfo(%s, %s) msg: %s\n", #opt, #v, curl_easy_strerror(r)); \
    } \

struct HttpMountConfig {
    std::string name{};
    std::string url{};
    std::string user{};
    std::string pass{};
    std::optional<int> port{};
    int timeout{DEFAULT_HTTP_TIMEOUT};
};
using HttpMountConfigs = std::vector<HttpMountConfig>;

struct Device {
    CURL* curl{};
    HttpMountConfig config;
    Mutex mutex{};
    bool mounted{};
};

struct DirEntry {
    std::string name{};
    std::string href{};
    bool is_dir{};
};
using DirEntries = std::vector<DirEntry>;

struct FileEntry {
    std::string path{};
    struct stat st{};
};

struct File {
    Device* client;
    FileEntry* entry;
    size_t off;
};

struct Dir {
    Device* client;
    DirEntries* entries;
    size_t index;
};

size_t dirlist_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto data = static_cast<std::vector<char>*>(userdata);

    // increase by chunk size.
    const auto realsize = size * nmemb;
    if (data->capacity() < data->size() + realsize) {
        const auto rsize = std::max(realsize, data->size() + 1024 * 1024);
        data->reserve(rsize);
    }

    // store the data.
    const auto offset = data->size();
    data->resize(offset + realsize);
    std::memcpy(data->data() + offset, ptr, realsize);

    return realsize;
}

size_t write_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto data = static_cast<std::span<char>*>(userdata);

    const auto realsize = size * nmemb;
    if (data->size() < realsize) {
        log_write("[HTTP] buffer is too small: %zu < %zu\n", data->size(), realsize);
        return 0; // buffer is too small
    }

    std::memcpy(data->data(), ptr, realsize);
    *data = data->subspan(realsize); // advance the span

    return realsize;
}

std::string url_encode(const std::string& str) {
    auto escaped = curl_escape(str.c_str(), str.length());
    if (!escaped) {
        return str;
    }

    std::string result(escaped);
    curl_free(escaped);
    return result;
}

std::string build_url(const std::string& base, const std::string& path, bool is_dir) {
    std::string url = base;
    if (!url.ends_with('/')) {
        url += '/';
    }

    url += url_encode(path);
    if (is_dir && !url.ends_with('/')) {
        url += '/'; // append trailing slash for folder.
    }

    return url;
}

void http_set_common_options(Device& client, const std::string& url) {
    curl_easy_reset(client.curl);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_URL, url.c_str());
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_TIMEOUT, (long)client.config.timeout);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_CONNECTTIMEOUT, (long)client.config.timeout);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_AUTOREFERER, 1L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_SSL_VERIFYPEER, 0L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // disabled as i want to see the http core.
    // CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_FAILONERROR, 1L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_NOPROGRESS, 0L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_BUFFERSIZE, 1024L * 512L);

    if (client.config.port) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_PORT, (long)client.config.port.value());
    }

    // enable all forms of compression supported by libcurl.
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_ACCEPT_ENCODING, "");

    // in most cases, this will use CURLAUTH_BASIC.
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);

    // enable TE is server supports it.
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_TRANSFER_ENCODING, 1L);

    if (!client.config.user.empty()) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_USERNAME, client.config.user.c_str());
    }
    if (!client.config.pass.empty()) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_PASSWORD, client.config.pass.c_str());
    }
}

bool http_dirlist(Device& client, const std::string& path, DirEntries& out) {
    const auto url = build_url(client.config.url, path, true);
    std::vector<char> chunk;

    log_write("[HTTP] Listing URL: %s path: %s\n", url.c_str(), path.c_str());

    http_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEFUNCTION, dirlist_callback);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEDATA, (void *)&chunk);

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[HTTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    chunk.emplace_back('\0'); // null-terminate the chunk
    const char* dilim = "<a href=\"";
    const char* ptr = chunk.data();

    // try and parse the href links.
    // it works with python http.serve, npm http-server and rclone http server.
    while ((ptr = std::strstr(ptr, dilim))) {
        // skip the delimiter.
        ptr += std::strlen(dilim);

        const auto href_begin = ptr;
        const auto href_end = std::strstr(href_begin, "\">");
        if (!href_end) {
            continue;
        }
        const auto href_len = href_end - href_begin;

        const auto name_begin = href_end + std::strlen("\">");
        const auto name_end = std::strstr(name_begin, "</a>");
        if (!name_end) {
            continue;
        }
        const auto name_len = name_end - name_begin;

        if (href_len <= 0 || name_len <= 0) {
            continue;
        }

        // skip if inside <script> or <style> tags (simple check)
        const auto script_tag = std::strstr(href_begin - 32, "<script");
        const auto style_tag = std::strstr(href_begin - 32, "<style");
        if ((script_tag && script_tag < href_begin) || (style_tag && style_tag < href_begin)) {
            continue;
        }

        std::string href(href_begin, href_len);
        std::string name(name_begin, name_len);

        // skip parent directory entry and external links.
        if (href == ".." || name == ".." || href.starts_with("../") || name.starts_with("../") || href.find("://") != std::string::npos) {
            continue;
        }

        // skip links that are not actual files/dirs (e.g. sorting/filter controls)
        if (href.starts_with('?')) {
            continue;
        }

        if (name.empty() || href.empty() || name == "/" || href.starts_with('?') || href.starts_with('#')) {
            continue;
        }

        const auto is_dir = name.ends_with('/');
        if (is_dir) {
            name.pop_back(); // remove the trailing '/'
        }

        out.emplace_back(name, href, is_dir);
    }

    return true;
}

bool http_stat(Device& client, const std::string& path, struct stat* st, bool is_dir) {
    std::memset(st, 0, sizeof(*st));
    const auto url = build_url(client.config.url, path, is_dir);

    http_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_NOBODY, 1L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_FILETIME, 1L);

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[HTTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    long response_code = 0;
    CURL_EASY_GETINFO_LOG(client.curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_off_t file_size = 0;
    CURL_EASY_GETINFO_LOG(client.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &file_size);

    curl_off_t file_time = 0;
    CURL_EASY_GETINFO_LOG(client.curl, CURLINFO_FILETIME_T, &file_time);

    const char* content_type{};
    CURL_EASY_GETINFO_LOG(client.curl, CURLINFO_CONTENT_TYPE, &content_type);

    const char* effective_url{};
    CURL_EASY_GETINFO_LOG(client.curl, CURLINFO_EFFECTIVE_URL, &effective_url);

    // handle error codes.
    if (response_code != 200 && response_code != 206) {
        log_write("[HTTP] Unexpected HTTP response code: %ld\n", response_code);
        return false;
    }

    if (effective_url) {
        if (std::string_view{effective_url}.ends_with('/')) {
            is_dir = true;
        }
    }

    if (content_type && !std::strcmp(content_type, "text/html")) {
        is_dir = true;
    }

    if (is_dir) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_size = file_size > 0 ? file_size : 0;
    }

    st->st_mtime = file_time > 0 ? file_time : 0;
    st->st_atime = st->st_mtime;
    st->st_ctime = st->st_mtime;
    st->st_nlink = 1;

    return true;
}

bool http_read_file_chunk(Device& client, const std::string& path, size_t start, std::span<char> buffer) {
    SCOPED_TIMESTAMP("http_read_file_chunk");
    const auto url = build_url(client.config.url, path, false);

    char range[64];
    std::snprintf(range, sizeof(range), "%zu-%zu", start, start + buffer.size() - 1);
    log_write("[HTTP] Requesting range: %s\n", range);

    http_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_RANGE, range);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEFUNCTION, write_data_callback);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEDATA, (void *)&buffer);

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[HTTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    return true;
}

bool mount_http(Device& client) {
    if (client.curl) {
        return true;
    }

    client.curl = curl_easy_init();
    if (!client.curl) {
        log_write("[HTTP] curl_easy_init() failed\n");
        return false;
    }

    return true;
}

int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int devoptab_open(struct _reent *r, void *fileStruct, const char *_path, int flags, int mode) {
    auto device = (Device*)r->deviceData;
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));
    SCOPED_MUTEX(&device->mutex);

    // todo: add this check to all devoptabs.
    if ((flags & O_ACCMODE) != O_RDONLY) {
        log_write("[HTTP] Only read-only mode is supported\n");
        return set_errno(r, EINVAL);
    }

    char path[PATH_MAX]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_http(*device)) {
        return set_errno(r, EIO);
    }

    struct stat st;
    if (!http_stat(*device, path, &st, false)) {
        log_write("[HTTP] http_stat() failed for file: %s\n", path);
        return set_errno(r, ENOENT);
    }

    if (st.st_mode & S_IFDIR) {
        log_write("[HTTP] Attempted to open a directory as a file: %s\n", path);
        return set_errno(r, EISDIR);
    }

    file->client = device;
    file->entry = new FileEntry{path, st};
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->client->mutex);

    delete file->entry;
    std::memset(file, 0, sizeof(*file));
    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->client->mutex);
    len = std::min(len, file->entry->st.st_size - file->off);

    if (!len) {
        return 0;
    }

    if (!http_read_file_chunk(*file->client, file->entry->path, file->off, {ptr, len})) {
        return set_errno(r, EIO);
    }

    file->off += len;
    return len;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->client->mutex);

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = file->entry->st.st_size;
    }

    r->_errno = 0;
    return file->off = std::clamp<u64>(pos, 0, file->entry->st.st_size);
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->client->mutex);

    std::memcpy(st, &file->entry->st, sizeof(*st));
    return r->_errno = 0;
}

DIR_ITER* devoptab_diropen(struct _reent *r, DIR_ITER *dirState, const char *_path) {
    auto device = (Device*)r->deviceData;
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));
    SCOPED_MUTEX(&device->mutex);

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        set_errno(r, ENOENT);
        return NULL;
    }

    if (!mount_http(*device)) {
        set_errno(r, EIO);
        return NULL;
    }

    auto entries = new DirEntries();
    if (!http_dirlist(*device, path, *entries)) {
        delete entries;
        set_errno(r, ENOENT);
        return NULL;
    }

    dir->client = device;
    dir->entries = entries;
    r->_errno = 0;
    return dirState;
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_MUTEX(&dir->client->mutex);

    dir->index = 0;
    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(filestat, 0, sizeof(*filestat));
    SCOPED_MUTEX(&dir->client->mutex);

    if (dir->index >= dir->entries->size()) {
        return set_errno(r, ENOENT);
    }

    auto& entry = (*dir->entries)[dir->index];
    if (entry.is_dir) {
        filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    }

    filestat->st_nlink = 1;
    std::strcpy(filename, entry.name.c_str());

    dir->index++;
    return r->_errno = 0;
}

int devoptab_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_MUTEX(&dir->client->mutex);

    delete dir->entries;
    std::memset(dir, 0, sizeof(*dir));
    return r->_errno = 0;
}

int devoptab_lstat(struct _reent *r, const char *_path, struct stat *st) {
    auto device = (Device*)r->deviceData;
    SCOPED_MUTEX(&device->mutex);

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_http(*device)) {
        return set_errno(r, EIO);
    }

    if (!http_stat(*device, path, st, false) && !http_stat(*device, path, st, true)) {
        return set_errno(r, ENOENT);
    }

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
    fs::FsPath mount{};
    char name[32]{};
    s32 ref_count{};

    ~Entry() {
        if (device.curl) {
            curl_easy_cleanup(device.curl);
        }
        RemoveDevice(mount);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountHttpAll() {
    SCOPED_MUTEX(&g_mutex);

    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<HttpMountConfigs*>(UserData);
        if (!Section || !Key || !Value) {
            return 1;
        }

        // add new entry if use section changed.
        if (e->empty() || std::strcmp(Section, e->back().name.c_str())) {
            e->emplace_back(Section);
        }

        if (!std::strcmp(Key, "url")) {
            e->back().url = Value;
        } else if (!std::strcmp(Key, "user")) {
            e->back().user = Value;
        } else if (!std::strcmp(Key, "pass")) {
            e->back().pass = Value;
        } else if (!std::strcmp(Key, "port")) {
            // todo: idk what the default should be.
            e->back().port = ini_parse_getl(Value, 8000);
        } else if (!std::strcmp(Key, "timeout")) {
            e->back().timeout = ini_parse_getl(Value, DEFAULT_HTTP_TIMEOUT);
        } else {
            log_write("[HTTP] INI: Unknown key %s=%s\n", Key, Value);
        }

        return 1;
    };

    HttpMountConfigs configs;
    ini_browse(cb, &configs, "/config/sphaira/http.ini");
    log_write("[HTTP] Found %zu mount configs\n", configs.size());

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
            log_write("[HTTP] Already mounted %s, skipping\n", config.name.c_str());
            continue;
        }

        // otherwise, find next free entry.
        auto itr = std::ranges::find_if(g_entries, [](auto& e){
            return !e;
        });

        if (itr == g_entries.end()) {
            log_write("[HTTP] No free entries to mount %s\n", config.name.c_str());
            break;
        }

        auto entry = std::make_unique<Entry>();
        entry->devoptab = DEVOPTAB;
        entry->devoptab.name = entry->name;
        entry->devoptab.deviceData = &entry->device;
        entry->device.config = config;
        std::snprintf(entry->name, sizeof(entry->name), "[HTTP] %s", config.name.c_str());
        std::snprintf(entry->mount, sizeof(entry->mount), "[HTTP] %s:/", config.name.c_str());

        R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
        log_write("[HTTP] DEVICE SUCCESS %s %s\n", entry->device.config.url.c_str(), entry->name);

        entry->ref_count++;
        *itr = std::move(entry);
        log_write("[HTTP] Mounted %s at /%s\n", config.url.c_str(), config.name.c_str());
    }

    R_SUCCEED();
}

void UnmountHttpAll() {
    SCOPED_MUTEX(&g_mutex);

    for (auto& entry : g_entries) {
        if (entry) {
            entry.reset();
        }
    }
}

Result GetHttpMounts(location::StdioEntries& out) {
    SCOPED_MUTEX(&g_mutex);
    out.clear();

    for (const auto& entry : g_entries) {
        if (entry) {
            out.emplace_back(entry->mount, entry->name, true);
        }
    }

    R_SUCCEED();
}

} // namespace sphaira::devoptab
