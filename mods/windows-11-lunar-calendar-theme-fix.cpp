// ==WindhawkMod==
// @id              windows-11-lunar-calendar-theme-fix
// @name            Windows 11 Lunar Calendar Theme Fix
// @description     Fix hard-to-read lunar holiday text when Windows and apps use different light/dark themes
// @version         0.1.1
// @author          deluxghost
// @github          https://github.com/deluxghost
// @homepage        https://github.com/deluxghost/my-windhawk
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lruntimeobject -lole32 -loleaut32 -lcomctl32
// @license         MIT
// ==/WindhawkMod==

// Copyright (c) 2026 deluxghost
// SPDX-License-Identifier: MIT

// ==WindhawkModReadme==
/*
# Windows 11 Lunar Calendar Theme Fix

Fixes hard-to-read holiday names in the Chinese lunar date line, which appears
beneath each date in the calendar opened from the Windows 11 taskbar clock.

When "Choose your default Windows mode" and "Choose your default app mode" are
set to different Light/Dark values, the calendar background follows the Windows
setting, while the lunar holiday text uses colors intended for the app setting.
This can leave the holiday text with too little contrast against the background.

The mod chooses the holiday text's accent-color shade using the Windows
light/dark setting, matching the calendar background.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace {

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace view = winrt::Windows::UI::ViewManagement;

constexpr wchar_t kActionCenterModuleName[] = L"Windows.UI.ActionCenter.dll";
constexpr wchar_t kLunarHighlightBrushKey[] = L"LunarCalendarHighlightBrush";

// These C++/CX methods use the native x64 calling convention. The foreground
// getter returns an owned ISolidColorBrush reference; the setter borrows one.
using OnGregorianForegroundColorChanged_t = void (*)(void*, void*);
using IsLunarHoliday_t = bool (*)(void*);
using GetForegroundBrush_t = void* (*)(void*);
using SetForegroundBrush_t = void (*)(void*, void*);
using ShouldSystemUseDarkMode_t = bool (*)();

HMODULE g_actionCenterModule = nullptr;
std::atomic<bool> g_calendarHookAttempted = false;
std::mutex g_calendarHookMutex;
bool g_calendarHooksReady = false;
bool g_unloading = false;
decltype(&LoadLibraryExW) g_loadLibraryExWOriginal = nullptr;
OnGregorianForegroundColorChanged_t
    g_onGregorianForegroundColorChangedOriginal = nullptr;
IsLunarHoliday_t g_isLunarHoliday = nullptr;
GetForegroundBrush_t g_getForegroundBrush = nullptr;
SetForegroundBrush_t g_setForegroundBrush = nullptr;
ShouldSystemUseDarkMode_t g_shouldSystemUseDarkMode = nullptr;
winrt::guid* g_lunarDayItemViewModelIid = nullptr;
UINT g_refreshExistingDatesMessage = 0;

bool IsHighContrastEnabled() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    winrt::check_bool(SystemParametersInfoW(
        SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0));
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool UpdateLunarHighlight(void* model) {
    if (!g_isLunarHoliday(model) || IsHighContrastEnabled()) {
        return false;
    }

    const wuxm::SolidColorBrush foreground{
        g_getForegroundBrush(model), winrt::take_ownership_from_abi};
    const auto highlight = wux::Application::Current().Resources().Lookup(
        winrt::box_value(winrt::hstring{kLunarHighlightBrushKey}))
        .as<wuxm::SolidColorBrush>();

    // Only replace the native holiday highlight. Native selected and disabled
    // foreground assignments must pass through without being recolored here.
    if (foreground != highlight) {
        return false;
    }

    // Query the native system-mode helper and accent palette on every refresh,
    // so theme changes do not depend on a separately cached theme value.
    const bool dark = g_shouldSystemUseDarkMode();
    const auto colorType = dark ? view::UIColorType::AccentLight3
                                : view::UIColorType::AccentDark2;
    const auto color = view::UISettings{}.GetColorValue(colorType);
    if (foreground.Color() == color) {
        return false;
    }

    // An independent brush avoids changing every consumer of a shared resource.
    const wuxm::SolidColorBrush replacement{color};
    g_setForegroundBrush(model, winrt::get_abi(replacement));
    Wh_Log(L"Updated lunar highlight: dark=%d, ARGB=%02X%02X%02X%02X",
           dark, color.A, color.R, color.G, color.B);
    return true;
}

void OnGregorianForegroundColorChangedHook(void* model, void* foreground) {
    g_onGregorianForegroundColorChangedOriginal(model, foreground);

    try {
        UpdateLunarHighlight(model);
    } catch (const winrt::hresult_error& error) {
        // Keep WinRT failures introduced by this mod inside the hook boundary.
        // The native assignment above remains valid; report the failure instead
        // of letting a C++ exception escape into the shell's C++/CX callback.
        Wh_Log(L"Failed to update lunar highlight: HRESULT=0x%08X, %s",
               static_cast<unsigned int>(error.code().value),
               error.message().c_str());
    }
}

unsigned int RefreshLunarDateModels(const wux::DependencyObject& element) {
    const auto textBlock = element.try_as<wuxc::TextBlock>();
    if (textBlock && textBlock.Name() == L"LunarTextBlock") {
        // Use the same model interface as the native loaded handler.
        // Updating its property preserves the TextBlock's binding.
        const auto context = textBlock.DataContext();
        wf::IInspectable model{nullptr};
        winrt::check_pointer(winrt::get_abi(context));
        winrt::check_hresult(winrt::get_unknown(context)->QueryInterface(
            *g_lunarDayItemViewModelIid, winrt::put_abi(model)));
        return UpdateLunarHighlight(winrt::get_abi(model));
    }

    unsigned int updatedCount = 0;
    const int childCount = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int index = 0; index < childCount; ++index) {
        updatedCount += RefreshLunarDateModels(
            wuxm::VisualTreeHelper::GetChild(element, index));
    }
    return updatedCount;
}

void RefreshExistingDates() {
    try {
        const auto window = wux::Window::Current();
        if (!window) {
            return;
        }
        const auto content = window.Content();
        if (!content) {
            return;
        }

        const auto updatedCount = RefreshLunarDateModels(content);
        Wh_Log(L"Updated %u existing lunar date models", updatedCount);
    } catch (const winrt::hresult_error& error) {
        Wh_Log(L"Failed to refresh existing lunar dates: HRESULT=0x%08X, %s",
               static_cast<unsigned int>(error.code().value),
               error.message().c_str());
    }
}

LRESULT CALLBACK InitialRefreshSubclassProc(HWND window,
                                          UINT message,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          DWORD_PTR) {
    if (message == g_refreshExistingDatesMessage) {
        RefreshExistingDates();
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK RefreshWindowThread(HWND window, LPARAM parameter) {
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) {
        return TRUE;
    }

    constexpr wchar_t kCoreWindowClass[] = L"Windows.UI.Core.CoreWindow";
    wchar_t className[ARRAYSIZE(kCoreWindowClass)]{};
    if (!GetClassNameW(window, className, ARRAYSIZE(className)) ||
        wcscmp(className, kCoreWindowClass) != 0) {
        return TRUE;
    }

    auto& refreshedThreads =
        *reinterpret_cast<std::unordered_set<DWORD>*>(parameter);
    if (!refreshedThreads.insert(threadId).second) {
        return TRUE;
    }

    // The temporary subclass runs on the owning UI thread and is removed
    // synchronously. No callback remains attached after initialization.
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(
            window, InitialRefreshSubclassProc, 0)) {
        const DWORD error = GetLastError();
        Wh_Log(L"Failed to access calendar UI thread %lu: error=%lu", threadId,
               error);
        return TRUE;
    }

    SendMessageW(window, g_refreshExistingDatesMessage, 0, 0);
    WindhawkUtils::RemoveWindowSubclassFromAnyThread(
        window, InitialRefreshSubclassProc);
    return TRUE;
}

bool HookCalendarSymbols(HMODULE module) {
    const WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{L"?OnGregorianForegroundColorChanged@"
          L"?Q__ILunarDayItemViewModelPublicNonVirtuals@ActionCenter@@"
          L"LunarDayItemViewModel@2@UE$AAAXPE$AAVSolidColorBrush@Media@"
          L"Xaml@UI@Windows@@@Z"},
         &g_onGregorianForegroundColorChangedOriginal,
         OnGregorianForegroundColorChangedHook},
        {{L"?get@?QIsLunarHoliday@__ILunarDayItemViewModelPublicNonVirtuals@"
          L"ActionCenter@@1LunarDayItemViewModel@3@UE$AAA_NXZ"},
         &g_isLunarHoliday},
        {{L"?get@?QForegroundBrush@__ILunarDayItemViewModelPublicNonVirtuals@"
          L"ActionCenter@@1LunarDayItemViewModel@3@UE$AAAPE$AAVSolidColorBrush@"
          L"Media@Xaml@UI@Windows@@XZ"},
         &g_getForegroundBrush},
        {{L"?set@?QForegroundBrush@__ILunarDayItemViewModelPublicNonVirtuals@"
          L"ActionCenter@@1LunarDayItemViewModel@3@UE$AAAXPE$AAVSolidColorBrush@"
          L"Media@Xaml@UI@Windows@@@Z"},
         &g_setForegroundBrush},
        {{L"?ShouldSystemUseDarkMode@SharedUtilities@@YA_NXZ"},
         &g_shouldSystemUseDarkMode},
        {{L"__uuidof_?AU__ILunarDayItemViewModelPublicNonVirtuals@ActionCenter@@"},
         &g_lunarDayItemViewModelIid},
    };

    WH_HOOK_SYMBOLS_OPTIONS options{};
    options.optionsSize = sizeof(options);
    options.noUndecoratedSymbols = TRUE;
    return WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks), &options);
}

bool InitializeCalendarHooks(HMODULE module) {
    // Serialize late initialization with unloading. The atomic flag also keeps
    // symbol resolution's own DLL loads from reentering this initialization.
    std::lock_guard lock(g_calendarHookMutex);
    if (g_unloading) {
        return false;
    }
    if (g_calendarHookAttempted.exchange(true)) {
        return g_calendarHooksReady;
    }

    // Retain an already loaded module without initiating DLL initialization.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(module),
                            &g_actionCenterModule)) {
        const DWORD error = GetLastError();
        Wh_Log(L"Failed to retain the calendar module: error=%lu", error);
        return false;
    }

    if (!HookCalendarSymbols(g_actionCenterModule)) {
        Wh_Log(L"Failed to resolve the required lunar calendar symbols");
        return false;
    }
    if (!Wh_ApplyHookOperations()) {
        Wh_Log(L"Failed to apply the lunar calendar hooks");
        return false;
    }

    g_calendarHooksReady = true;
    Wh_Log(L"Lunar calendar hooks initialized");
    return true;
}

HMODULE WINAPI LoadLibraryExWHook(LPCWSTR fileName, HANDLE file, DWORD flags) {
    const HMODULE module = g_loadLibraryExWOriginal(fileName, file, flags);
    const DWORD error = GetLastError();
    if (module && !g_calendarHookAttempted &&
        module == GetModuleHandleW(kActionCenterModuleName)) {
        InitializeCalendarHooks(module);
    }
    SetLastError(error);
    return module;
}

}  // namespace

BOOL Wh_ModInit() {
    g_refreshExistingDatesMessage = RegisterWindowMessageW(
        L"Windhawk_RefreshExistingDates_windows-11-lunar-calendar-theme-fix");
    if (!g_refreshExistingDatesMessage) {
        const DWORD error = GetLastError();
        Wh_Log(L"Failed to register the calendar refresh message: error=%lu", error);
        return FALSE;
    }

    // Let the shell load its calendar component at its normal startup point.
    // Forcing the load here can fail before its dependencies are initialized.
    // Kernel32 has a separate wrapper; WinRT can call KernelBase directly.
    const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    const auto loadLibraryExW = reinterpret_cast<decltype(&LoadLibraryExW)>(
        GetProcAddress(kernelBase, "LoadLibraryExW"));
    if (!loadLibraryExW) {
        const DWORD error = GetLastError();
        Wh_Log(L"Failed to find KernelBase!LoadLibraryExW: error=%lu", error);
        return FALSE;
    }
    if (!WindhawkUtils::SetFunctionHook(loadLibraryExW, LoadLibraryExWHook,
                                       &g_loadLibraryExWOriginal)) {
        Wh_Log(L"Failed to hook LoadLibraryExW");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    // Also cover a DLL loaded before the loader hook became active.
    const HMODULE module = GetModuleHandleW(kActionCenterModuleName);
    if (!module) {
        Wh_Log(L"Waiting for the shell to load the calendar module");
        return;
    }
    if (!InitializeCalendarHooks(module)) {
        return;
    }

    // Existing date models predate the hook and otherwise retain their old
    // foreground until the native calendar updates them.
    std::unordered_set<DWORD> refreshedThreads;
    if (!EnumWindows(RefreshWindowThread,
                     reinterpret_cast<LPARAM>(&refreshedThreads))) {
        const DWORD error = GetLastError();
        Wh_Log(L"Failed to enumerate calendar windows: error=%lu", error);
    }
}

void Wh_ModBeforeUninit() {
    std::lock_guard lock(g_calendarHookMutex);
    g_unloading = true;
}

void Wh_ModUninit() {
    if (g_actionCenterModule) {
        FreeLibrary(g_actionCenterModule);
        g_actionCenterModule = nullptr;
    }
}
