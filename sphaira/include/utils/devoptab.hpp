#pragma once

#include "fs.hpp"
#include "yati/source/base.hpp"
#include "location.hpp"

#include <switch.h>
#include <memory>

namespace sphaira::devoptab {

// mounts to "lower_case_hex_id:/"
Result MountSaveSystem(u64 id, fs::FsPath& out_path);
void UnmountSave(u64 id);

Result MountZip(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountZip(const fs::FsPath& mount);

Result MountNsp(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountNsp(const fs::FsPath& mount);

Result MountXci(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
Result MountXciSource(const std::shared_ptr<sphaira::yati::source::Base>& source, s64 size, const fs::FsPath& path, fs::FsPath& out_path);
void UmountXci(const fs::FsPath& mount);

Result MountNca(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
Result MountNcaNcm(NcmContentStorage* cs, const NcmContentId* id, fs::FsPath& out_path);
void UmountNca(const fs::FsPath& mount);

Result MountBfsar(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountBfsar(const fs::FsPath& mount);

Result MountNro(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_path);
void UmountNro(const fs::FsPath& mount);

Result MountVfsAll();
Result MountWebdavAll();
Result MountHttpAll();
Result MountFtpAll();
Result MountNfsAll();
Result MountSmb2All();

Result GetNetworkDevices(location::StdioEntries& out);
void UmountAllNeworkDevices();

} // namespace sphaira::devoptab
