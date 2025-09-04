#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"
#include "utils/devoptab_romfs.hpp"
#include "utils/utils.hpp"

#include "defines.hpp"
#include "log.hpp"

#include "yati/nx/es.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ncz.hpp"
#include "yati/nx/ncm.hpp"
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

struct NcaContentTypeFsName {
    const char* name;
    nca::FileSystemType fs_type;
};

constexpr const NcaContentTypeFsName CONTENT_TYPE_FS_NAMES[][NCA_SECTION_TOTAL] = {
    [nca::ContentType_Program] = {
        { "exeFS", nca::FileSystemType_PFS0 },
        { "RomFS", nca::FileSystemType_RomFS },
        { "Logo", nca::FileSystemType_PFS0 },
    },
    [nca::ContentType_Meta] = {
        { "Meta", nca::FileSystemType_PFS0 },
    },
    [nca::ContentType_Control] = {
        { "RomFS", nca::FileSystemType_RomFS },
    },
    [nca::ContentType_Manual] = {
        { "RomFS", nca::FileSystemType_RomFS },
    },
    [nca::ContentType_Data] = {
        { "RomFS", nca::FileSystemType_RomFS }, // verify
    },
    [nca::ContentType_PublicData] = {
        { "RomFS", nca::FileSystemType_RomFS },
    },
};

struct NamedCollection {
    std::string name; // exeFS, RomFS, Logo.
    u8 fs_type; // PFS0 or RomFS.
    yati::container::Collections pfs0_collections;
    romfs::RomfsCollection romfs_collections;
};

struct FileEntry {
    u8 fs_type; // PFS0 or RomFS.
    romfs::FileEntry romfs;
    const yati::container::CollectionEntry* pfs0;
    u64 offset;
    u64 size;
};

struct DirEntry {
    u8 fs_type; // PFS0 or RomFS.
    romfs::DirEntry romfs;
    const yati::container::Collections* pfs0;
};

struct Device {
    std::vector<NamedCollection> collections;
    std::unique_ptr<yati::source::Base> source;
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
            out.fs_type = e.fs_type;

            const auto rel_name = path.substr(e.name.length() + 1);

            if (out.fs_type == nca::FileSystemType_RomFS) {
                if (!romfs::find_file(e.romfs_collections, rel_name, out.romfs)) {
                    return false;
                }

                out.offset = out.romfs.offset;
                out.size = out.romfs.size;
                return true;
            } else if (out.fs_type == nca::FileSystemType_PFS0) {
                for (const auto& collection : e.pfs0_collections) {
                    if (rel_name == "/" + collection.name) {
                        out.pfs0 = &collection;
                        out.offset = out.pfs0->offset;
                        out.size = out.pfs0->size;
                        return true;
                    }
                }

                return false;
            } else {
                log_write("[NCAFS] invalid fs type in find file\n");
                return false;
            }
        }
    }

    return false;
}

bool find_dir(std::span<const NamedCollection> named, std::string_view path, DirEntry& out) {
    for (auto& e : named) {
        if (path.starts_with("/" + e.name)) {
            out.fs_type = e.fs_type;

            const auto rel_name = path.substr(e.name.length() + 1);

            if (out.fs_type == nca::FileSystemType_RomFS) {
                return romfs::find_dir(e.romfs_collections, rel_name, out.romfs);
            } else if (out.fs_type == nca::FileSystemType_PFS0) {
                if (rel_name.length()) {
                    return false;
                }

                out.pfs0 = &e.pfs0_collections;
                return true;
            } else {
                log_write("[NCAFS] invalid fs type in find file\n");
                return false;
            }
        }
    }

    return false;
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
        log_write("[NCAFS] failed to find file entry\n");
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
        if (entry.fs_type == nca::FileSystemType_RomFS) {
            romfs::dirreset(entry.romfs);
        } else {
            dir->index = 0;
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

        filestat->st_nlink = 1;
        filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        std::strcpy(filename, dir->device->collections[dir->index].name.c_str());
    } else {
        if (entry.fs_type == nca::FileSystemType_RomFS) {
            if (!romfs::dirnext(entry.romfs, filename, filestat)) {
                return set_errno(r, ENOENT);
            }
        } else {
            if (dir->index >= entry.pfs0->size()) {
                return set_errno(r, ENOENT);
            }

            const auto& collection = (*entry.pfs0)[dir->index];
            filestat->st_nlink = 1;
            filestat->st_size = collection.size;
            filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            std::strcpy(filename, collection.name.c_str());
        }
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

Result MountNcaInternal(fs::Fs* fs, const std::shared_ptr<yati::source::Base>& source, s64 size, const fs::FsPath& path, fs::FsPath& out_path) {
    // otherwise, find next free entry.
    auto itr = std::ranges::find_if(g_entries, [](auto& e){
        return !e;
    });
    R_UNLESS(itr != g_entries.end(), 0x1);
    const auto index = std::distance(g_entries.begin(), itr);

    // todo: rather than manually fetching tickets, use spl to
    // decrypt the nca for use (somehow, look how ams does it?).
    keys::Keys keys;
    R_TRY(keys::parse_keys(keys, true));

    nca::Header header;
    R_TRY(source->Read2(&header, 0, sizeof(header)));
    R_TRY(nca::DecryptHeader(&header, keys, header));

    std::unique_ptr<yati::source::Base> nca_reader;
    log_write("[NCA] got header, type: %s\n", nca::GetContentTypeStr(header.content_type));

    // check if this is a ncz.
    ncz::Header ncz_header{};
    R_TRY(source->Read2(&ncz_header, NCZ_NORMAL_SIZE, sizeof(ncz_header)));

    if (ncz_header.magic == NCZ_SECTION_MAGIC) {
        // read all the sections.
        s64 ncz_offset = NCZ_SECTION_OFFSET;
        ncz::Sections ncz_sections(ncz_header.total_sections);
        R_TRY(source->Read2(ncz_sections.data(), ncz_offset, ncz_sections.size() * sizeof(ncz::Section)));

        ncz_offset += ncz_sections.size() * sizeof(ncz::Section);
        ncz::BlockHeader ncz_block_header{};
        R_TRY(source->Read2(&ncz_block_header, ncz_offset, sizeof(ncz_block_header)));

        // ensure this is a block compressed nsz, otherwise bail out
        // because random access is not supported with solid compression.
        R_TRY(ncz_block_header.IsValid());

        ncz_offset += sizeof(ncz_block_header);
        ncz::Blocks ncz_blocks(ncz_block_header.total_blocks);
        R_TRY(source->Read2(ncz_blocks.data(), ncz_offset, ncz_blocks.size() * sizeof(ncz::Block)));

        ncz_offset += ncz_blocks.size() * sizeof(ncz::Block);
        nca_reader = std::make_unique<ncz::NczBlockReader>(
            ncz_header, ncz_sections, ncz_block_header, ncz_blocks, ncz_offset, source
        );
    } else {
        keys::KeyEntry title_key;
        R_TRY(nca::GetDecryptedTitleKey(fs, path, header, keys, title_key));

        // create nca reader which will handle decryption for us.
        // create a LRU buffer cache as the source in order to reduce small reads.
        nca_reader = std::make_unique<nca::NcaReader>(
            header, &title_key, size,
            std::make_shared<common::LruBufferedData>(source, size)
        );
    }

    std::vector<NamedCollection> collections;
    const auto& content_type_fs = CONTENT_TYPE_FS_NAMES[header.content_type];

    for (u32 i = 0; i < NCA_SECTION_TOTAL; i++) {
        const auto& fs_header = header.fs_header[i];
        const auto& fs_table = header.fs_table[i];

        const auto section_offset = NCA_MEDIA_REAL(fs_table.media_start_offset);
        const auto section_offset_end = NCA_MEDIA_REAL(fs_table.media_end_offset);
        const auto section_size = section_offset_end - section_offset;

        // check if we have hit eof.
        if (fs_header.version != 2 || !section_offset || !section_offset_end) {
            break;
        }

        // ensure the section_offset is valid.
        R_UNLESS(section_offset_end >= section_offset, 0x1);

        if (!content_type_fs[i].name) {
            log_write("[NCA] extra fs section found\n");
            R_THROW(0x1);
        }

        if (content_type_fs[i].fs_type != fs_header.fs_type) {
            log_write("[NCA] fs type missmatch! expected: %u got: %u\n", content_type_fs[i].fs_type, fs_header.fs_type);
            R_THROW(0x1);
        }

        if (fs_header.compression_info.table_offset || fs_header.compression_info.table_size) {
            log_write("[NCA] skipping compressed fs section\n");
            continue;
        }

        if (fs_header.encryption_type == nca::EncryptionType_AesCtrEx || fs_header.encryption_type == nca::EncryptionType_AesCtrExSkipLayerHash) {
            log_write("[NCA] skipping AesCtrEx encryption: %u\n", fs_header.encryption_type);
            continue;
        }

        NamedCollection collection;
        collection.name = content_type_fs[i].name;
        collection.fs_type = fs_header.fs_type;

        log_write("\t[NCA] section[%u] fs_type: %u\n", i, fs_header.fs_type);
        log_write("\t[NCA] section[%u] encryption_type: %u\n", i, fs_header.encryption_type);
        log_write("\t[NCA] section[%u] section_offset: %zu\n", i, section_offset);
        log_write("\t[NCA] section[%u] size: %zu\n", i, section_size);
        log_write("\n");

        if (fs_header.fs_type == nca::FileSystemType_PFS0) {
            const auto& hash_data = fs_header.hash_data.hierarchical_sha256_data;
            const auto off = section_offset + hash_data.pfs0_layer.offset;
            // const auto size = hash_data.pfs0_layer.size;

            log_write("[NCA] found pfs0, trying\n");
            yati::container::Nsp pfs0(nca_reader.get());

            R_TRY(pfs0.GetCollections(collection.pfs0_collections, off));
        } else if (fs_header.fs_type == nca::FileSystemType_RomFS) {
            const auto& hash_data = fs_header.hash_data.integrity_meta_info;
            R_UNLESS(hash_data.magic == 0x43465649, 0x1);
            R_UNLESS(hash_data.version == 0x20000, 0x2);
            R_UNLESS(hash_data.master_hash_size == SHA256_HASH_SIZE, 0x3);
            R_UNLESS(hash_data.info_level_hash.max_layers == 0x7, 0x4);

            if (fs_header.encryption_type == nca::EncryptionType_AesCtrEx || fs_header.encryption_type == nca::EncryptionType_AesCtrExSkipLayerHash) {
                R_UNLESS(fs_header.patch_info.indirect_header.magic == 0x52544B42, 0x5);
                R_UNLESS(fs_header.patch_info.aes_ctr_header.magic == 0x52544B42, 0x6);
                // todo: bktr
                continue;
            } else {
                auto& romfs = collection.romfs_collections;
                const auto offset = section_offset + hash_data.info_level_hash.levels[5].logical_offset;
                R_TRY(romfs::LoadRomfsCollection(nca_reader.get(), offset, romfs));
            }
        } else {
            log_write("[NCA] unsupported fs type: %u\n", fs_header.fs_type);
            R_THROW(0x1);
        }

        collections.emplace_back(std::move(collection));
    }

    R_UNLESS(!collections.empty(), 0x9);

    auto entry = std::make_unique<Entry>();
    entry->path = path;
    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    entry->device.source = std::move(nca_reader);
    entry->device.collections = std::move(collections);
    std::snprintf(entry->name, sizeof(entry->name), "nca_%zu", index);
    std::snprintf(entry->mount, sizeof(entry->mount), "nca_%zu:/", index);

    R_UNLESS(AddDevice(&entry->devoptab) >= 0, 0x1);
    log_write("[NCA] DEVICE SUCCESS %s %s\n", path.s, entry->name);

    out_path = entry->mount;
    entry->ref_count++;
    *itr = std::move(entry);

    R_SUCCEED();
}

} // namespace

Result MountNca(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path) {
    SCOPED_MUTEX(&g_mutex);

    if (IsAlreadyMounted(path, out_path)) {
        R_SUCCEED();
    }

    s64 size;
    auto source = std::make_shared<yati::source::File>(fs, path);
    R_TRY(source->GetSize(&size));

    return MountNcaInternal(fs, source, size, path, out_path);
}

Result MountNcaNcm(NcmContentStorage* cs, const NcmContentId* id, fs::FsPath& out_path) {
    SCOPED_MUTEX(&g_mutex);

    fs::FsPath path;
    const auto id_lower = std::byteswap(*(const u64*)id->c);
    const auto id_upper = std::byteswap(*(const u64*)(id->c + 0x8));
    std::snprintf(path, sizeof(path), "%016lx%016lx", id_lower, id_upper);

    if (IsAlreadyMounted(path, out_path)) {
        R_SUCCEED();
    }

    s64 size;
    auto source = std::make_shared<ncm::NcmSource>(cs, id);
    R_TRY(source->GetSize(&size));

    return MountNcaInternal(nullptr, source, size, path, out_path);
}

void UmountNca(const fs::FsPath& mount) {
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
