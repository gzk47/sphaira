#pragma once

#include <switch.h>
#include "fs.hpp"

namespace sphaira::devoptab {

// mounts to "lower_case_hex_id:/"
Result MountFromSavePath(u64 id, fs::FsPath& out_path);
void UnmountSave(u64 id);

// todo:
void MountZip(fs::Fs* fs, const fs::FsPath& mount, fs::FsPath& out_path);
void UmountZip(const fs::FsPath& mount);

// todo:
void MountNsp(fs::Fs* fs, const fs::FsPath& mount, fs::FsPath& out_path);
void UmountNsp(const fs::FsPath& mount);

// todo:
void MountXci(fs::Fs* fs, const fs::FsPath& mount, fs::FsPath& out_path);
void UmountXci(const fs::FsPath& mount);

} // namespace sphaira::devoptab
