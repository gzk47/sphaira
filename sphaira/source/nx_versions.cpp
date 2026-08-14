#include "nx_versions.hpp"

#include "download.hpp"
#include "option.hpp"
#include "ui/progress_box.hpp"
#include "yati/nx/ncm.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <span>
#include <string_view>

namespace sphaira::nx_versions {
namespace {

constexpr const char* DEFAULT_VERSIONS_URL = "https://raw.githubusercontent.com/16BitWonder/nx-versions/master/versions.txt";
constexpr fs::FsPath CACHE_PATH{"/switch/sphaira/cache/nx_versions.txt"};
constexpr s64 CACHE_MAX_AGE = 7 * 24 * 60 * 60;

struct CatalogEntry {
    u64 app_id{};
    u64 content_id{};
    u32 version{};
    u8 meta_type{};
};

option::OptionString g_versions_url{"nx_versions", "url", DEFAULT_VERSIONS_URL};
std::vector<CatalogEntry> g_catalog;
bool g_catalog_loaded{};
bool g_update_prompted{};

auto Parse(std::span<const u8> data) -> std::vector<CatalogEntry> {
    std::vector<CatalogEntry> catalog;
    if (data.empty()) {
        return catalog;
    }

    const std::string_view text{reinterpret_cast<const char*>(data.data()), data.size()};

    const auto header_end = text.find('\n');
    if (header_end == std::string_view::npos) {
        return catalog;
    }

    auto header = text.substr(0, header_end);
    if (!header.empty() && header.back() == '\r') {
        header.remove_suffix(1);
    }
    if (header != "id|version") {
        return catalog;
    }

    catalog.reserve(data.size() / 24);
    auto offset = header_end + 1;
    while (offset < text.size()) {
        const auto line_end = text.find('\n', offset);
        auto line = text.substr(offset, line_end == std::string_view::npos ? text.size() - offset : line_end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        const auto separator = line.find('|');
        if (separator == 16 && separator + 1 < line.size()) {
            u64 content_id{};
            u32 version{};
            const auto id_result = std::from_chars(line.data(), line.data() + separator, content_id, 16);
            const auto version_result = std::from_chars(line.data() + separator + 1, line.data() + line.size(), version, 10);

            if (id_result.ec == std::errc{} && id_result.ptr == line.data() + separator &&
                version_result.ec == std::errc{} && version_result.ptr == line.data() + line.size()) {
                const auto suffix = content_id & 0xFFF;
                if (suffix) {
                    const u8 meta_type = suffix == 0x800
                        ? NcmContentMetaType_Patch
                        : NcmContentMetaType_AddOnContent;
                    catalog.push_back({ncm::GetAppId(meta_type, content_id), content_id, version, meta_type});
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }

    std::sort(catalog.begin(), catalog.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.app_id != rhs.app_id
            ? lhs.app_id < rhs.app_id
            : lhs.content_id < rhs.content_id;
    });
    return catalog;
}

void LoadCatalog() {
    if (g_catalog_loaded) {
        return;
    }

    g_catalog_loaded = true;
    std::vector<u8> data;
    if (R_SUCCEEDED(fs::FsNativeSd().read_entire_file(CACHE_PATH, data))) {
        g_catalog = Parse(data);
    }
}

auto IsCacheStale() -> bool {
    FsTimeStampRaw timestamp{};
    if (R_FAILED(fs::GetFileTimeStampRaw(CACHE_PATH, &timestamp)) || !timestamp.is_valid) {
        return true;
    }

    const auto now = std::time(nullptr);
    return now < timestamp.modified || now - timestamp.modified >= CACHE_MAX_AGE;
}

}

auto GetAvailable(u64 app_id, const InstalledVersions& installed) -> std::vector<AvailableEntry> {
    LoadCatalog();

    std::vector<AvailableEntry> available;
    auto entry = std::lower_bound(g_catalog.begin(), g_catalog.end(), app_id, [](const auto& lhs, u64 rhs) {
        return lhs.app_id < rhs;
    });

    for (; entry != g_catalog.end() && entry->app_id == app_id; ++entry) {
        const auto installed_entry = installed.find(entry->content_id);
        if (installed_entry == installed.end() || installed_entry->second < entry->version) {
            available.push_back({
                entry->content_id,
                entry->version,
                entry->meta_type,
                installed_entry != installed.end(),
            });
        }
    }

    return available;
}

auto ShouldPromptForUpdate() -> bool {
    if (g_update_prompted) {
        return false;
    }

    g_update_prompted = true;
    LoadCatalog();
    return g_catalog.empty() || IsCacheStale();
}

auto Download(ui::ProgressBox* pbox) -> Result {
    pbox->NewTransfer("versions.txt");
    const auto result = curl::Api().ToMemory(
        curl::Url{g_versions_url.Get()},
        curl::OnProgress{pbox->OnDownloadProgressCallback()}
    );

    R_TRY(pbox->ShouldExitResult());
    R_UNLESS(result.success, Result_NxVersionsFailedToDownload);

    auto catalog = Parse(result.data);
    R_UNLESS(!catalog.empty(), Result_NxVersionsInvalidDatabase);

    fs::FsNativeSd sd;
    R_TRY(sd.CreateDirectoryRecursivelyWithPath(CACHE_PATH));
    R_TRY(sd.write_entire_file(CACHE_PATH, result.data));
    R_TRY(sd.Commit());

    g_catalog = std::move(catalog);
    g_catalog_loaded = true;
    R_SUCCEED();
}

}
