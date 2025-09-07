#include "location.hpp"
#include "fs.hpp"
#include "app.hpp"
#include "usbdvd.hpp"
#include "utils/devoptab.hpp"

#include <ff.h>
#include <cstring>
#include <minIni.h>
#include <usbhsfs.h>

namespace sphaira::location {
namespace {

constexpr fs::FsPath location_path{"/config/sphaira/locations.ini"};

} // namespace

void Add(const Entry& e) {
    if (e.name.empty() || e.url.empty()) {
        return;
    }

    ini_puts(e.name.c_str(), "url", e.url.c_str(), location_path);
    if (!e.user.empty()) {
        ini_puts(e.name.c_str(), "user", e.user.c_str(), location_path);
    }
    if (!e.pass.empty()) {
        ini_puts(e.name.c_str(), "pass", e.pass.c_str(), location_path);
    }
    if (!e.bearer.empty()) {
        ini_puts(e.name.c_str(), "bearer", e.bearer.c_str(), location_path);
    }
    if (!e.pub_key.empty()) {
        ini_puts(e.name.c_str(), "pub_key", e.pub_key.c_str(), location_path);
    }
    if (!e.priv_key.empty()) {
        ini_puts(e.name.c_str(), "priv_key", e.priv_key.c_str(), location_path);
    }
    if (e.port) {
        ini_putl(e.name.c_str(), "port", e.port, location_path);
    }
}

auto Load() -> Entries {
    Entries out{};

    auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<Entries*>(UserData);

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
        } else if (!std::strcmp(Key, "bearer")) {
            e->back().bearer = Value;
        } else if (!std::strcmp(Key, "pub_key")) {
            e->back().pub_key = Value;
        } else if (!std::strcmp(Key, "priv_key")) {
            e->back().priv_key = Value;
        } else if (!std::strcmp(Key, "port")) {
            e->back().port = std::atoi(Value);
        }

        return 1;
    };

    ini_browse(cb, &out, location_path);

    return out;
}

auto GetStdio(bool write) -> StdioEntries {
    StdioEntries out{};

    const auto add_from_entries = [](const StdioEntries& entries, StdioEntries& out, bool write) {
        for (const auto& e : entries) {
            if (write && (e.flags & FsEntryFlag::FsEntryFlag_ReadOnly)) {
                log_write("[STDIO] skipping read only mount: %s\n", e.name.c_str());
                continue;
            }

            out.emplace_back(e);
        }
    };

    {
        StdioEntries entries;
        if (R_SUCCEEDED(devoptab::GetNetworkDevices(entries))) {
            log_write("[LOCATION] got devoptab mounts: %zu\n", entries.size());
            add_from_entries(entries, out, write);
        }
    }

    // try and load usbdvd entry.
    // todo: check if more than 1 entry is supported.
    // todo: only call if usbdvd is init.
    if (!write) {
        StdioEntry entry;
        if (usbdvd::GetMountPoint(entry)) {
            out.emplace_back(entry);
        }
    }

    // bail out early if usbhdd is disabled.
    if (!App::GetHddEnable()) {
        log_write("[USBHSFS] not enabled\n");
        return out;
    }

    static UsbHsFsDevice devices[0x20];
    const auto count = usbHsFsListMountedDevices(devices, std::size(devices));
    log_write("[USBHSFS] got connected: %u\n", usbHsFsGetPhysicalDeviceCount());
    log_write("[USBHSFS] got count: %u\n", count);

    for (s32 i = 0; i < count; i++) {
        const auto& e = devices[i];

        if (write && (e.write_protect || (e.flags & UsbHsFsMountFlags_ReadOnly))) {
            log_write("[USBHSFS] skipping write protect\n");
            continue;
        }

        char display_name[0x100];
        std::snprintf(display_name, sizeof(display_name), "%s (%s - %s - %zu GB)", e.name, LIBUSBHSFS_FS_TYPE_STR(e.fs_type), e.product_name, e.capacity / 1024 / 1024 / 1024);

        u32 flags = 0;
        if (e.write_protect || (e.flags & UsbHsFsMountFlags_ReadOnly)) {
            flags |= FsEntryFlag::FsEntryFlag_ReadOnly;
        }

        out.emplace_back(e.name, display_name, flags);
        log_write("\t[USBHSFS] %s name: %s serial: %s man: %s\n", e.name, e.product_name, e.serial_number, e.manufacturer);
    }

    return out;
}

auto GetFat() -> StdioEntries {
    StdioEntries out{};

    for (auto& e : VolumeStr) {
        char path[64];
        std::snprintf(path, sizeof(path), "%s:/", e);

        char display_name[0x100];
        std::snprintf(display_name, sizeof(display_name), "%s (Read Only)", path);

        out.emplace_back(path, display_name, FsEntryFlag::FsEntryFlag_ReadOnly);
    }

    return out;
}

} // namespace sphaira::location
