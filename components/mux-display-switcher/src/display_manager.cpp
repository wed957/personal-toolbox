#include "display_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace mux {
namespace {

constexpr bool kUseVirtualModeAware = false;
constexpr UINT32 kQueryFlags = QDC_ALL_PATHS;
constexpr UINT32 kActiveQueryFlags = QDC_ONLY_ACTIVE_PATHS;
constexpr UINT32 kApplyFlags =
    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
constexpr UINT32 kValidateFlags =
    SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;

std::wstring Normalize(std::wstring value) {
    while (!value.empty() &&
           (value.back() == L'\0' || std::iswspace(value.back()))) {
        value.pop_back();
    }
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }
    value.erase(0, first);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool EqualsOrdinalIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), static_cast<int>(left.size()), right.c_str(),
               static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring TrimDeviceString(const wchar_t* value, size_t capacity) {
    size_t length = 0;
    while (length < capacity && value[length] != L'\0') {
        ++length;
    }
    std::wstring result(value, length);
    while (!result.empty() && std::iswspace(result.back())) {
        result.pop_back();
    }
    return result;
}

std::wstring TransientTargetKey(const LUID& adapter_id, UINT32 target_id) {
    wchar_t buffer[64]{};
    const int length = std::swprintf(
        buffer, std::size(buffer), L"ccd:%08lX:%08lX:%u",
        static_cast<unsigned long>(adapter_id.HighPart),
        static_cast<unsigned long>(adapter_id.LowPart), target_id);
    return length > 0 ? std::wstring(buffer, static_cast<size_t>(length))
                      : std::wstring(L"ccd:unknown");
}

bool QuerySnapshot(UINT32 flags, DisplaySnapshot& snapshot, LONG& error_code) {
    std::array<DISPLAYCONFIG_PATH_INFO, 16> local_paths{};
    std::array<DISPLAYCONFIG_MODE_INFO, 32> local_modes{};
    UINT32 local_path_count = static_cast<UINT32>(local_paths.size());
    UINT32 local_mode_count = static_cast<UINT32>(local_modes.size());
    LONG result = QueryDisplayConfig(
        flags, &local_path_count, local_paths.data(), &local_mode_count,
        local_modes.data(), nullptr);
    if (result == ERROR_SUCCESS) {
        snapshot.paths.assign(
            local_paths.begin(), local_paths.begin() + local_path_count);
        snapshot.modes.assign(
            local_modes.begin(), local_modes.begin() + local_mode_count);
        error_code = ERROR_SUCCESS;
        return true;
    }
    if (result != ERROR_INSUFFICIENT_BUFFER) {
        error_code = result;
        return false;
    }

    for (int attempt = 0; attempt < 5; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) {
            error_code = result;
            return false;
        }

        snapshot.paths.resize(path_count);
        snapshot.modes.resize(mode_count);
        result = QueryDisplayConfig(
            flags, &path_count, snapshot.paths.data(), &mode_count,
            snapshot.modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != ERROR_SUCCESS) {
            error_code = result;
            return false;
        }

        snapshot.paths.resize(path_count);
        snapshot.modes.resize(mode_count);
        error_code = ERROR_SUCCESS;
        return true;
    }

    error_code = ERROR_INSUFFICIENT_BUFFER;
    return false;
}

bool GetTargetName(
    const DISPLAYCONFIG_PATH_TARGET_INFO& target,
    DISPLAYCONFIG_TARGET_DEVICE_NAME& name) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        name = {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = target.adapterId;
        name.header.id = target.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
            return true;
        }
    }
    return false;
}

std::wstring TargetIdentity(
    const DISPLAYCONFIG_PATH_TARGET_INFO& target,
    const DISPLAYCONFIG_TARGET_DEVICE_NAME& name) {
    std::wstring path = TrimDeviceString(
        name.monitorDevicePath, std::size(name.monitorDevicePath));
    return path.empty() ? TransientTargetKey(target.adapterId, target.id) : path;
}

std::wstring FriendlyFallback(const DISPLAYCONFIG_PATH_TARGET_INFO& target) {
    switch (target.outputTechnology) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
            return L"内置显示器";
        default:
            return L"未命名显示器";
    }
}

std::wstring SourceGdiName(const DISPLAYCONFIG_PATH_SOURCE_INFO& source) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = source.adapterId;
    name.header.id = source.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
        return {};
    }
    return TrimDeviceString(name.viewGdiDeviceName, std::size(name.viewGdiDeviceName));
}

void PopulateModeDetails(
    const DISPLAYCONFIG_PATH_INFO& path,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    DisplayTarget& target) {
    if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        return;
    }

    const bool virtual_mode =
        kUseVirtualModeAware &&
        (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0;
    const UINT32 source_index = virtual_mode
                                    ? path.sourceInfo.sourceModeInfoIdx
                                    : path.sourceInfo.modeInfoIdx;
    const bool source_index_valid =
        virtual_mode
            ? source_index != DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID
            : source_index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    if (source_index_valid &&
        source_index < modes.size() &&
        modes[source_index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const auto& source = modes[source_index].sourceMode;
        target.width = source.width;
        target.height = source.height;
    }

    const auto refresh = path.targetInfo.refreshRate;
    if (refresh.Denominator != 0) {
        target.refresh_hz =
            static_cast<double>(refresh.Numerator) / refresh.Denominator;
    }
}

void PopulatePreferredMode(
    const DISPLAYCONFIG_PATH_TARGET_INFO& target_info,
    DisplayTarget& target) {
    if (target.preferred_width != 0 && target.preferred_height != 0) {
        return;
    }

    DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};
    preferred.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
    preferred.header.size = sizeof(preferred);
    preferred.header.adapterId = target_info.adapterId;
    preferred.header.id = target_info.id;
    if (DisplayConfigGetDeviceInfo(&preferred.header) == ERROR_SUCCESS) {
        target.preferred_width = preferred.width;
        target.preferred_height = preferred.height;
    }
}

struct SourceKey {
    LUID adapter_id{};
    UINT32 source_id = 0;

    bool operator<(const SourceKey& other) const {
        return std::tie(adapter_id.HighPart, adapter_id.LowPart, source_id) <
               std::tie(
                   other.adapter_id.HighPart, other.adapter_id.LowPart,
                   other.source_id);
    }
};

std::set<std::wstring> NormalizedSet(
    const std::vector<std::wstring>& values) {
    std::set<std::wstring> result;
    for (const auto& value : values) {
        result.insert(Normalize(value));
    }
    return result;
}

bool SnapshotActiveKeys(
    const DisplaySnapshot& snapshot,
    std::vector<std::wstring>& keys,
    LONG& error_code,
    const std::vector<DisplayAlias>* aliases = nullptr) {
    keys.clear();
    for (const auto& path : snapshot.paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }
        const std::wstring connector = TransientTargetKey(
            path.targetInfo.adapterId, path.targetInfo.id);
        const DisplayAlias* matched_alias = nullptr;
        if (aliases != nullptr) {
            const auto found = std::find_if(
                aliases->begin(), aliases->end(),
                [&connector](const DisplayAlias& candidate) {
                    return EqualsOrdinalIgnoreCase(
                        candidate.connector, connector);
                });
            if (found != aliases->end()) {
                matched_alias = &*found;
            }
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        if (GetTargetName(path.targetInfo, name)) {
            keys.push_back(TargetIdentity(path.targetInfo, name));
            continue;
        }

        keys.push_back(
            matched_alias != nullptr ? matched_alias->identity : connector);
    }
    error_code = ERROR_SUCCESS;
    return true;
}

bool QueryActiveKeys(
    std::vector<std::wstring>& keys,
    LONG& error_code,
    const std::vector<DisplayAlias>* aliases = nullptr) {
    DisplaySnapshot active;
    if (!QuerySnapshot(kActiveQueryFlags, active, error_code)) {
        return false;
    }
    return SnapshotActiveKeys(active, keys, error_code, aliases);
}

bool VerifyActiveKeys(
    const std::vector<std::wstring>& expected_keys,
    LONG& error_code,
    const std::vector<DisplayAlias>* aliases = nullptr) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<std::wstring> active_keys;
        if (QueryActiveKeys(active_keys, error_code, aliases) &&
            NormalizedSet(active_keys) == NormalizedSet(expected_keys)) {
            return true;
        }
        if (attempt < 2) {
            Sleep(50);
        }
    }
    if (error_code == ERROR_SUCCESS) {
        error_code = ERROR_BAD_CONFIGURATION;
    }
    return false;
}

LONG ConfigureSnapshot(DisplaySnapshot& snapshot, UINT32 flags) {
    if (snapshot.paths.empty()) {
        return ERROR_INVALID_PARAMETER;
    }
    return SetDisplayConfig(
        static_cast<UINT32>(snapshot.paths.size()), snapshot.paths.data(),
        static_cast<UINT32>(snapshot.modes.size()),
        snapshot.modes.empty() ? nullptr : snapshot.modes.data(),
        flags);
}

LONG ApplySnapshot(DisplaySnapshot& snapshot) {
    return ConfigureSnapshot(snapshot, kApplyFlags);
}

LONG ValidateSnapshot(DisplaySnapshot& snapshot, bool virtual_mode_aware = false) {
    const UINT32 flags = virtual_mode_aware
                             ? kValidateFlags | SDC_VIRTUAL_MODE_AWARE
                             : kValidateFlags;
    return ConfigureSnapshot(snapshot, flags);
}

bool RemapModeIndex(
    UINT32& index,
    UINT32 invalid_index,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& source_modes,
    std::vector<DISPLAYCONFIG_MODE_INFO>& destination_modes,
    std::map<UINT32, UINT32>& remapped_indices) {
    if (index == invalid_index) {
        return true;
    }
    if (index >= source_modes.size()) {
        index = invalid_index;
        return true;
    }

    const auto existing = remapped_indices.find(index);
    if (existing != remapped_indices.end()) {
        index = existing->second;
        return true;
    }

    const UINT32 new_index = static_cast<UINT32>(destination_modes.size());
    destination_modes.push_back(source_modes[index]);
    remapped_indices.emplace(index, new_index);
    index = new_index;
    return true;
}

bool NormalizeSourcePositions(DisplaySnapshot& configuration) {
    bool has_anchor = false;
    bool has_origin = false;
    POINTL anchor{};

    for (const auto& path : configuration.paths) {
        const UINT32 mode_index = path.sourceInfo.modeInfoIdx;
        if (mode_index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
            mode_index >= configuration.modes.size()) {
            continue;
        }
        const auto& mode = configuration.modes[mode_index];
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            continue;
        }
        const POINTL position = mode.sourceMode.position;
        if (!has_anchor) {
            anchor = position;
            has_anchor = true;
        }
        if (position.x == 0 && position.y == 0) {
            has_origin = true;
            break;
        }
    }

    if (!has_anchor || has_origin) {
        return true;
    }

    for (auto& mode : configuration.modes) {
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            continue;
        }
        const LONGLONG translated_x =
            static_cast<LONGLONG>(mode.sourceMode.position.x) - anchor.x;
        const LONGLONG translated_y =
            static_cast<LONGLONG>(mode.sourceMode.position.y) - anchor.y;
        if (translated_x < std::numeric_limits<LONG>::min() ||
            translated_x > std::numeric_limits<LONG>::max() ||
            translated_y < std::numeric_limits<LONG>::min() ||
            translated_y > std::numeric_limits<LONG>::max()) {
            return false;
        }
        mode.sourceMode.position.x = static_cast<LONG>(translated_x);
        mode.sourceMode.position.y = static_cast<LONG>(translated_y);
    }
    return true;
}

bool BuildSelectedConfiguration(
    const DisplaySnapshot& all_paths,
    const std::set<size_t>& selected_path_indices,
    DisplaySnapshot& configuration) {
    configuration = {};
    configuration.paths.reserve(selected_path_indices.size());
    std::map<UINT32, UINT32> remapped_indices;

    for (size_t path_index = 0; path_index < all_paths.paths.size(); ++path_index) {
        if (!selected_path_indices.contains(path_index)) {
            continue;
        }

        DISPLAYCONFIG_PATH_INFO path = all_paths.paths[path_index];
        path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        if (path.targetInfo.rotation < DISPLAYCONFIG_ROTATION_IDENTITY ||
            path.targetInfo.rotation > DISPLAYCONFIG_ROTATION_ROTATE270) {
            path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
        }
        if ((path.targetInfo.scaling < DISPLAYCONFIG_SCALING_IDENTITY ||
             path.targetInfo.scaling >
                 DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX) &&
            path.targetInfo.scaling != DISPLAYCONFIG_SCALING_PREFERRED) {
            path.targetInfo.scaling = DISPLAYCONFIG_SCALING_PREFERRED;
        }
        const bool virtual_mode =
            kUseVirtualModeAware &&
            (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0;
        bool valid = true;
        if (virtual_mode) {
            UINT32 source_index = path.sourceInfo.sourceModeInfoIdx;
            valid = RemapModeIndex(
                source_index, DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID,
                all_paths.modes, configuration.modes, remapped_indices);
            path.sourceInfo.sourceModeInfoIdx = source_index;

            UINT32 target_index = path.targetInfo.targetModeInfoIdx;
            valid = valid && RemapModeIndex(
                                 target_index,
                                 DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID,
                                 all_paths.modes, configuration.modes,
                                 remapped_indices);
            path.targetInfo.targetModeInfoIdx = target_index;

            UINT32 desktop_index = path.targetInfo.desktopModeInfoIdx;
            valid = valid && RemapModeIndex(
                                 desktop_index,
                                 DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID,
                                 all_paths.modes, configuration.modes,
                                 remapped_indices);
            path.targetInfo.desktopModeInfoIdx = desktop_index;
        } else {
            UINT32 source_index = path.sourceInfo.modeInfoIdx;
            UINT32 target_index = path.targetInfo.modeInfoIdx;
            if (source_index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                target_index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
                path.sourceInfo.modeInfoIdx =
                    DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                path.targetInfo.modeInfoIdx =
                    DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            } else {
                valid = RemapModeIndex(
                    source_index, DISPLAYCONFIG_PATH_MODE_IDX_INVALID,
                    all_paths.modes, configuration.modes, remapped_indices);
                path.sourceInfo.modeInfoIdx = source_index;

                valid = valid && RemapModeIndex(
                                     target_index,
                                     DISPLAYCONFIG_PATH_MODE_IDX_INVALID,
                                     all_paths.modes, configuration.modes,
                                     remapped_indices);
                path.targetInfo.modeInfoIdx = target_index;
            }
        }
        if (!valid) {
            return false;
        }
        configuration.paths.push_back(path);
    }
    return configuration.paths.size() == selected_path_indices.size() &&
           NormalizeSourcePositions(configuration);
}

}  // namespace

std::wstring FormatSystemError(LONG error_code) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error_code),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);

    std::wstring result;
    if (length != 0 && message != nullptr) {
        result.assign(message, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n' ||
                std::iswspace(result.back()))) {
            result.pop_back();
        }
    } else {
        result = L"未知错误";
    }
    if (message != nullptr) {
        LocalFree(message);
    }

    wchar_t suffix[40]{};
    std::swprintf(suffix, std::size(suffix), L"（错误 %ld）", error_code);
    result += suffix;
    return result;
}

bool DisplayManager::Refresh(std::wstring& error) {
    LONG error_code = ERROR_SUCCESS;
    DisplaySnapshot snapshot;
    if (!QuerySnapshot(kQueryFlags, snapshot, error_code)) {
        error = L"无法读取 Windows 显示拓扑：" + FormatSystemError(error_code);
        return false;
    }

    std::vector<InternalTarget> internals;
    struct CachedTargetName {
        bool valid = false;
        DISPLAYCONFIG_TARGET_DEVICE_NAME value{};
        std::wstring identity;
    };
    std::map<std::wstring, CachedTargetName> names_by_connector;

    for (const auto& path : snapshot.paths) {
        if (!path.targetInfo.targetAvailable) {
            continue;
        }
        const std::wstring connector = Normalize(TransientTargetKey(
            path.targetInfo.adapterId, path.targetInfo.id));
        auto& cached = names_by_connector[connector];
        if (cached.valid || !GetTargetName(path.targetInfo, cached.value)) {
            continue;
        }
        cached.valid = true;
        cached.identity = TargetIdentity(path.targetInfo, cached.value);
    }

    std::map<std::wstring, size_t> index_by_identity;

    for (size_t path_index = 0; path_index < snapshot.paths.size(); ++path_index) {
        const auto& path = snapshot.paths[path_index];
        if (!path.targetInfo.targetAvailable) {
            continue;
        }

        const std::wstring connector =
            Normalize(TransientTargetKey(
                path.targetInfo.adapterId, path.targetInfo.id));
        const auto cached_name = names_by_connector.find(connector);
        const bool has_device_name =
            cached_name != names_by_connector.end() && cached_name->second.valid;
        const std::wstring identity =
            has_device_name
                ? cached_name->second.identity
                : TransientTargetKey(
                      path.targetInfo.adapterId, path.targetInfo.id);
        const std::wstring normalized_identity = Normalize(identity);

        const auto identity_match = index_by_identity.find(normalized_identity);
        size_t internal_index =
            identity_match == index_by_identity.end()
                ? SIZE_MAX
                : identity_match->second;

        if (internal_index == SIZE_MAX) {
            InternalTarget internal;
            internal.display.key = identity;
            if (has_device_name) {
                internal.display.name = TrimDeviceString(
                    cached_name->second.value.monitorFriendlyDeviceName,
                    std::size(
                        cached_name->second.value.monitorFriendlyDeviceName));
            }
            if (internal.display.name.empty()) {
                internal.display.name = FriendlyFallback(path.targetInfo);
            }
            internal.display.available = true;
            internal.adapter_id = path.targetInfo.adapterId;
            internal.target_id = path.targetInfo.id;
            internals.push_back(std::move(internal));
            internal_index = internals.size() - 1;
            index_by_identity.emplace(normalized_identity, internal_index);
        }

        auto& internal = internals[internal_index];
        internal.candidate_paths.push_back(path_index);
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
            internal.display.active = true;
            internal.display.gdi_name = SourceGdiName(path.sourceInfo);
            PopulateModeDetails(path, snapshot.modes, internal.display);
        }
        PopulatePreferredMode(path.targetInfo, internal.display);
    }

    std::stable_sort(
        internals.begin(), internals.end(), [](const auto& left, const auto& right) {
            const int name_order = CompareStringOrdinal(
                left.display.name.c_str(), -1, right.display.name.c_str(), -1,
                TRUE);
            if (name_order != CSTR_EQUAL) {
                return name_order == CSTR_LESS_THAN;
            }
            return CompareStringOrdinal(
                       left.display.key.c_str(), -1, right.display.key.c_str(), -1,
                       TRUE) == CSTR_LESS_THAN;
        });

    std::vector<DisplayTarget> public_targets;
    public_targets.reserve(internals.size());
    for (const auto& internal : internals) {
        public_targets.push_back(internal.display);
    }

    all_paths_ = std::move(snapshot);
    internal_targets_ = std::move(internals);
    targets_ = std::move(public_targets);
    error.clear();
    return true;
}

bool DisplayManager::CreateApplyRequest(
    const std::vector<std::wstring>& selected_keys,
    ApplyRequest& request,
    std::wstring& error) const {
    request = {};
    if (selected_keys.empty()) {
        error = L"至少要保留一台显示器。";
        return false;
    }

    const std::set<std::wstring> requested = NormalizedSet(selected_keys);
    std::vector<size_t> selected_targets;
    for (size_t index = 0; index < internal_targets_.size(); ++index) {
        const auto& target = internal_targets_[index];
        if (requested.contains(Normalize(target.display.key))) {
            selected_targets.push_back(index);
            request.expected_keys.push_back(target.display.key);
        }
    }

    if (selected_targets.size() != requested.size()) {
        error = L"显示器列表已变化，请刷新后重试。";
        return false;
    }

    for (const auto& target : internal_targets_) {
        for (const size_t path_index : target.candidate_paths) {
            const auto& target_info = all_paths_.paths[path_index].targetInfo;
            const std::wstring connector = TransientTargetKey(
                target_info.adapterId, target_info.id);
            const bool already_added = std::any_of(
                request.aliases.begin(), request.aliases.end(),
                [&connector](const DisplayAlias& alias) {
                    return EqualsOrdinalIgnoreCase(alias.connector, connector);
                });
            if (!already_added) {
                request.aliases.push_back(
                    {connector, target.display.key});
            }
        }
    }

    LONG error_code = ERROR_SUCCESS;
    if (!QuerySnapshot(kActiveQueryFlags, request.rollback, error_code)) {
        error = L"无法保存当前显示布局：" + FormatSystemError(error_code);
        return false;
    }

    std::vector<std::wstring> active_keys;
    if (!SnapshotActiveKeys(
            request.rollback, active_keys, error_code, &request.aliases)) {
        error = L"无法验证当前显示布局：" + FormatSystemError(error_code);
        return false;
    }
    if (NormalizedSet(active_keys) == NormalizedSet(request.expected_keys)) {
        request.no_change = true;
        error.clear();
        return true;
    }

    std::stable_sort(
        selected_targets.begin(), selected_targets.end(),
        [this](size_t left, size_t right) {
            return internal_targets_[left].candidate_paths.size() <
                   internal_targets_[right].candidate_paths.size();
        });

    std::vector<size_t> matched_path(internal_targets_.size(), SIZE_MAX);
    std::set<SourceKey> used_sources;
    std::function<bool(size_t)> match = [&](size_t position) {
        if (position == selected_targets.size()) {
            return true;
        }

        const size_t target_index = selected_targets[position];
        std::vector<size_t> candidates =
            internal_targets_[target_index].candidate_paths;
        std::stable_sort(
            candidates.begin(), candidates.end(), [this](size_t left, size_t right) {
                const bool left_active =
                    (all_paths_.paths[left].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
                const bool right_active =
                    (all_paths_.paths[right].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
                return left_active > right_active;
            });

        for (const size_t path_index : candidates) {
            const auto& source = all_paths_.paths[path_index].sourceInfo;
            SourceKey key{source.adapterId, source.id};
            if (used_sources.contains(key)) {
                continue;
            }
            used_sources.insert(key);
            matched_path[target_index] = path_index;
            if (match(position + 1)) {
                return true;
            }
            matched_path[target_index] = SIZE_MAX;
            used_sources.erase(key);
        }
        return false;
    };

    if (!match(0)) {
        error = L"显卡驱动没有提供可用的扩展模式路径，无法同时启用所选显示器。";
        return false;
    }

    std::set<size_t> selected_path_indices;
    for (const size_t target_index : selected_targets) {
        selected_path_indices.insert(matched_path[target_index]);
    }
    if (!BuildSelectedConfiguration(
            all_paths_, selected_path_indices, request.configuration)) {
        error = L"显示模式索引无效，请刷新显示器列表后重试。";
        return false;
    }

    error.clear();
    return true;
}

bool DisplayManager::CreateOnlyByNameRequest(
    const std::wstring& exact_name,
    ApplyRequest& request,
    std::wstring& error) const {
    std::vector<std::wstring> matches;
    for (const auto& target : targets_) {
        if (target.available && EqualsOrdinalIgnoreCase(target.name, exact_name)) {
            matches.push_back(target.key);
        }
    }

    if (matches.empty()) {
        error = L"未检测到名为 “" + exact_name + L"” 的可用显示器。";
        return false;
    }
    if (matches.size() > 1) {
        error = L"检测到多台同名显示器，请在主界面手动选择具体设备。";
        return false;
    }
    return CreateApplyRequest(matches, request, error);
}

OperationResult DisplayManager::Execute(ApplyRequest request) {
    if (request.no_change) {
        return {true, true, false, ERROR_SUCCESS, L"显示布局已经符合选择。"};
    }

    std::vector<std::wstring> rollback_keys;
    LONG rollback_snapshot_error = ERROR_SUCCESS;
    if (!SnapshotActiveKeys(
            request.rollback, rollback_keys, rollback_snapshot_error,
            &request.aliases)) {
        return {
            false, false, false, rollback_snapshot_error,
            L"无法读取恢复快照，未执行切换：" +
                FormatSystemError(rollback_snapshot_error)};
    }
    const LONG apply_result = ApplySnapshot(request.configuration);
    if (apply_result == ERROR_SUCCESS) {
        return {true, false, false, ERROR_SUCCESS, L"显示布局已应用。"};
    }

    // Some drivers return an error after committing the topology. Only the
    // exceptional path pays for verification and recovery.
    LONG verify_error = ERROR_SUCCESS;
    if (VerifyActiveKeys(
            request.expected_keys, verify_error, &request.aliases)) {
        return {
            true, false, false, ERROR_SUCCESS,
            L"显示布局已应用；显卡驱动同时返回了警告 " +
                std::to_wstring(apply_result) + L"。"};
    }

    const LONG rollback_error = ApplySnapshot(request.rollback);
    LONG rollback_verify_error = ERROR_SUCCESS;
    if (VerifyActiveKeys(
            rollback_keys, rollback_verify_error, &request.aliases)) {
        if (apply_result != ERROR_SUCCESS) {
            return {
                false, false, true, apply_result,
                L"显卡驱动未能完成切换，已恢复原显示布局：" +
                    FormatSystemError(apply_result)};
        }
        return {
            false, false, true,
            verify_error == ERROR_SUCCESS ? ERROR_BAD_CONFIGURATION : verify_error,
            L"驱动返回的布局与选择不一致，已恢复原显示布局。"};
    }

    std::wstring recovery_detail;
    if (rollback_error != ERROR_SUCCESS) {
        recovery_detail = FormatSystemError(rollback_error);
    } else {
        recovery_detail =
            L"Windows 接受了恢复请求，但实际活动显示器与原布局不一致。";
    }
    OperationResult failure{
        false, false, false,
        apply_result != ERROR_SUCCESS
            ? apply_result
            : (rollback_verify_error == ERROR_SUCCESS
                   ? ERROR_BAD_CONFIGURATION
                   : rollback_verify_error),
        L"布局验证失败，且自动恢复也失败：" +
            recovery_detail};
    failure.recovery_needed = true;
    return failure;
}

OperationResult DisplayManager::Validate(
    ApplyRequest request, bool virtual_mode_aware) {
    if (request.no_change) {
        return {true, true, false, ERROR_SUCCESS, L"显示布局已经符合选择。"};
    }
    if (request.configuration.paths.empty()) {
        return {
            false, false, false, ERROR_INVALID_PARAMETER,
            L"没有可验证的显示路径。"};
    }

    const LONG result =
        ValidateSnapshot(request.configuration, virtual_mode_aware);
    if (result == ERROR_SUCCESS) {
        return {true, false, false, ERROR_SUCCESS, L"显示布局验证通过。"};
    }
    return {
        false, false, false, result,
        L"显示布局验证失败：" + FormatSystemError(result)};
}

OperationResult DisplayManager::Restore(DisplaySnapshot snapshot) {
    std::vector<std::wstring> expected_keys;
    LONG snapshot_error = ERROR_SUCCESS;
    if (!SnapshotActiveKeys(snapshot, expected_keys, snapshot_error)) {
        return {
            false, false, false, snapshot_error,
            L"无法读取上一次显示布局：" + FormatSystemError(snapshot_error)};
    }

    const LONG result = ApplySnapshot(snapshot);
    if (result == ERROR_SUCCESS) {
        return {true, false, false, ERROR_SUCCESS, L"已恢复上一次显示布局。"};
    }

    LONG verify_error = ERROR_SUCCESS;
    if (VerifyActiveKeys(expected_keys, verify_error)) {
        return {
            true, false, false, ERROR_SUCCESS,
            L"已恢复上一次显示布局；显卡驱动同时返回了警告 " +
                std::to_wstring(result) + L"。"};
    }
    return {
        false, false, false,
        result != ERROR_SUCCESS ? result : verify_error,
        L"无法恢复上一次显示布局：" +
            FormatSystemError(
                result != ERROR_SUCCESS ? result : verify_error)};
}

OperationResult DisplayManager::RestoreExtended() {
    const LONG result = SetDisplayConfig(
        0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    if (result == ERROR_SUCCESS) {
        return {true, false, false, ERROR_SUCCESS, L"已恢复 Windows 扩展布局。"};
    }
    return {
        false, false, false, result,
        L"无法恢复扩展布局：" + FormatSystemError(result)};
}

}  // namespace mux
