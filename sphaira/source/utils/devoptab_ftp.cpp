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
#include <ctime>
#include <ranges>
#include <sys/stat.h>

namespace sphaira::devoptab {
namespace {

constexpr long DEFAULT_FTP_PORT = 21;
constexpr long DEFAULT_FTP_TIMEOUT = 3000; // 3 seconds.

#define CURL_EASY_SETOPT_LOG(handle, opt, v) \
    if (auto r = curl_easy_setopt(handle, opt, v); r != CURLE_OK) { \
        log_write("curl_easy_setopt(%s, %s) msg: %s\n", #opt, #v, curl_easy_strerror(r)); \
    } \

#define CURL_EASY_GETINFO_LOG(handle, opt, v) \
    if (auto r = curl_easy_getinfo(handle, opt, v); r != CURLE_OK) { \
        log_write("curl_easy_getinfo(%s, %s) msg: %s\n", #opt, #v, curl_easy_strerror(r)); \
    } \

struct FtpMountConfig {
    std::string name{};
    std::string url{};
    std::string user{};
    std::string pass{};
    std::optional<long> port{};
    long timeout{DEFAULT_FTP_TIMEOUT};
    bool read_only{};
};
using FtpMountConfigs = std::vector<FtpMountConfig>;

struct Device {
    CURL* curl{};
    FtpMountConfig config{};
    Mutex mutex{};
    bool mounted{};
};

struct DirEntry {
    std::string name{};
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
    bool write_mode;
};

struct Dir {
    Device* client;
    DirEntries* entries;
    size_t index;
};

size_t write_memory_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
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
    const auto rsize = std::min(size * nmemb, data->size());

    std::memcpy(data->data(), ptr, rsize);
    *data = data->subspan(rsize);
    return rsize;
}

size_t read_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto data = static_cast<std::span<const char>*>(userdata);
    const auto rsize = std::min(size * nmemb, data->size());

    std::memcpy(ptr, data->data(), rsize);
    *data = data->subspan(rsize);
    return rsize;
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

void ftp_set_common_options(Device& client, const std::string& url) {
    curl_easy_reset(client.curl);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_URL, url.c_str());
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_TIMEOUT_MS, client.config.timeout);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_CONNECTTIMEOUT_MS, client.config.timeout);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_NOPROGRESS, 0L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_FTP_CREATE_MISSING_DIRS, CURLFTP_CREATE_DIR_NONE);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_FTP_FILEMETHOD, CURLFTPMETHOD_NOCWD);

    if (client.config.port) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_PORT, client.config.port.value());
    }

    if (!client.config.user.empty()) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_USERNAME, client.config.user.c_str());
    }
    if (!client.config.pass.empty()) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_PASSWORD, client.config.pass.c_str());
    }
}

bool ftp_parse_mlst_line(std::string_view line, struct stat* st, std::string* file_out, bool type_only) {
    // trim leading white space.
    while (line.size() > 0 && std::isspace(line[0])) {
        line = line.substr(1);
    }

    auto file_name_pos = line.rfind(';');
    if (file_name_pos == std::string_view::npos || file_name_pos + 1 >= line.size()) {
        return false;
    }

    // trim white space.
    while (file_name_pos + 1 < line.size() && std::isspace(line[file_name_pos + 1])) {
        file_name_pos++;
    }
    auto file_name = line.substr(file_name_pos + 1);

    auto facts = line.substr(0, file_name_pos);
    if (file_name.empty()) {
        return false;
    }

    bool found_type = false;
    while (!facts.empty()) {
        const auto sep = facts.find(';');
        if (sep == std::string_view::npos) {
            break;
        }

        const auto fact = facts.substr(0, sep);
        facts = facts.substr(sep + 1);

        const auto eq = fact.find('=');
        if (eq == std::string_view::npos || eq + 1 >= fact.size()) {
            continue;
        }

        const auto key = fact.substr(0, eq);
        const auto val = fact.substr(eq + 1);

        if (key == "type") {
            if (val == "file") {
                st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            } else if (val == "dir") {
                st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
            } else {
                log_write("[FTP] Unknown type fact value: %.*s\n", (int)val.size(), val.data());
                return false;
            }

            found_type = true;
        } else if (!type_only) {
            if (key == "size") {
                st->st_size = std::stoull(std::string(val));
            } else if (key == "modify") {
                if (val.size() >= 14) {
                    struct tm tm{};
                    tm.tm_year = std::stoi(std::string(val.substr(0, 4))) - 1900;
                    tm.tm_mon = std::stoi(std::string(val.substr(4, 2))) - 1;
                    tm.tm_mday = std::stoi(std::string(val.substr(6, 2)));
                    tm.tm_hour = std::stoi(std::string(val.substr(8, 2)));
                    tm.tm_min = std::stoi(std::string(val.substr(10, 2)));
                    tm.tm_sec = std::stoi(std::string(val.substr(12, 2)));
                    st->st_mtime = std::mktime(&tm);
                    st->st_atime = st->st_mtime;
                    st->st_ctime = st->st_mtime;
                }
            }
        }
    }

    if (!found_type) {
        log_write("[FTP] MLST line missing type fact\n");
        return false;
    }

    st->st_nlink = 1;
    if (file_out) {
        *file_out = std::string(file_name.data(), file_name.size());
    }

    return true;
}

/*
C> MLst file1
S> 250- Listing file1
S>  Type=file;Modify=19990929003355.237; file1
S> 250 End
*/
bool ftp_parse_mlist(std::string_view chunk, struct stat* st) {
    // sometimes the header data includes the full login exchange
    // so we need to find the actual start of the MLST response.
    const auto start_pos = chunk.find("250-");
    const auto end_pos = chunk.rfind("\n250");

    if (start_pos == std::string_view::npos || end_pos == std::string_view::npos) {
        log_write("[FTP] MLST response missing start or end\n");
        return false;
    }

    const auto end_line = chunk.find('\n', start_pos + 1);
    if (end_line == std::string_view::npos || end_line > end_pos) {
        log_write("[FTP] MLST response missing end line\n");
        return false;
    }

    chunk = chunk.substr(end_line + 1, end_pos - (end_line + 1));
    return ftp_parse_mlst_line(chunk, st, nullptr, false);
}

/*
C> MLSD tmp
S> 150 BINARY connection open for MLSD tmp
D> Type=cdir;Modify=19981107085215;Perm=el; tmp
D> Type=cdir;Modify=19981107085215;Perm=el; /tmp
D> Type=pdir;Modify=19990112030508;Perm=el; ..
D> Type=file;Size=25730;Modify=19940728095854;Perm=; capmux.tar.z
D> Type=file;Size=1024990;Modify=19980130010322;Perm=r; cap60.pl198.tar.gz
S> 226 MLSD completed
*/
void ftp_parse_mlsd(std::string_view chunk, DirEntries& out) {
    if (chunk.ends_with("\r\n")) {
        chunk = chunk.substr(0, chunk.size() - 2);
    } else if (chunk.ends_with('\n')) {
        chunk = chunk.substr(0, chunk.size() - 1);
    }

    for (const auto line : std::views::split(chunk, '\n')) {
        std::string_view line_str(line.data(), line.size());
        if (line_str.empty() || line_str == "\r") {
            continue;
        }

        DirEntry entry{};
        struct stat st{};
        if (!ftp_parse_mlst_line(line_str, &st, &entry.name, true)) {
            log_write("[FTP] Failed to parse MLSD line: %.*s\n", (int)line.size(), line.data());
            continue;
        }

        entry.is_dir = S_ISDIR(st.st_mode);
        out.emplace_back(entry);
    }
}

std::pair<bool, long> ftp_quote(Device& client, std::span<const std::string> commands, bool is_dir, std::vector<char>* response_data = nullptr) {
    const auto url = build_url(client.config.url, "/", is_dir);

    curl_slist* cmdlist{};
    ON_SCOPE_EXIT(curl_slist_free_all(cmdlist));

    for (const auto& cmd : commands) {
        cmdlist = curl_slist_append(cmdlist, cmd.c_str());
    }

    ftp_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_QUOTE, cmdlist);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_NOBODY, 1L);

    if (response_data) {
        response_data->clear();
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_HEADERFUNCTION, write_memory_callback);
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_HEADERDATA, (void *)response_data);
    }

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[FTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return {false, 0};
    }

    long response_code = 0;
    CURL_EASY_GETINFO_LOG(client.curl, CURLINFO_RESPONSE_CODE, &response_code);
    return {true, response_code};
}

bool ftp_dirlist(Device& client, const std::string& path, DirEntries& out) {
    const auto url = build_url(client.config.url, path, true);
    std::vector<char> chunk;

    ftp_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEDATA, (void *)&chunk);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_CUSTOMREQUEST, "MLSD");

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[FTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    ftp_parse_mlsd({chunk.data(), chunk.size()}, out);
    return true;
}

bool ftp_stat(Device& client, const std::string& path, struct stat* st, bool is_dir) {
    std::memset(st, 0, sizeof(*st));

    std::vector<char> chunk;
    const auto [success, response_code] = ftp_quote(client, {"MLST " + path}, is_dir, &chunk);
    if (!success) {
        return false;
    }

    if (!success || response_code >= 400) {
        log_write("[FTP] MLST command failed with response code: %ld\n", response_code);
        return false;
    }

    if (!ftp_parse_mlist({chunk.data(), chunk.size()}, st)) {
        log_write("[FTP] Failed to parse MLST response for path: %s\n", path.c_str());
        return false;
    }

    return true;
}

bool ftp_read_file_chunk(Device& client, const std::string& path, size_t start, std::span<char> buffer) {
    const auto url = build_url(client.config.url, path, false);

    char range[64]{};
    std::snprintf(range, sizeof(range), "%zu-%zu", start, start + buffer.size() - 1);
    log_write("[FTP] Requesting range: %s\n", range);

    ftp_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_RANGE, range);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEFUNCTION, write_data_callback);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_WRITEDATA, (void *)&buffer);

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[FTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    return true;
}

bool ftp_write_file_chunk(Device& client, const std::string& path, size_t start, std::span<const char> buffer) {
    // manually set offset as curl seems to not do anything if CURLOPT_RESUME_FROM_LARGE is used for ftp.
    // NOTE: RFC 3659 specifies that setting the offset to anything other than the end for STOR
    // is undefined behavior, so random access writes are disabled for now.
    #if 0
    if (start || !buffer.empty()) {
        const auto [success, response_code] = ftp_quote(client, {"REST " + std::to_string(start)}, false);
        if (!success || response_code != 350) {
            log_write("[FTP] REST command failed with response code: %ld\n", response_code);
            return false;
        }
    }
    #endif

    const auto url = build_url(client.config.url, path, false);

    log_write("[FTP] Writing %zu bytes at offset %zu to %s\n", buffer.size(), start, path.c_str());
    ftp_set_common_options(client, url);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_UPLOAD, 1L);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)buffer.size());
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_READFUNCTION, read_data_callback);
    CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_READDATA, (void *)&buffer);

    // set resume from if needed.
    if (start || !buffer.empty()) {
        CURL_EASY_SETOPT_LOG(client.curl, CURLOPT_APPEND, 1L);
    }

    const auto res = curl_easy_perform(client.curl);
    if (res != CURLE_OK) {
        log_write("[FTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    return true;
}

bool ftp_remove_file_folder(Device& client, const std::string& path, bool is_dir) {
    const auto cmd = (is_dir ? "RMD " : "DELE ") + path;
    const auto [success, response_code] = ftp_quote(client, {cmd}, is_dir);
    if (!success || response_code >= 400) {
        log_write("[FTP] MLST command failed with response code: %ld\n", response_code);
        return false;
    }

    return true;
}

bool ftp_unlink(Device& client, const std::string& path) {
    return ftp_remove_file_folder(client, path, false);
}

bool ftp_rename(Device& client, const std::string& old_path, const std::string& new_path, bool is_dir) {
    const auto url = build_url(client.config.url, "/", is_dir);

    std::vector<std::string> commands;
    commands.emplace_back("RNFR " + old_path);
    commands.emplace_back("RNTO " + new_path);

    const auto [success, response_code] = ftp_quote(client, commands, is_dir);
    if (!success || response_code >= 400) {
        log_write("[FTP] MLST command failed with response code: %ld\n", response_code);
        return false;
    }

    return true;
}

bool ftp_mkdir(Device& client, const std::string& path) {
    std::vector<char> chunk;
    const auto [success, response_code] = ftp_quote(client, {"MKD " + path}, true);
    if (!success) {
        return false;
    }

    // todo: handle result if directory already exists.
    if (response_code >= 400) {
        log_write("[FTP] MLST command failed with response code: %ld\n", response_code);
        return false;
    }

    return true;
}

bool ftp_rmdir(Device& client, const std::string& path) {
    return ftp_remove_file_folder(client, path, true);
}

bool mount_ftp(Device& client) {
    if (client.mounted) {
        return true;
    }

    if (!client.curl) {
        client.curl = curl_easy_init();
        if (!client.curl) {
            log_write("[FTP] curl_easy_init() failed\n");
            return false;
        }
    }

    // issue FEAT command to see if we support MLST/MLSD.
    std::vector<char> chunk;
    const auto [success, response_code] = ftp_quote(client, {"FEAT"}, true, &chunk);
    if (!success || response_code != 211) {
        log_write("[FTP] FEAT command failed with response code: %ld\n", response_code);
        return false;
    }

    std::string_view view(chunk.data(), chunk.size());

    // check for MLST/MLSD support.
    // NOTE: RFC 3659 states that servers must support MLSD if they support MLST.
    if (view.find("MLST") == std::string_view::npos) {
        log_write("[FTP] Server does not support MLST/MLSD commands\n");
        return false;
    }

    // if we support UTF8, enable it.
    if (view.find("UTF8") != std::string_view::npos) {
        // it doesn't matter if this fails tbh.
        // also, i am not sure if this persists between logins or not...
        ftp_quote(client, {"OPTS UTF8 ON"}, true);
    }

    client.mounted = true;
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

    if (device->config.read_only && (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))) {
        return set_errno(r, EROFS);
    }

    char path[PATH_MAX]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_ftp(*device)) {
        return set_errno(r, EIO);
    }

    // create an empty file.
    if (flags & (O_CREAT | O_TRUNC)) {
        std::span<const char> empty{};
        if (!ftp_write_file_chunk(*device, path, 0, empty)) {
            log_write("[FTP] Failed to create file: %s\n", path);
            return set_errno(r, EIO);
        }
    }

    // ensure the file exists and get its size.
    struct stat st{};
    if (!ftp_stat(*device, path, &st, false)) {
        log_write("[FTP] File not found: %s\n", path);
        return set_errno(r, ENOENT);
    }

    if (st.st_mode & S_IFDIR) {
        log_write("[FTP] Path is a directory, not a file: %s\n", path);
        return set_errno(r, EISDIR);
    }

    if (flags & O_APPEND) {
        file->off = st.st_size;
    } else {
        file->off = 0;
    }

    log_write("[FTP] Opened file: %s (size=%zu)\n", path, (size_t)st.st_size);
    file->client = device;
    file->entry = new FileEntry{path, st};
    file->write_mode = (flags & (O_WRONLY | O_RDWR));
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

    if (file->write_mode) {
        log_write("[FTP] Attempt to read from a write-only file\n");
        return set_errno(r, EBADF);
    }

    if (!len) {
        return 0;
    }

    if (!ftp_read_file_chunk(*file->client, file->entry->path, file->off, {ptr, len})) {
        log_write("[FTP] Failed to read file chunk: %s\n", file->entry->path.c_str());
        return set_errno(r, EIO);
    }

    file->off += len;
    return len;
}

ssize_t devoptab_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->client->mutex);

    if (!ftp_write_file_chunk(*file->client, file->entry->path, file->off, {ptr, len})) {
        log_write("[FTP] Failed to write file chunk: %s\n", file->entry->path.c_str());
        return set_errno(r, EIO);
    }

    file->off += len;
    file->entry->st.st_size = std::max<off_t>(file->entry->st.st_size, file->off);
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

    // for now, random access writes are disabled.
    if (file->write_mode && pos != file->off) {
        set_errno(r, ESPIPE);
        return file->off;
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

int devoptab_unlink(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[PATH_MAX]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_ftp(*device)) {
        return set_errno(r, EIO);
    }

    if (!ftp_unlink(*device, path)) {
        return set_errno(r, EIO);
    }

    return r->_errno = 0;
}

int devoptab_rename(struct _reent *r, const char *_oldName, const char *_newName) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char oldName[PATH_MAX]{};
    if (!common::fix_path(_oldName, oldName)) {
        return set_errno(r, ENOENT);
    }

    char newName[PATH_MAX]{};
    if (!common::fix_path(_newName, newName)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_ftp(*device)) {
        return set_errno(r, EIO);
    }

    if (!ftp_rename(*device, oldName, newName, false) && !ftp_rename(*device, oldName, newName, true)) {
        return set_errno(r, EIO);
    }

    return r->_errno = 0;
}

int devoptab_mkdir(struct _reent *r, const char *_path, int mode) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[PATH_MAX]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_ftp(*device)) {
        return set_errno(r, EIO);
    }

    if (!ftp_mkdir(*device, path)) {
        return set_errno(r, EIO);
    }

    return r->_errno = 0;
}

int devoptab_rmdir(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_MUTEX(&device->mutex);

    char path[PATH_MAX]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!mount_ftp(*device)) {
        return set_errno(r, EIO);
    }

    if (!ftp_rmdir(*device, path)) {
        return set_errno(r, EIO);
    }

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

    if (!mount_ftp(*device)) {
        set_errno(r, EIO);
        return NULL;
    }

    auto entries = new DirEntries();
    if (!ftp_dirlist(*device, path, *entries)) {
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

    if (!mount_ftp(*device)) {
        return set_errno(r, EIO);
    }

    if (!ftp_stat(*device, path, st, false) && !ftp_stat(*device, path, st, true)) {
        return set_errno(r, ENOENT);
    }

    return r->_errno = 0;
}

int devoptab_ftruncate(struct _reent *r, void *fd, off_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->client->mutex);

    if (!file->write_mode) {
        log_write("[FTP] Attempt to truncate a read-only file\n");
        return set_errno(r, EBADF);
    }

    file->entry->st.st_size = len;
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
    .ftruncate_r  = devoptab_ftruncate,
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
        if (device.curl) {
            curl_easy_cleanup(device.curl);
        }

        RemoveDevice(mount);
    }
};

Mutex g_mutex;
std::array<std::unique_ptr<Entry>, common::MAX_ENTRIES> g_entries;

} // namespace

Result MountFtpAll() {
    SCOPED_MUTEX(&g_mutex);

    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<FtpMountConfigs*>(UserData);
        if (!Section || !Key || !Value) return 1;

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
            e->back().port = ini_parse_getl(Value, DEFAULT_FTP_PORT);
        } else if (!std::strcmp(Key, "timeout")) {
            e->back().timeout = ini_parse_getl(Value, DEFAULT_FTP_TIMEOUT);
        } else if (!std::strcmp(Key, "read_only")) {
            e->back().read_only = ini_parse_getbool(Value, false);
        } else {
            log_write("[FTP] INI: Unknown key %s=%s\n", Key, Value);
        }

        return 1;
    };

    FtpMountConfigs configs;
    ini_browse(cb, &configs, "/config/sphaira/ftp.ini");
    log_write("[FTP] Found %zu mount configs\n", configs.size());

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
            log_write("[FTP] Already mounted %s, skipping\n", config.name.c_str());
            continue;
        }

        // otherwise, find next free entry.
        auto itr = std::ranges::find_if(g_entries, [](auto& e){
            return !e;
        });

        if (itr == g_entries.end()) {
            log_write("[FTP] No free entries to mount %s\n", config.name.c_str());
            break;
        }

        auto entry = std::make_unique<Entry>();
        entry->devoptab = DEVOPTAB;
        entry->devoptab.name = entry->name;
        entry->devoptab.deviceData = &entry->device;
        entry->device.config = config;
        std::snprintf(entry->name, sizeof(entry->name), "[FTP] %s", config.name.c_str());
        std::snprintf(entry->mount, sizeof(entry->mount), "[FTP] %s:/", config.name.c_str());
        common::update_devoptab_for_read_only(&entry->devoptab, config.read_only);

        R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
        log_write("[FTP] DEVICE SUCCESS %s %s\n", entry->device.config.url.c_str(), entry->name);

        entry->ref_count++;
        *itr = std::move(entry);
        log_write("[FTP] Mounted %s at /%s\n", config.url.c_str(), config.name.c_str());
    }

    R_SUCCEED();
}

void UnmountFtpAll() {
    SCOPED_MUTEX(&g_mutex);

    for (auto& entry : g_entries) {
        if (entry) {
            entry.reset();
        }
    }
}

Result GetFtpMounts(location::StdioEntries& out) {
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
