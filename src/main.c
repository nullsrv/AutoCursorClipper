// AutoCursorClipper
// 
// Copyright (C) 2026  nullsrv
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
// 
// SPDX-License-Identifier: GPL-3.0-only

////////////////////////////////////////////////////////////////////////////////
// Includes
////////////////////////////////////////////////////////////////////////////////

#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#define MNI_IMPLEMENTATION
#include <mni/mni.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
// Constants
////////////////////////////////////////////////////////////////////////////////

#define ACC_PROGRAM_NAME_STRING         TEXT("Auto Cursor Clipper v0.1")
#define ACC_HOMEPAGE                    TEXT("https://github.com/nullsrv/AutoCursorClipper")
#define ACC_COPYRIGHT                   TEXT("Copyright (C) 2026  nullsrv")
#define ACC_WINDOW_TITLE_MAX_LENGTH     2048
#define ACC_ALT_TAB_WNDCLASS            TEXT("XamlExplorerHostIslandWindow")


////////////////////////////////////////////////////////////////////////////////
// Types
////////////////////////////////////////////////////////////////////////////////

typedef struct AutoCursorClipper {
    Mni5                tray;
    wchar_t             *window;        // title of the window to clip

    bool                is_locked;    
    HWND                locked_hwnd;    // window handle to clipped window
    RECT                locked_rect;    // region that is currently clipped
    HWND                last_checked_hwnd;

    // Hooks
    HWINEVENTHOOK       foreground_hook;
    HWINEVENTHOOK       move_hook;
    HWINEVENTHOOK       minimize_hook;
    HWINEVENTHOOK       switchtask_hook;
    HWINEVENTHOOK       showhide_hook;
} AutoCursorClipper;

typedef enum {
    ACC_INITIALIZED = 0,

    ACC_FAILED_TO_PROCESS_COMMAND_LINE  = -1,
    ACC_FAILED_TO_CREATE_TRAY_ICON      = -2,
    ACC_FAILED_TO_SHOW_TRAY_ICON        = -3,
    ACC_FAILED_TO_CREATE_HOOKS          = -4,
} AccInitStatus;


////////////////////////////////////////////////////////////////////////////////
// Globals
////////////////////////////////////////////////////////////////////////////////

static AutoCursorClipper    g_ACC;                  // need to be global for hook proc
static bool                 g_enable_log = false;
static HWND                 g_about_dlg = NULL;


////////////////////////////////////////////////////////////////////////////////
// Forward declarations
////////////////////////////////////////////////////////////////////////////////

static void on_context_menu(Mni5 *mni, int id);
static void on_dpi_change(Mni5 *mni, int dpi);
static void on_system_theme_change(Mni5 *mni, MniThemeInfo mti);

static void CALLBACK acc_hook_proc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
static INT_PTR CALLBACK about_dlg_proc(HWND, UINT, WPARAM, LPARAM);

////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////

static void log_message(const char *fmt, ...) {
    if (!g_enable_log) {
        return;
    }

    static FILE *log_file = NULL;
    if (log_file == NULL) {
         fopen_s(&log_file, "AutoCursorClipper.log", "w");
    }

    if (log_file != NULL) {
        va_list arg_list;
        va_start(arg_list, fmt);

        char buffer[1024];
        int len = _vsnprintf(buffer, 1024-2, fmt, arg_list);

        va_end(arg_list);

        if (len > 0) {
            buffer[len++] = '\n';
            buffer[len] = '\0';

            fwrite(buffer, len, 1, log_file);

        #ifdef _DEBUG
            OutputDebugStringA(buffer);
        #endif
        }
    }
}

static bool is_color_light(DWORD color) {
    BYTE r = GetRValue(color);
    BYTE g = GetGValue(color);
    BYTE b = GetBValue(color);

    return (((5 * g) + (2 * r) + b) > (8 * 128));
}
 
static HICON load_icon_from_res(int id, int width, int height, int dpi) {
    int w = MulDiv(width, dpi, 96);
    int h = MulDiv(height, dpi, 96);
    HICON ico = (HICON)LoadImageW(
        GetModuleHandle(NULL), MAKEINTRESOURCE(id), IMAGE_ICON, w, h, LR_DEFAULTCOLOR | LR_SHARED);

    return ico;
}


////////////////////////////////////////////////////////////////////////////////
// AutoCursorClipper Functions
////////////////////////////////////////////////////////////////////////////////

static void acc_refresh_icon_ex(AutoCursorClipper *acc, int dpi, MniThemeInfo mti) {
    UNREFERENCED_PARAMETER(mti);

    int id = acc->is_locked ? IDI_ICON_GREEN : IDI_ICON_RED;
    HICON ico = load_icon_from_res(id, 16, 16, dpi);
    
    MniError err = MniSetIcon(&acc->tray, ico, MNI_RDP_MANUAL);
    if (MNI_FAILED(err)) {
        log_message("failed to change tray icon, error: %d", err);
    }
}

static void acc_refresh_icon(AutoCursorClipper *acc) {
    acc_refresh_icon_ex(acc, acc->tray.dpi, acc->tray.system_theme);
}

static bool acc_process_cmdline(AutoCursorClipper *acc) {
    int argc = 0;
    LPWSTR *args = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (args == NULL) {
        log_message("CommandLineToArgvW() failed");
        return false;
    }

    for (int i = 1; i < argc; i += 1) {
        LPWSTR arg = args[i];
        if (arg != NULL) {
            if (wcscmp(L"-log", arg) == 0) {
                g_enable_log = true;
            } else {
                wchar_t *window = wcsdup(arg);
                if (window == NULL) {
                    log_message("out of memory");
                    break;
                }

                acc->window = window;
                break;
            }
        }
    }

    LocalFree(args);

    if (acc->window == NULL) {
        log_message("missing <window title> argument");
        return false;
    }

    return true;
}

static AccInitStatus acc_init(AutoCursorClipper *acc) {
    assert(acc != NULL && "invalid argument");

    log_message("initializing auto clipper...");

    // Process command line arguments.
    if (!acc_process_cmdline(acc)) {
        return ACC_FAILED_TO_PROCESS_COMMAND_LINE;
    }

    acc->is_locked = false;
    acc->locked_hwnd = NULL;
    acc->locked_rect = (RECT){0,0,0,0};
    acc->last_checked_hwnd = NULL;

    // Setup MniInfo.
    MniInfo info;
    memset(&info, 0, sizeof(info));

    HMENU menu_main = CreateMenu();
    HMENU menu_popup = CreateMenu();

    AppendMenuW(menu_main, MF_STRING | MF_DISABLED, 0, ACC_PROGRAM_NAME_STRING);
    AppendMenuW(menu_main, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu_main, MF_STRING, IDM_MENU_ABOUT, L"About");
    AppendMenuW(menu_main, MF_STRING, IDM_MENU_EXIT, L"Exit");
    AppendMenuW(menu_popup, MF_POPUP, (UINT_PTR)menu_main, L"");

    info.menu                       = menu_popup;
    info.menu_rdp                   = MNI_RDP_AUTO;
    info.tip                        = L"Auto Cursor Clipper";
    info.user_data1                 = acc;
    info.on_dpi_change              = on_dpi_change;
    info.on_system_theme_change     = on_system_theme_change;
    info.on_context_menu_item_click = on_context_menu;

    // Init tray icon.    
    if (MNI_FAILED(MniInit(&acc->tray, info))) {
        log_message("failed to initialize tray icon");
        return ACC_FAILED_TO_CREATE_TRAY_ICON;
    }

    acc_refresh_icon(acc);

    // Show the icon in Notification Area.
    if (MNI_FAILED(MniShow(&acc->tray))) {
        log_message("failed to show tray icon");
        return ACC_FAILED_TO_SHOW_TRAY_ICON;
    }

    // Init hooks.
    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;

    acc->foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, acc_hook_proc, 0, 0, flags);
    if (acc->foreground_hook == NULL) {
        return ACC_FAILED_TO_CREATE_HOOKS;
    }

    acc->move_hook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL, acc_hook_proc, 0, 0, flags);
    if (acc->move_hook == NULL) {
        return ACC_FAILED_TO_CREATE_HOOKS;
    }     

    acc->minimize_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL, acc_hook_proc, 0, 0, flags);
    if (acc->minimize_hook  == NULL) {
        return ACC_FAILED_TO_CREATE_HOOKS;
    }

    // This event is never sent for some reason.
    //acc->switchtask_hook = SetWinEventHook(EVENT_SYSTEM_SWITCHSTART, EVENT_SYSTEM_SWITCHEND, NULL, acc_hook_proc, 0, 0, flags);
    //if (acc->switchtask_hook == NULL) {
    //    return ACC_FAILED_TO_CREATE_HOOKS;
    //}

    // We don't need this,
    //acc->showhide_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, NULL, acc_hook_proc, 0, 0, flags);
    //if (acc->showhide_hook == NULL) {
    //    return ACC_FAILED_TO_CREATE_HOOKS;
    //}

    log_message("initialization done");

    return ACC_INITIALIZED;
}

static int acc_run(AutoCursorClipper *acc) {
    UNREFERENCED_PARAMETER(acc);

    MSG msg;

    while (1) {
        BOOL ret = GetMessage(&msg, NULL, 0, 0);
        if (ret == -1) {
            return -1;
        }

        if (ret == FALSE) {
            break;
        }

        if (!IsDialogMessageW(g_about_dlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)(msg.wParam);
}

static void acc_free(AutoCursorClipper *acc) {
    assert(acc != NULL && "invalid argument");
    
    if (acc->foreground_hook) {
        UnhookWinEvent(acc->foreground_hook);
    }

    if (acc->move_hook) {
        UnhookWinEvent(acc->move_hook);
    }

    if (acc->minimize_hook) {
        UnhookWinEvent(acc->minimize_hook);
    }

    if (acc->switchtask_hook) {
        UnhookWinEvent(acc->switchtask_hook);
    }

    if (acc->showhide_hook) {
        UnhookWinEvent(acc->showhide_hook);
    }

    MniRelease(&acc->tray);

    if (acc->window != NULL) {
        free(acc->window);
        acc->window = NULL;
    }
}

static bool acc_lock(AutoCursorClipper *acc, HWND hwnd) {
    assert(acc != NULL && "invalid argument");

    log_message("trying to lock cursor...");

    if (hwnd != NULL) {
        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) {
            // TODO: should we unlock here?
            log_message("failed to get rect for the window %p", hwnd);
            return false;
        } else {
            // After we restore window from minimize state, foreground event is fired before
            // the minimize end event and window rect has invalid values.
            // This check make sure rect has valid values.
            HMONITOR hmon = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
            if (hmon == NULL) {
                log_message("invalid rect for window window %p (%d, %d, %d, %d)",
                    hwnd, rect.left, rect.top, rect.right, rect.bottom);
                return false;
            }

            if (!ClipCursor(&rect)) {
                log_message("failed to lock cursor");
                return false;
            }

            acc->is_locked = true;
            acc->locked_hwnd = hwnd;
            acc->locked_rect = rect;
            acc_refresh_icon(acc);
        }
    }

    return true;
}

static bool acc_unlock(AutoCursorClipper *acc) {
    assert(acc != NULL && "invalid argument");

    if (!ClipCursor(NULL)) {
        log_message("failed to unlock cursor");
        return false;
    }

    acc->is_locked = false;
    acc->locked_hwnd = NULL;
    acc->locked_rect = (RECT){0,0,0,0};
    acc_refresh_icon(acc);

    return true;
}

static bool acc_check_window(AutoCursorClipper *acc, HWND hwnd) {
    assert(acc != NULL && "invalid argument");

    if (hwnd == NULL || acc->window == NULL) {
        return false;
    }

    wchar_t title[ACC_WINDOW_TITLE_MAX_LENGTH];
    wchar_t wndclass[MAX_PATH];
    memset(&title, 0, sizeof(title));
    memset(&wndclass, 0, sizeof(wndclass));

    GetWindowTextW(hwnd, title, ACC_WINDOW_TITLE_MAX_LENGTH - 1);
    GetClassNameW(hwnd, wndclass, MAX_PATH - 1);

    log_message("checking window %p...", hwnd);

    log_message("\ttitle: %ls", title);
    log_message("\twndclass: %ls", wndclass);

    if (wcsncmp(title, acc->window, ACC_WINDOW_TITLE_MAX_LENGTH) == 0) {
        if (acc_lock(acc, hwnd)) {
            log_message("cursor locked to window %p (%d, %d, %d, %d)",
                hwnd,
                acc->locked_rect.left, acc->locked_rect.top,
                acc->locked_rect.right, acc->locked_rect.bottom);
        }
    } else {
        // When doing Alt+Tab to restore the window, the Alt-Tab window is getting to foreground
        // right after the target windows is clipped. This check is here to prevent the steal.
        if (acc->last_checked_hwnd == acc->locked_hwnd) {
            if (wcscmp(wndclass, ACC_ALT_TAB_WNDCLASS) != 0) {
                if (acc_unlock(acc)) {
                    log_message("cursor unlocked");
                }
            }
        }
    }

    acc->last_checked_hwnd = hwnd;

    return true;
}

static void CALLBACK acc_hook_proc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime
) {
    UNREFERENCED_PARAMETER(hWinEventHook);
    UNREFERENCED_PARAMETER(idObject);
    UNREFERENCED_PARAMETER(idChild);
    UNREFERENCED_PARAMETER(dwEventThread);
    UNREFERENCED_PARAMETER(dwmsEventTime);

    AutoCursorClipper *acc = &g_ACC;

    switch (event) {
    case EVENT_SYSTEM_FOREGROUND: {
        log_message("event: EVENT_SYSTEM_FOREGROUND (hwnd: %p)", hwnd);
        acc_check_window(acc, hwnd);
        break;
    }
    case EVENT_SYSTEM_MOVESIZESTART:
        log_message("event: EVENT_SYSTEM_MOVESIZESTART (hwnd: %p)", hwnd);
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        log_message("event: EVENT_SYSTEM_MOVESIZEEND (hwnd: %p)", hwnd);
        acc_check_window(acc, hwnd); 
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
        log_message("event: EVENT_SYSTEM_MINIMIZESTART (hwnd: %p)", hwnd);
        break;
    case EVENT_SYSTEM_MINIMIZEEND:
        log_message("event: EVENT_SYSTEM_MINIMIZEEND (hwnd: %p)", hwnd);
        acc_check_window(acc, hwnd); 
        break;
    case EVENT_OBJECT_SHOW:
        log_message("event: EVENT_OBJECT_SHOW (hwnd: %p)", hwnd);
        break;
    case EVENT_OBJECT_HIDE:
        log_message("event: EVENT_OBJECT_HIDE (hwnd: %p)", hwnd);
        break;
    case EVENT_SYSTEM_SWITCHSTART:
        log_message("event: EVENT_SYSTEM_SWITCHSTART (hwnd: %p)", hwnd);
        break;
    case EVENT_SYSTEM_SWITCHEND:
        log_message("event: EVENT_SYSTEM_SWITCHEND (hwnd: %p)", hwnd);
        break;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Window Events
////////////////////////////////////////////////////////////////////////////////

static void on_context_menu(Mni5 *mni, int id) {
    AutoCursorClipper *acc = mni->user_data1;
    assert(acc != NULL);

    static bool is_about_dialog = false;

    switch (id) {
    case IDM_MENU_ABOUT:
        if (!is_about_dialog) {
            is_about_dialog = true;
            DialogBoxParamW(0, MAKEINTRESOURCE(IDD_DIALOG_ABOUT), mni->window_handle, about_dlg_proc, (LPARAM)acc);
            is_about_dialog = false;
        }
        break;
    case IDM_MENU_EXIT:
        MniQuit();
        break;
    }
}

static void on_dpi_change(Mni5 *mni, int dpi) {
    AutoCursorClipper *acc = mni->user_data1;
    assert(acc != NULL);

    acc_refresh_icon_ex(acc, dpi, mni->system_theme);
}

static void on_system_theme_change(Mni5 *mni, MniThemeInfo mti) {
    AutoCursorClipper *acc = mni->user_data1;
    assert(acc != NULL);

    acc_refresh_icon_ex(acc, mni->dpi, mti);
}


////////////////////////////////////////////////////////////////////////////////
// Show Error/Usage Boxes
////////////////////////////////////////////////////////////////////////////////

static void show_usage_box(void) {
    const wchar_t *usage = 
        L"Usage: AutoCursorClipper.exe [options] <window title>\n\n"
        L"Options:\n"
        L"    -log    [enable logging]";

    MessageBoxW(NULL, usage, L"AutoCursorClipper", MB_OK);
}

static void show_error_box(AccInitStatus status) {
    switch (status) {
    case ACC_FAILED_TO_PROCESS_COMMAND_LINE:
        show_usage_box();
        break;
    case ACC_FAILED_TO_CREATE_TRAY_ICON:
        MessageBoxW(NULL, L"Failed to create tray icon!", L"Error", MB_OK);
        break;
    case ACC_FAILED_TO_SHOW_TRAY_ICON:
        MessageBoxW(NULL, L"Failed to show tray icon!", L"Error", MB_OK);
        break;
    case ACC_FAILED_TO_CREATE_HOOKS:
        MessageBoxW(NULL, L"Failed to create event hooks!", L"Error", MB_OK);
        break;
    }
}


////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////
int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_     LPWSTR    lpCmdLine,
    _In_     int       nShowCmd
) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    INITCOMMONCONTROLSEX ccs = {0};
    ccs.dwSize = sizeof(ccs);
    ccs.dwICC  = ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
    if (!InitCommonControlsEx(&ccs)) {
        log_message("InitCommonControlsEx() failed");
        return -255;
    }

#ifdef _DEBUG
    g_enable_log = true;
#endif

    int ret_code = 0;

    AutoCursorClipper *acc = &g_ACC;;
    memset(acc, 0, sizeof(AutoCursorClipper));

    AccInitStatus status = acc_init(acc);
    if (status < 0) {
        show_error_box(status);
        ret_code = (int)status;
        goto cleanup;
    }

    ret_code = acc_run(acc);

cleanup:
    acc_free(acc);

    return ret_code;
}


////////////////////////////////////////////////////////////////////////////////
// About Dialog
////////////////////////////////////////////////////////////////////////////////

static INT_PTR CALLBACK about_dlg_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        AutoCursorClipper *acc = (AutoCursorClipper *)lParam;

        HWND hico = GetDlgItem(hwnd, IDC_ABOUT_PROGRAM_ICON);
        RECT r2;
        GetClientRect(hico, &r2);

        POINT px1 = {r2.left, r2.top};
        POINT px2 = {r2.right, r2.bottom};
        MapWindowPoints(hico, GetParent(hico), &px1, 1);
        MapWindowPoints(hico, GetParent(hico), &px2, 1);

        int x0 = MulDiv(px1.x, acc->tray.dpi, 96);
        int y0 = MulDiv(px1.y, acc->tray.dpi, 96);
        int x1 = MulDiv(px2.x, acc->tray.dpi, 96);
        int y1 = MulDiv(px2.y, acc->tray.dpi, 96);
        SetWindowPos(hico, 0, x0, y0, x1, y1, SWP_NONE);

        HICON ico = load_icon_from_res(IDI_ICON_ACC, 48, 48, acc->tray.dpi);
        SendDlgItemMessageW(hwnd, IDC_ABOUT_PROGRAM_ICON, STM_SETIMAGE, IMAGE_ICON, (LPARAM)ico);
        SetDlgItemTextW(hwnd, IDC_ABOUT_PROGRAM_NAME, ACC_PROGRAM_NAME_STRING);
        SetDlgItemTextW(hwnd, IDC_ABOUT_COPYRIGHT, ACC_COPYRIGHT);
        SetDlgItemTextW(hwnd, IDC_ABOUT_HOMEPAGE, L"<a href=\"" ACC_HOMEPAGE "\">" ACC_HOMEPAGE "</a>");
        return TRUE;
    }

    case WM_COMMAND:
        EndDialog(hwnd, IDOK);
        break;

    case WM_NOTIFY:
        switch (((LPNMHDR)lParam)->code)
        {
        case NM_CLICK:
        case NM_RETURN: {
            PNMLINK link = (PNMLINK)lParam;
            LITEM item = link->item;
            ShellExecuteW(NULL, L"open", item.szUrl, NULL, NULL, SW_SHOW);
            EndDialog(hwnd, IDOK);
            break;
        }
        }

    default:
        return FALSE;
    }

    return TRUE;
}
