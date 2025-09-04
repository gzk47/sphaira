#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "defines.hpp"
#include "log.hpp"

#include "yati/source/file.hpp"

#include <cstring>
#include <cerrno>
#include <array>
#include <memory>
#include <algorithm>
#include <sys/iosupport.h>
#include <zlib.h>

namespace sphaira::devoptab {
namespace {

#define LOCAL_HEADER_SIG 0x4034B50
#define FILE_HEADER_SIG 0x2014B50
#define DATA_DESCRIPTOR_SIG 0x8074B50
#define END_RECORD_SIG 0x6054B50

enum mmz_Flag {
    mmz_Flag_Encrypted = 1 << 0,
    mmz_Flag_DataDescriptor = 1 << 3,
    mmz_Flag_StrongEncrypted = 1 << 6,
};

enum mmz_Compression {
    mmz_Compression_None = 0,
    mmz_Compression_Deflate = 8,
};

// 30 bytes (0x1E)
#pragma pack(push,1)
typedef struct mmz_LocalHeader {
    uint32_t sig;
    uint16_t version;
    uint16_t flags;
    uint16_t compression;
    uint16_t modtime;
    uint16_t moddate;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extrafield_len;
} mmz_LocalHeader;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct mmz_DataDescriptor {
    uint32_t sig;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} mmz_DataDescriptor;
#pragma pack(pop)

// 46 bytes (0x2E)
#pragma pack(push,1)
typedef struct mmz_FileHeader {
    uint32_t sig;
    uint16_t version;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t modtime;
    uint16_t moddate;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extrafield_len;
    uint16_t filecomment_len;
    uint16_t disk_start; // wat
    uint16_t internal_attr; // wat
    uint32_t external_attr; // wat
    uint32_t local_hdr_off;
} mmz_FileHeader;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct mmz_EndRecord {
    uint32_t sig;
    uint16_t disk_number;
    uint16_t disk_wcd;
    uint16_t disk_entries;
    uint16_t total_entries;
    uint32_t central_directory_size;
    uint32_t file_hdr_off;
    uint16_t comment_len;
} mmz_EndRecord;
#pragma pack(pop)

struct FileEntry {
    std::string path;
    u16 flags;
    u16 compression_type;
    u16 modtime;
    u16 moddate;
    u32 compressed_size; // may be zero.
    u32 uncompressed_size; // may be zero.
    u32 local_file_header_off;
};

struct DirectoryEntry {
    std::string path;
    std::vector<DirectoryEntry> dir_child;
    std::vector<FileEntry> file_child;
};

using FileTableEntries = std::vector<FileEntry>;

struct Device {
    std::unique_ptr<common::LruBufferedData> source;
    DirectoryEntry root;
};

struct Zfile {
    z_stream z; // zlib stream.
    Bytef* buffer; // buffer that compressed data is read into.
    size_t buffer_size; // size of the above buffer.
    size_t compressed_off; // offset of the compressed file.
};

struct File {
    Device* device;
    const FileEntry* entry;
    Zfile zfile; // only used if the file is compressed.
    size_t data_off; // offset of the file data.
    size_t off;
};

struct Dir {
    Device* device;
    const DirectoryEntry* entry;
    u32 index;
};

const FileEntry* find_file_entry(const DirectoryEntry& dir, std::string_view path) {
    if (path.starts_with(dir.path)) {
        // todo: check if / comes after file name in order to only check dirs.
        for (const auto& e : dir.file_child) {
            if (e.path == path) {
                return &e;
            }
        }

        for (const auto& e : dir.dir_child) {
            if (auto entry = find_file_entry(e, path)) {
                return entry;
            }
        }
    }

    return nullptr;
}

const DirectoryEntry* find_dir_entry(const DirectoryEntry& dir, std::string_view path) {
    if (dir.path == path) {
        return &dir;
    }

    if (path.starts_with(dir.path)) {
        for (const auto& e : dir.dir_child) {
            if (auto entry = find_dir_entry(e, path)) {
                return entry;
            }
        }
    }

    return nullptr;
}

void set_stat_file(const FileEntry* entry, struct stat *st) {
    std::memset(st, 0, sizeof(*st));

    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_size = entry->uncompressed_size;
    st->st_nlink = 1;

    struct tm tm{};
    tm.tm_sec = (entry->modtime & 0x1F) << 1;
    tm.tm_min = (entry->modtime >> 5) & 0x3F;
    tm.tm_hour = (entry->modtime >> 11);
    tm.tm_mday = (entry->moddate & 0x1F);
    tm.tm_mon = ((entry->moddate >> 5) & 0xF) - 1;
    tm.tm_year = (entry->moddate >> 9) + 80;

    st->st_atime = mktime(&tm);
    st->st_mtime = st->st_atime;
    st->st_ctime = st->st_atime;
}

int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int devoptab_open(struct _reent *r, void *fileStruct, const char *_path, int flags, int mode) {
    auto device = (Device*)r->deviceData;
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));

    char path[PATH_MAX]{};
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    const auto entry = find_file_entry(device->root, path);
    if (!entry) {
        return set_errno(r, ENOENT);
    }

    if ((entry->flags & mmz_Flag_Encrypted) || (entry->flags & mmz_Flag_StrongEncrypted)) {
        log_write("[ZIP] encrypted zip not supported\n");
        return set_errno(r, ENOENT);
    }

    if (entry->compression_type != mmz_Compression_None && entry->compression_type != mmz_Compression_Deflate) {
        log_write("[ZIP] unsuported compression type: %u\n", entry->compression_type);
        return set_errno(r, ENOENT);
    }

    mmz_LocalHeader local_hdr{};
    auto offset = entry->local_file_header_off;
    if (R_FAILED(device->source->Read2(&local_hdr, offset, sizeof(local_hdr)))) {
        return set_errno(r, ENOENT);
    }

    if (local_hdr.sig != LOCAL_HEADER_SIG) {
        return set_errno(r, ENOENT);
    }

    offset += sizeof(local_hdr) + local_hdr.filename_len + local_hdr.extrafield_len;

    // todo: does a decs take prio over file header?
    if (local_hdr.flags & mmz_Flag_DataDescriptor) {
        mmz_DataDescriptor data_desc{};
        if (R_FAILED(device->source->Read2(&data_desc, offset, sizeof(data_desc)))) {
            return set_errno(r, ENOENT);
        }

        if (data_desc.sig != DATA_DESCRIPTOR_SIG) {
            return set_errno(r, ENOENT);
        }

        offset += sizeof(data_desc);
    }

    if (entry->compression_type == mmz_Compression_Deflate) {
        auto& zfile = file->zfile;
        zfile.buffer_size = 1024 * 64;
        zfile.buffer = (Bytef*)calloc(1, zfile.buffer_size);
        if (!zfile.buffer) {
            return set_errno(r, ENOENT);
        }

        // skip zlib header.
        if (Z_OK != inflateInit2(&zfile.z, -MAX_WBITS)) {
            free(zfile.buffer);
            zfile.buffer = nullptr;
            return set_errno(r, ENOENT);
        }
    }

    file->device = device;
    file->entry = entry;
    file->data_off = offset;
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);

    if (file->entry->compression_type == mmz_Compression_Deflate) {
        inflateEnd(&file->zfile.z);

        if (file->zfile.buffer) {
            free(file->zfile.buffer);
        }
    }

    std::memset(file, 0, sizeof(*file));
    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    len = std::min(len, file->entry->uncompressed_size - file->off);

    if (!len) {
        return 0;
    }

    if (file->entry->compression_type == mmz_Compression_None) {
        if (R_FAILED(file->device->source->Read2(ptr, file->data_off + file->off, len))) {
            return set_errno(r, ENOENT);
        }
    } else if (file->entry->compression_type == mmz_Compression_Deflate) {
        auto& zfile = file->zfile;
        zfile.z.next_out = (Bytef*)ptr;
        zfile.z.avail_out = len;

        // run until we have inflated enough data.
        while (zfile.z.avail_out) {
            // check if we need to fetch more data.
            if (!zfile.z.next_in || !zfile.z.avail_in) {
                const auto clen = std::min(zfile.buffer_size, file->entry->compressed_size - zfile.compressed_off);
                if (R_FAILED(file->device->source->Read2(zfile.buffer, file->data_off + zfile.compressed_off, clen))) {
                    return set_errno(r, ENOENT);
                }

                zfile.compressed_off += clen;
                zfile.z.next_in = zfile.buffer;
                zfile.z.avail_in = clen;
            }

            const auto rc = inflate(&zfile.z, Z_SYNC_FLUSH);
            if (Z_OK != rc) {
                if (Z_STREAM_END == rc) {
                    len -= zfile.z.avail_out;
                } else {
                    log_write("[ZLIB] failed to inflate: %d %s\n", rc, zfile.z.msg);
                    return set_errno(r, ENOENT);
                }
            }
        }
    }

    file->off += len;
    return len;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);

    // seek like normal.
    if (file->entry->compression_type == mmz_Compression_None) {
        if (dir == SEEK_CUR) {
            pos += file->off;
        } else if (dir == SEEK_END) {
            pos = file->entry->uncompressed_size;
        }
    } else if (file->entry->compression_type == mmz_Compression_Deflate) {
        // limited seek options.
        // todo: support seeking to end and then back to orig position.
        if (dir == SEEK_SET) {
            if (pos == 0) {
                inflateReset(&file->zfile.z);
            } else if (pos != file->off) {
                // seeking to the end is fine as it may be used to calc size.
                if (pos != file->entry->uncompressed_size) {
                    // random access seek is not supported.
                    pos = file->off;
                }
            }
        } else if (dir == SEEK_CUR) {
            // random access seek is not supported.
            pos = file->off;
        } else if (dir == SEEK_END) {
            pos = file->entry->uncompressed_size;
        }
    }

    r->_errno = 0;
    return file->off = std::clamp<u64>(pos, 0, file->entry->uncompressed_size);
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);

    std::memset(st, 0, sizeof(*st));
    set_stat_file(file->entry, st);
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

    const auto entry = find_dir_entry(device->root, path);
    if (!entry) {
        set_errno(r, ENOENT);
        return NULL;
    }

    dir->device = device;
    dir->entry = entry;
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

    u32 index = dir->index;
    if (index >= dir->entry->dir_child.size()) {
        index -= dir->entry->dir_child.size();
        if (index >= dir->entry->file_child.size()) {
            return set_errno(r, ENOENT);
        } else {
            const auto& entry = dir->entry->file_child[index];
            const auto rel_path = entry.path.substr(entry.path.find_last_of('/') + 1);

            set_stat_file(&entry, filestat);
            std::strcpy(filename, rel_path.c_str());
        }

    } else {
        const auto& entry = dir->entry->dir_child[index];
        const auto rel_path = entry.path.substr(entry.path.find_last_of('/') + 1);

        filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        std::strcpy(filename, rel_path.c_str());
    }

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

    if (!device) {
        return set_errno(r, ENOENT);
    }

    char path[PATH_MAX];
    if (!common::fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    std::memset(st, 0, sizeof(*st));
    st->st_nlink = 1;

    if (find_dir_entry(device->root, path)) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else if (auto entry = find_file_entry(device->root, path)) {
        set_stat_file(entry, st);
    } else {
        log_write("[ZIP] didn't find in lstat\n");
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

auto BuildPath(const std::string& path) -> std::string {
    if (path.starts_with('/')) {
        return path;
    }
    return "/" + path;
}

void Parse(const FileTableEntries& entries, u32& index, DirectoryEntry& out) {
    for (; index < entries.size(); index++) {
        const auto path = BuildPath(entries[index].path);

        // check if this path belongs to this dir.
        if (!path.starts_with(out.path)) {
            return;
        }

        if (path.ends_with('/')) {
            auto& new_entry = out.dir_child.emplace_back();
            new_entry.path = path.substr(0, path.length() - 1);

            u32 new_index = index + 1;
            Parse(entries, new_index, new_entry);
            index = new_index - 1;
        } else {
            // check if this file actually belongs to this folder.
            const auto idx = path.find_first_of('/', out.path.length() + 1);
            const auto sub = path.substr(0, idx);

            if (idx != path.npos && out.path != sub) {
                auto& new_entry = out.dir_child.emplace_back();
                new_entry.path = sub;
                Parse(entries, index, new_entry);
            } else {
                auto& new_entry = out.file_child.emplace_back(entries[index]);
                new_entry.path = path;
            }
        }
    }
}

void Parse(const FileTableEntries& entries, DirectoryEntry& out) {
    u32 index = 0;
    out.path = "/"; // add root folder.
    Parse(entries, index, out);
}

Result find_central_dir_offset(common::LruBufferedData* source, s64 size, mmz_EndRecord* record) {
    // check if the record is at the end (no extra header).
    auto offset = size - sizeof(*record);
    R_TRY(source->Read2(record, offset, sizeof(*record)));

    if (record->sig == END_RECORD_SIG) {
        R_SUCCEED();
    }

    // failed, find the sig by reading the last 64k and loop across it.
    const auto rsize = std::min<u64>(UINT16_MAX, size);
    offset = size - rsize;
    std::vector<u8> data(rsize);
    R_TRY(source->Read2(data.data(), offset, data.size()));

    // check in reverse order as it's more likely at the end.
    for (s64 i = data.size() - sizeof(*record); i >= 0; i--) {
        u32 sig;
        std::memcpy(&sig, data.data() + i, sizeof(sig));
        if (sig == END_RECORD_SIG) {
            std::memcpy(record, data.data() + i, sizeof(*record));
            R_SUCCEED();
        }
    }

    R_THROW(0x1);
}

Result ParseZip(common::LruBufferedData* source, s64 size, FileTableEntries& out) {
    mmz_EndRecord end_rec;
    R_TRY(find_central_dir_offset(source, size, &end_rec));

    out.reserve(end_rec.total_entries);
    auto file_header_off = end_rec.file_hdr_off;

    for (u16 i = 0; i < end_rec.total_entries; i++) {
        // read the file header.
        mmz_FileHeader file_hdr{};
        R_TRY(source->Read2(&file_hdr, file_header_off, sizeof(file_hdr)));

        if (file_hdr.sig != FILE_HEADER_SIG) {
            log_write("[ZIP] invalid file record\n");
            R_THROW(0x1);
        }

        // save all the data hat we care about.
        auto& new_entry = out.emplace_back();
        new_entry.flags = file_hdr.flags;
        new_entry.compression_type = file_hdr.compression;
        new_entry.modtime = file_hdr.modtime;
        new_entry.moddate = file_hdr.moddate;
        new_entry.compressed_size = file_hdr.compressed_size;
        new_entry.uncompressed_size = file_hdr.uncompressed_size;
        new_entry.local_file_header_off = file_hdr.local_hdr_off;

        // read the file name.
        const auto filename_off = file_header_off + sizeof(file_hdr);
        new_entry.path.resize(file_hdr.filename_len);
        R_TRY(source->Read2(new_entry.path.data(), filename_off, new_entry.path.size()));

        // advance the offset.
        file_header_off += sizeof(file_hdr) + file_hdr.filename_len + file_hdr.extrafield_len + file_hdr.filecomment_len;
    }

    R_SUCCEED();
}

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

Result MountZip(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path) {
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

    FileTableEntries table_entries;
    R_TRY(ParseZip(buffered.get(), size, table_entries));
    log_write("[ZIP] parsed zip\n");

    DirectoryEntry root;
    Parse(table_entries, root);

    auto entry = std::make_unique<Entry>();
    entry->path = path;
    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    entry->device.source = std::move(buffered);
    entry->device.root = root;
    std::snprintf(entry->name, sizeof(entry->name), "zip_%zu", index);
    std::snprintf(entry->mount, sizeof(entry->mount), "zip_%zu:/", index);

    R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
    log_write("[ZIP] DEVICE SUCCESS %s %s\n", path.s, entry->name);

    out_path = entry->mount;
    entry->ref_count++;
    *itr = std::move(entry);

    R_SUCCEED();
}

void UmountZip(const fs::FsPath& mount) {
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
