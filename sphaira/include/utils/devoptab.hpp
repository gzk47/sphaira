#pragma once

#include <switch.h>
#include "fs.hpp"

namespace sphaira::devoptab {

// mounts to "lower_case_hex_id:/"
Result MountFromSavePath(u64 id, fs::FsPath& out_path);
void UnmountSave(u64 id);

// todo:
Result MountZip(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountZip(const fs::FsPath& mount);

Result MountNsp(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountNsp(const fs::FsPath& mount);

Result MountXci(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountXci(const fs::FsPath& mount);

bool fix_path(const char* str, char* out);

} // namespace sphaira::devoptab
