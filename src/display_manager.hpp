#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

namespace mux {

struct DisplaySnapshot {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

struct DisplayTarget {
    std::wstring key;
    std::wstring name;
    std::wstring gdi_name;
    unsigned width = 0;
    unsigned height = 0;
    double refresh_hz = 0.0;
    bool active = false;
    bool available = false;
};

struct ApplyRequest {
    DisplaySnapshot configuration;
    DisplaySnapshot rollback;
    std::vector<std::wstring> expected_keys;
    bool no_change = false;
};

struct OperationResult {
    bool success = false;
    bool no_change = false;
    bool rolled_back = false;
    LONG error_code = ERROR_SUCCESS;
    std::wstring message;
    bool recovery_needed = false;
};

class DisplayManager {
public:
    bool Refresh(std::wstring& error);

    const std::vector<DisplayTarget>& targets() const noexcept {
        return targets_;
    }

    bool CreateApplyRequest(
        const std::vector<std::wstring>& selected_keys,
        ApplyRequest& request,
        std::wstring& error) const;

    bool CreateOnlyByNameRequest(
        const std::wstring& exact_name,
        ApplyRequest& request,
        std::wstring& error) const;

    static OperationResult Execute(ApplyRequest request);
    static OperationResult Validate(
        ApplyRequest request, bool virtual_mode_aware = false);
    static OperationResult Restore(DisplaySnapshot snapshot);
    static OperationResult RestoreExtended();

private:
    struct InternalTarget {
        DisplayTarget display;
        LUID adapter_id{};
        UINT32 target_id = 0;
        std::vector<size_t> candidate_paths;
    };

    DisplaySnapshot all_paths_;
    std::vector<InternalTarget> internal_targets_;
    std::vector<DisplayTarget> targets_;
};

std::wstring FormatSystemError(LONG error_code);

}  // namespace mux
