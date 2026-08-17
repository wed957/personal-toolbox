#include "display_manager.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

using mux::ApplyRequest;
using mux::DisplayManager;
using mux::OperationResult;

namespace {

void Print(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(),
        size, nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
}

void PrintLine(const std::wstring& text) {
    Print(text);
    std::fwrite("\n", 1, 1, stdout);
}

int ListDisplays(DisplayManager& manager) {
    for (const auto& target : manager.targets()) {
        std::wstring line = target.active ? L"[on]  " : L"[off] ";
        line += target.name;
        if (!target.gdi_name.empty()) {
            line += L"  " + target.gdi_name;
        }
        if (target.width != 0 && target.height != 0) {
            line += L"  " + std::to_wstring(target.width) + L"x" +
                    std::to_wstring(target.height);
        }
        PrintLine(line);
    }
    return 0;
}

int PrintOperation(const OperationResult& result) {
    PrintLine(result.message);
    return result.success ? 0 : 2;
}

int SelfTest(DisplayManager& manager) {
    const auto& targets = manager.targets();
    if (targets.empty() || targets.size() > 16) {
        PrintLine(L"自检要求可用显示器数量在 1 到 16 之间。");
        return 1;
    }

    const unsigned long long combination_count = 1ull << targets.size();
    unsigned long long tested = 0;
    for (unsigned long long mask = 1; mask < combination_count; ++mask) {
        std::vector<std::wstring> keys;
        for (size_t index = 0; index < targets.size(); ++index) {
            if ((mask & (1ull << index)) != 0) {
                keys.push_back(targets[index].key);
            }
        }

        ApplyRequest request;
        std::wstring error;
        if (!manager.CreateApplyRequest(keys, request, error)) {
            PrintLine(L"组合路径规划失败：" + error);
            return 1;
        }
        if (!request.no_change) {
            const OperationResult validation = DisplayManager::Validate(request);
            if (!validation.success) {
                PrintLine(L"组合参数验证失败：" + validation.message);
                return 1;
            }
        }
        ++tested;
    }

    PrintLine(
        L"只读自检通过：" + std::to_wstring(tested) +
        L" 个非空显示器组合均通过路径规划和参数验证；未执行显示切换。");
    return 0;
}

int DiagnoseP25(DisplayManager& manager) {
    ApplyRequest full_request;
    std::wstring error;
    if (!manager.CreateOnlyByNameRequest(L"P25W2GC", full_request, error)) {
        PrintLine(error);
        return 1;
    }

    ApplyRequest active_only = full_request;
    std::erase_if(active_only.configuration.paths, [](const auto& path) {
        return (path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0;
    });

    const OperationResult full = DisplayManager::Validate(std::move(full_request));
    const OperationResult filtered = DisplayManager::Validate(std::move(active_only));
    PrintLine(L"完整 QDC_ALL_PATHS：" + full.message);
    PrintLine(L"仅 ACTIVE 路径：" + filtered.message);
    return full.success && filtered.success ? 0 : 2;
}

void UseBestModeLogic(ApplyRequest& request) {
    request.configuration.modes.clear();
    for (auto& path : request.configuration.paths) {
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
        path.targetInfo.scaling = DISPLAYCONFIG_SCALING_PREFERRED;
        path.targetInfo.refreshRate = {};
        path.targetInfo.scanLineOrdering =
            DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
    }
}

int DiagnoseAll(DisplayManager& manager) {
    const auto& targets = manager.targets();
    if (targets.empty() || targets.size() > 16) {
        PrintLine(L"诊断要求可用显示器数量在 1 到 16 之间。");
        return 1;
    }

    bool all_valid = true;
    bool baseline_validated = false;
    const unsigned long long combination_count = 1ull << targets.size();
    for (unsigned long long mask = 1; mask < combination_count; ++mask) {
        std::vector<std::wstring> keys;
        std::wstring label;
        for (size_t index = 0; index < targets.size(); ++index) {
            if ((mask & (1ull << index)) == 0) {
                continue;
            }
            keys.push_back(targets[index].key);
            if (!label.empty()) {
                label += L" + ";
            }
            label += targets[index].name;
        }

        ApplyRequest full_request;
        std::wstring error;
        if (!manager.CreateApplyRequest(keys, full_request, error)) {
            PrintLine(label + L"：路径规划失败：" + error);
            all_valid = false;
            continue;
        }
        if (!baseline_validated) {
            ApplyRequest baseline;
            baseline.configuration = full_request.rollback;
            const OperationResult baseline_result =
                DisplayManager::Validate(std::move(baseline));
            PrintLine(
                L"当前活跃布局基线：" +
                std::to_wstring(baseline_result.error_code));
            baseline_validated = true;
        }
        if (full_request.no_change) {
            PrintLine(label + L"：当前布局（跳过验证）");
            continue;
        }

        ApplyRequest best_mode = full_request;
        UseBestModeLogic(best_mode);
        ApplyRequest best_mode_virtual = best_mode;
        const OperationResult full = DisplayManager::Validate(std::move(full_request));
        const OperationResult best = DisplayManager::Validate(std::move(best_mode));
        const OperationResult best_virtual =
            DisplayManager::Validate(std::move(best_mode_virtual), true);
        PrintLine(
            label + L"：紧凑模式=" + std::to_wstring(full.error_code) +
            L"，best-mode=" + std::to_wstring(best.error_code) +
            L"，virtual-best=" + std::to_wstring(best_virtual.error_code));
        all_valid =
            all_valid && (full.success || best.success);
    }
    return all_valid ? 0 : 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    DisplayManager manager;
    std::wstring error;
    if (!manager.Refresh(error)) {
        PrintLine(error);
        return 1;
    }

    const std::wstring command = argc >= 2 ? argv[1] : L"list";
    if (command == L"list" || command == L"--list") {
        return ListDisplays(manager);
    }
    if (command == L"only-p25w2gc") {
        ApplyRequest request;
        if (!manager.CreateOnlyByNameRequest(L"P25W2GC", request, error)) {
            PrintLine(error);
            return 1;
        }
        return PrintOperation(DisplayManager::Execute(std::move(request)));
    }
    if (command == L"check-p25w2gc") {
        ApplyRequest request;
        if (!manager.CreateOnlyByNameRequest(L"P25W2GC", request, error)) {
            PrintLine(error);
            return 1;
        }
        PrintLine(L"P25W2GC 路径规划成功；未执行显示切换。");
        return 0;
    }
    if (command == L"self-test") {
        return SelfTest(manager);
    }
    if (command == L"diagnose-p25w2gc") {
        return DiagnoseP25(manager);
    }
    if (command == L"diagnose-all") {
        return DiagnoseAll(manager);
    }
    if (command == L"extend") {
        return PrintOperation(DisplayManager::RestoreExtended());
    }

    PrintLine(L"用法：MUX-cli.exe [list|self-test|diagnose-all|diagnose-p25w2gc|check-p25w2gc|only-p25w2gc|extend]");
    return 64;
}
