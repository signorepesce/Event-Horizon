#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "../core/mpv.h"
#include "../core/wallpaper.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <math.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wtsapi32.h>

#define APP L"event_horizon"
#define NAME L"Event Horizon"
#define WM_TRAY (WM_APP + 1)
#define WM_ENCODED (WM_APP + 2)
#define TIMER_WAKE 1
#define SCAN_DELAY 0.3
#define RETRY_DELAY 30.0
#define REFUSED_MAX 8
#define SLIDER_STEPS 400
#define MAX_TIERS 16
#define MAX_RECTS 512
#define MAX_REGIONS 16

enum { ID_PAUSE = 100, ID_CHOOSE, ID_BUILTIN, ID_SPEED, ID_BATTERY, ID_LOW_POWER, ID_LOGIN, ID_QUIT, ID_FIT = 200 };

static const GUID power_saving_status = {0xE00958C0, 0xC213, 0x4ACE, {0xAC, 0x77, 0xFE, 0xCC, 0xED, 0x2E, 0xEE, 0xA5}};


static struct {
    HINSTANCE instance;
    HWND control, host, worker, progman;
    NOTIFYICONDATAW tray;
    HANDLE job;
    HANDLE mpv;
    double mpv_started, retry_at;
    char playing[WP_PATH_MAX];
    double loop_length;
    wchar_t data_dir[MAX_PATH];
    wchar_t variants[MAX_PATH];
    HANDLE encoder, encoder_wait;
    bool encoder_stopped;
    char encoding[MAX_PATH];
    char encoding_source[WP_PATH_MAX];
    char fit_name[MAX_PATH];
    char fit_source[WP_PATH_MAX];
    wp_tier fit_tier;
    double fit_due;
    char refused[REFUSED_MAX][MAX_PATH];
    int refused_count;
    wchar_t exe_dir[MAX_PATH];
    wchar_t folder[MAX_PATH];
    wchar_t config_path[MAX_PATH];
    wchar_t pipe[128];
    wchar_t mpv_exe[MAX_PATH];
    char tier_paths[MAX_TIERS][WP_PATH_MAX];
    wp_tier tiers[MAX_TIERS];
    int tier_count;
    char preview[WP_PATH_MAX];
    wp_config config;
    wp_system system;
    wp_surface surface;
    wp_rect canvas;
    wp_rect regions[MAX_REGIONS];
    int region_count;
    double rate;
    int mpv_version;
    bool covered;
    double last_scan, scan_at;
    uint64_t synced;
    HWINEVENTHOOK hooks[3];
} app;

static double now(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static void to_wide(const char *in, wchar_t *out, int size) {
    if (!MultiByteToWideChar(CP_UTF8, 0, in, -1, out, size)) out[0] = 0;
}

static void to_utf8(const wchar_t *in, char *out, int size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, out, size, NULL, NULL)) out[0] = 0;
}

static bool file_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void load_config(void) {
    wp_config_default(&app.config);
    HANDLE f = CreateFileW(app.config_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char text[8192];
    DWORD n = 0;
    ReadFile(f, text, sizeof text - 1, &n, NULL);
    CloseHandle(f);
    text[n] = 0;
    wp_config_parse(&app.config, text);
}

static void save_config(void) {
    char text[8192];
    size_t n = wp_config_format(&app.config, text, sizeof text);
    wchar_t temporary[MAX_PATH + 8];
    swprintf(temporary, MAX_PATH + 8, L"%ls.tmp", app.config_path);
    HANDLE f = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, text, (DWORD)n, &written, NULL);
    CloseHandle(f);
    MoveFileExW(temporary, app.config_path, MOVEFILE_REPLACE_EXISTING);
}

static void scan_tiers(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        char name[MAX_PATH * 3];
        to_utf8(entry.cFileName, name, sizeof name);
        wp_tier tier;
        if (app.tier_count < MAX_TIERS && wp_tier_parse(name, &tier)) {
            wchar_t full[MAX_PATH];
            swprintf(full, MAX_PATH, L"%ls\\%ls", dir, entry.cFileName);
            to_utf8(full, app.tier_paths[app.tier_count], WP_PATH_MAX);
            app.tiers[app.tier_count++] = tier;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static void load_tiers(void) {
    app.tier_count = 0;
    if (app.data_dir[0]) scan_tiers(app.data_dir);
    scan_tiers(app.variants);
}

static int tier_index(const char *path) {
    for (int i = 0; i < app.tier_count; i++) {
        if (strcmp(app.tier_paths[i], path) == 0) return i;
    }
    return -1;
}

static void resolve_paths(void) {
    GetModuleFileNameW(NULL, app.exe_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(app.exe_dir, L'\\');
    if (slash) *slash = 0;
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) wcscpy(appdata, app.exe_dir);
    swprintf(app.folder, MAX_PATH, L"%ls\\%ls", appdata, APP);
    CreateDirectoryW(app.folder, NULL);
    swprintf(app.config_path, MAX_PATH, L"%ls\\config", app.folder);
    swprintf(app.pipe, 128, L"\\\\.\\pipe\\%ls_mpv_%lu", APP, GetCurrentProcessId());

    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) wcscpy(local, appdata);
    swprintf(app.variants, MAX_PATH, L"%ls\\%ls", local, APP);
    CreateDirectoryW(app.variants, NULL);
    swprintf(app.variants, MAX_PATH, L"%ls\\%ls\\variants", local, APP);
    CreateDirectoryW(app.variants, NULL);

    wchar_t data[MAX_PATH];
    if (GetEnvironmentVariableW(L"EVENT_HORIZON_DATA", data, MAX_PATH)) {
        scan_tiers(data);
        if (app.tier_count > 0) wcscpy(app.data_dir, data);
    }
    const wchar_t *subdirs[] = {L"", L"\\videos", L"\\share", L"\\..\\..\\media"};
    for (int i = 0; i < 4 && app.tier_count == 0; i++) {
        swprintf(data, MAX_PATH, L"%ls%ls", app.exe_dir, subdirs[i]);
        scan_tiers(data);
        if (app.tier_count > 0) wcscpy(app.data_dir, data);
    }
    scan_tiers(app.variants);
    for (int i = 0; i < 4 && !app.preview[0]; i++) {
        swprintf(data, MAX_PATH, L"%ls%ls\\gargantua.gif", app.exe_dir, subdirs[i]);
        if (file_exists(data)) to_utf8(data, app.preview, WP_PATH_MAX);
    }
    swprintf(app.mpv_exe, MAX_PATH, L"%ls\\mpv.exe", app.exe_dir);
    if (file_exists(app.mpv_exe) || SearchPathW(NULL, L"mpv.exe", NULL, MAX_PATH, app.mpv_exe, NULL)) return;
    const wchar_t *roots[] = {L"ProgramFiles", L"ProgramFiles", L"LOCALAPPDATA", L"USERPROFILE"};
    const wchar_t *installs[] = {L"MPV Player", L"mpv", L"Programs\\mpv", L"scoop\\shims"};
    for (int i = 0; i < 4; i++) {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(roots[i], base, MAX_PATH)) continue;
        swprintf(app.mpv_exe, MAX_PATH, L"%ls\\%ls\\mpv.exe", base, installs[i]);
        if (file_exists(app.mpv_exe)) return;
    }
    app.mpv_exe[0] = 0;
}

static bool ipc_request(const char *command, int id, char *reply, size_t size) {
    HANDLE pipe = CreateFileW(app.pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    bool ok = WriteFile(pipe, command, (DWORD)strlen(command), &written, NULL) != 0;
    if (ok && id > 0 && reply) {
        ok = false;
        char buffer[4096];
        DWORD used = 0;
        double deadline = now() + 0.5;
        while (!ok && now() < deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) break;
            if (!available) {
                Sleep(10);
                continue;
            }
            DWORD n = 0;
            if (!ReadFile(pipe, buffer + used, (DWORD)(sizeof buffer - 1 - used), &n, NULL) || !n) break;
            used += n;
            buffer[used] = 0;
            char *line = buffer, *newline;
            while ((newline = strchr(line, '\n'))) {
                *newline = 0;
                double got;
                if (wp_json_number(line, "request_id", &got) && (int)got == id) {
                    snprintf(reply, size, "%s", line);
                    ok = true;
                    break;
                }
                line = newline + 1;
            }
            if (!ok) {
                size_t rest = strlen(line);
                memmove(buffer, line, rest + 1);
                used = (DWORD)rest;
            }
        }
    }
    CloseHandle(pipe);
    return ok;
}

static void ipc_set(const char *property, const char *value) {
    char command[WP_MPV_COMMAND_MAX];
    wp_mpv_set(command, sizeof command, property, value);
    ipc_request(command, 0, NULL, 0);
}

static double ipc_number(const char *property) {
    const char *argv[] = {"get_property", property};
    char command[256], reply[4096];
    wp_mpv_command(command, sizeof command, 7, argv, 2);
    double value = -1;
    if (ipc_request(command, 7, reply, sizeof reply)) wp_json_number(reply, "data", &value);
    return value;
}

static int ipc_version(void) {
    const char *argv[] = {"get_property", "mpv-version"};
    char command[256], reply[4096], text[128];
    wp_mpv_command(command, sizeof command, 8, argv, 2);
    if (ipc_request(command, 8, reply, sizeof reply) && wp_json_string(reply, "data", text, sizeof text))
        return wp_mpv_version(text);
    return 0;
}

static void append_argument(wchar_t *out, size_t capacity, const wchar_t *argument) {
    size_t n = wcslen(out);
    if (n + 4 >= capacity) return;
    out[n++] = L' ';
    out[n++] = L'"';
    for (const wchar_t *c = argument; *c && n + 4 < capacity; c++) {
        size_t backslashes = 0;
        while (*c == L'\\') {
            c++;
            backslashes++;
        }
        size_t repeat = !*c ? backslashes * 2 : *c == L'"' ? backslashes * 2 + 1 : backslashes;
        for (size_t i = 0; i < repeat && n + 4 < capacity; i++) out[n++] = L'\\';
        if (!*c) break;
        out[n++] = *c;
    }
    out[n++] = L'"';
    out[n] = 0;
}

static bool write_out(void *context, const char *data, size_t size) {
    return fwrite(data, 1, size, context) == size;
}

static size_t read_in(void *context, uint64_t offset, void *buffer, size_t size) {
    if (_fseeki64(context, (long long)offset, SEEK_SET) != 0) return 0;
    return fread(buffer, 1, size, context);
}

static double video_length(const char *path) {
    wchar_t wide[WP_PATH_MAX];
    to_wide(path, wide, WP_PATH_MAX);
    FILE *file = _wfopen(wide, L"rb");
    if (!file) return -1;
    double length = -1;
    if (_fseeki64(file, 0, SEEK_END) == 0) {
        long long end = _ftelli64(file);
        if (end > 0) length = wp_mp4_duration(read_in, file, (uint64_t)end);
    }
    fclose(file);
    return length;
}

static bool prepare_source(const char *path, bool image, char *source, size_t size) {
    snprintf(source, size, "%s", path);
    app.loop_length = image ? -1 : video_length(path);
    if (app.loop_length <= 0) return false;
    wchar_t list[MAX_PATH], temp[MAX_PATH];
    swprintf(list, MAX_PATH, L"%ls\\loop-%016llx.ffconcat", app.folder, (unsigned long long)wp_hash(path));
    swprintf(temp, MAX_PATH, L"%ls.tmp", list);
    FILE *file = _wfopen(temp, L"wb");
    if (!file) return false;
    bool ok = wp_mpv_concat_write(write_out, file, path, app.loop_length) > 0;
    ok = fclose(file) == 0 && ok;
    if (!ok || !MoveFileExW(temp, list, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp);
        return false;
    }
    to_utf8(list, source, (int)size);
    return true;
}

static double loop_position(void) {
    double position = ipc_number("time-pos");
    if (position > 0 && app.loop_length > 0) position = fmod(position, app.loop_length);
    return position;
}

static void stop_mpv(void) {
    if (app.mpv) {
        ipc_request("{\"command\":[\"quit\"]}\n", 0, NULL, 0);
        if (WaitForSingleObject(app.mpv, 500) == WAIT_TIMEOUT) TerminateProcess(app.mpv, 0);
        CloseHandle(app.mpv);
    }
    app.mpv = NULL;
    app.playing[0] = 0;
}

static void start_mpv(const char *path, bool image, double start) {
    stop_mpv();
    if (!app.mpv_exe[0] || !app.host) return;
    char source[WP_PATH_MAX];
    bool concat = prepare_source(path, image, source, sizeof source);
    char pipe[128], window[32];
    to_utf8(app.pipe, pipe, sizeof pipe);
    snprintf(window, sizeof window, "%lld", (long long)(intptr_t)app.host);
    wp_mpv_options options;
    wp_mpv_build_options(&options, WP_MPV_WINDOWS, app.config.prefs.fit, app.rate, false, pipe, window);
    static wchar_t line[32768];
    swprintf(line, 32768, L"\"%ls\"", app.mpv_exe);
    wchar_t wide[WP_PATH_MAX];
    for (int i = 0; i < options.count; i++) {
        to_wide(options.items[i], wide, WP_PATH_MAX);
        append_argument(line, 32768, wide);
    }
    if (image) append_argument(line, 32768, L"--image-display-duration=inf");
    if (start > 0) {
        swprintf(wide, WP_PATH_MAX, L"--start=%.3f", start);
        append_argument(line, 32768, wide);
    }
    if (concat) {
        append_argument(line, 32768, L"--{");
        to_wide("--" WP_MPV_CONCAT_FORMAT, wide, WP_PATH_MAX);
        append_argument(line, 32768, wide);
    } else {
        append_argument(line, 32768, L"--");
    }
    to_wide(source, wide, WP_PATH_MAX);
    append_argument(line, 32768, wide);
    if (concat) append_argument(line, 32768, L"--}");
    STARTUPINFOW startup = {0};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS,
                        NULL, NULL, &startup, &process)) return;
    if (app.job) AssignProcessToJobObject(app.job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    app.mpv = process.hProcess;
    app.mpv_started = now();
    snprintf(app.playing, sizeof app.playing, "%s", path);
    for (int i = 0; i < 40; i++) {
        if (WaitNamedPipeW(app.pipe, 25)) break;
        Sleep(25);
    }
    app.mpv_version = ipc_version();
}

static void switch_file(const char *path, bool image, bool keep_time) {
    if (!app.mpv) return;
    double position = keep_time ? loop_position() : 0;
    if (position < 0) position = 0;
    char source[WP_PATH_MAX];
    bool concat = prepare_source(path, image, source, sizeof source);
    ipc_set("image-display-duration", image ? "inf" : "5");
    char command[WP_MPV_COMMAND_MAX];
    wp_mpv_loadfile(command, sizeof command, source, position, app.mpv_version, concat);
    ipc_request(command, 0, NULL, 0);
    snprintf(app.playing, sizeof app.playing, "%s", path);
}

static BOOL CALLBACK find_worker(HWND top, LPARAM out) {
    if (FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL)) *(HWND *)out = FindWindowExW(NULL, top, L"WorkerW", NULL);
    return TRUE;
}

static BOOL CALLBACK add_monitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data) {
    (void)dc;
    (void)rect;
    (void)data;
    MONITORINFO info = {0};
    info.cbSize = sizeof info;
    if (app.region_count < MAX_REGIONS && GetMonitorInfoW(monitor, &info)) {
        RECT w = info.rcWork;
        app.regions[app.region_count++] = (wp_rect){w.left, w.top, w.right - w.left, w.bottom - w.top};
    }
    return TRUE;
}

static void measure_screens(void) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    app.canvas = (wp_rect){x, y, w, h};
    app.region_count = 0;
    EnumDisplayMonitors(NULL, NULL, add_monitor, 0);
    if (app.host) SetWindowPos(app.host, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK host_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(window, &r);
        FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, w, l);
}

static bool attach_to_desktop(void) {
    app.progman = FindWindowW(L"Progman", NULL);
    if (!app.progman) return false;
    SendMessageTimeoutW(app.progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, NULL);
    SendMessageTimeoutW(app.progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);
    app.worker = FindWindowExW(app.progman, NULL, L"WorkerW", NULL);
    if (!app.worker) EnumWindows(find_worker, (LPARAM)&app.worker);
    HWND parent = app.worker ? app.worker : app.progman;
    WNDCLASSW type = {0};
    type.lpfnWndProc = host_proc;
    type.hInstance = app.instance;
    type.lpszClassName = L"event_horizon_host";
    type.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&type);
    measure_screens();
    app.host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"event_horizon_host", APP, WS_CHILD | WS_VISIBLE,
                               0, 0, (int)app.canvas.w, (int)app.canvas.h, parent, NULL, app.instance, NULL);
    return app.host != NULL;
}

static BOOL CALLBACK collect_window(HWND window, LPARAM data) {
    struct { wp_rect *rects; int count; } *out = (void *)data;
    if (out->count >= MAX_RECTS || window == app.host || window == app.worker || window == app.progman) return TRUE;
    if (!IsWindowVisible(window) || IsIconic(window)) return TRUE;
    LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (extended & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) return TRUE;
    if (extended & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(window, NULL, &alpha, &flags) && (flags & LWA_ALPHA) && alpha < 242) return TRUE;
    }
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    wchar_t type[64];
    GetClassNameW(window, type, 64);
    if (!wcscmp(type, L"Progman") || !wcscmp(type, L"WorkerW") || !wcscmp(type, L"Shell_TrayWnd") ||
        !wcscmp(type, L"Shell_SecondaryTrayWnd")) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(window, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    out->rects[out->count++] = (wp_rect){r.left, r.top, r.right - r.left, r.bottom - r.top};
    return TRUE;
}

static bool windows_covered(void) {
    static wp_rect rects[MAX_RECTS];
    struct { wp_rect *rects; int count; } collected = {rects, 0};
    EnumWindows(collect_window, (LPARAM)&collected);
    for (int i = 0; i < app.region_count; i++) {
        if (!wp_covered(app.regions[i], rects, collected.count)) return false;
    }
    return app.region_count > 0;
}

static bool current_covered(void) {
    double t = now();
    if (t - app.last_scan >= WP_COVERAGE_POLL - 0.05 || (app.scan_at >= 0 && t >= app.scan_at)) {
        app.covered = windows_covered();
        app.last_scan = t;
        app.scan_at = -1;
    }
    return app.covered;
}

static void read_power(void) {
    SYSTEM_POWER_STATUS status;
    if (GetSystemPowerStatus(&status)) {
        app.system.on_battery = status.ACLineStatus == 0;
        app.system.low_power = status.SystemStatusFlag == 1;
    }
}

static bool choose_content(char *path, size_t size, bool *image) {
    *image = false;
    const wp_config *c = &app.config;
    wchar_t wide[WP_PATH_MAX];
    if (c->content == WP_CONTENT_VIDEO || c->content == WP_CONTENT_IMAGE) {
        to_wide(c->path, wide, WP_PATH_MAX);
        if (file_exists(wide)) {
            snprintf(path, size, "%s", c->path);
            *image = c->content == WP_CONTENT_IMAGE;
            return true;
        }
    }
    int index = wp_pick_tier(app.tiers, app.tier_count, app.canvas.w, app.canvas.h, app.rate, app.config.prefs.fit);
    if (index < 0 && app.preview[0]) {
        snprintf(path, size, "%s", app.preview);
        *image = true;
        return true;
    }
    if (index < 0) return false;
    snprintf(path, size, "%s", app.tier_paths[index]);
    return true;
}

static void sync_system_wallpaper(void) {
    if (!app.mpv) return;
    uint64_t key = wp_hash(app.playing) ^ (uint64_t)app.config.prefs.fit;
    if (key == app.synced) return;
    wchar_t folder[MAX_PATH], shot[MAX_PATH];
    wcscpy(folder, app.config_path);
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash) *slash = 0;
    swprintf(shot, MAX_PATH, L"%ls\\event_horizon.jpg", folder);
    char path[WP_PATH_MAX];
    to_utf8(shot, path, sizeof path);
    const char *argv[] = {"screenshot-to-file", path, "video"};
    char command[WP_MPV_COMMAND_MAX];
    wp_mpv_command(command, sizeof command, 0, argv, 3);
    if (!ipc_request(command, 0, NULL, 0)) return;
    for (int i = 0; i < 20 && !file_exists(shot); i++) Sleep(50);
    if (file_exists(shot) && SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, shot, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        app.synced = key;
}

static void schedule(void);

static bool is_refused(const char *name) {
    for (int i = 0; i < app.refused_count; i++) {
        if (strcmp(app.refused[i], name) == 0) return true;
    }
    return false;
}

static void refuse(const char *name) {
    if (app.refused_count < REFUSED_MAX && !is_refused(name))
        snprintf(app.refused[app.refused_count++], MAX_PATH, "%s", name);
}

static void consider_fitted(const char *path) {
    int index = tier_index(path);
    wp_tier tier;
    char name[MAX_PATH];
    if (app.config.content != WP_CONTENT_BUILTIN || index < 0 || !wp_fitted_allowed(&app.system) ||
        !wp_fitted_tier(app.tiers[index], app.canvas.w, app.canvas.h, app.config.prefs.fit, &tier) ||
        wp_tier_name(WP_SCENE, tier, name, sizeof name) == 0) {
        app.fit_name[0] = 0;
        return;
    }
    wchar_t wide[MAX_PATH], destination[MAX_PATH];
    to_wide(name, wide, MAX_PATH);
    swprintf(destination, MAX_PATH, L"%ls\\%ls", app.variants, wide);
    if (app.encoder || strcmp(app.fit_name, name) == 0 || is_refused(name) || file_exists(destination)) return;
    snprintf(app.fit_name, sizeof app.fit_name, "%s", name);
    snprintf(app.fit_source, sizeof app.fit_source, "%s", app.tier_paths[index]);
    app.fit_tier = tier;
    app.fit_due = now() + WP_FIT_DELAY;
}

static VOID CALLBACK encoder_exited(PVOID context, BOOLEAN timed_out) {
    (void)context;
    (void)timed_out;
    PostMessageW(app.control, WM_ENCODED, 0, 0);
}

static void start_encoder(void) {
    wchar_t temp[MAX_PATH], wide[WP_PATH_MAX];
    swprintf(temp, MAX_PATH, L"%ls\\tmp", app.variants);
    CreateDirectoryW(temp, NULL);
    to_wide(app.fit_name, wide, WP_PATH_MAX);
    swprintf(temp, MAX_PATH, L"%ls\\tmp\\%ls", app.variants, wide);
    DeleteFileW(temp);
    char output[WP_PATH_MAX];
    to_utf8(temp, output, sizeof output);
    static wp_mpv_options options;
    wp_mpv_encode_options(&options, app.fit_tier, output);
    static wchar_t line[32768];
    swprintf(line, 32768, L"\"%ls\"", app.mpv_exe);
    for (int i = 0; i < options.count; i++) {
        to_wide(options.items[i], wide, WP_PATH_MAX);
        append_argument(line, 32768, wide);
    }
    append_argument(line, 32768, L"--");
    to_wide(app.fit_source, wide, WP_PATH_MAX);
    append_argument(line, 32768, wide);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process;
    if (!app.mpv_exe[0] || !CreateProcessW(NULL, line, NULL, NULL, FALSE,
                                           CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                                           NULL, NULL, &startup, &process)) {
        refuse(app.fit_name);
        app.fit_name[0] = 0;
        return;
    }
    if (app.job) AssignProcessToJobObject(app.job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    app.encoder = process.hProcess;
    app.encoder_stopped = false;
    snprintf(app.encoding, sizeof app.encoding, "%s", app.fit_name);
    snprintf(app.encoding_source, sizeof app.encoding_source, "%s", app.fit_source);
    app.fit_name[0] = 0;
    if (!RegisterWaitForSingleObject(&app.encoder_wait, app.encoder, encoder_exited, NULL, INFINITE, WT_EXECUTEONLYONCE))
        app.encoder_wait = NULL;
}

static void prune_variants(const wchar_t *keep) {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.mp4", app.variants);
    static wchar_t paths[64][MAX_PATH];
    ULONGLONG times[64];
    int count = 0;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (count >= 64) break;
        swprintf(paths[count], MAX_PATH, L"%ls\\%ls", app.variants, entry.cFileName);
        if (_wcsicmp(paths[count], keep) == 0) continue;
        times[count++] = ((ULONGLONG)entry.ftLastWriteTime.dwHighDateTime << 32) | entry.ftLastWriteTime.dwLowDateTime;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    for (int kept = 1; kept < WP_FITTED_LIMIT && count > 0; kept++) {
        int newest = 0;
        for (int i = 1; i < count; i++) {
            if (times[i] > times[newest]) newest = i;
        }
        times[newest] = times[count - 1];
        wcscpy(paths[newest], paths[count - 1]);
        count--;
    }
    for (int i = 0; i < count; i++) DeleteFileW(paths[i]);
}

static void finish_encoder(void) {
    if (!app.encoder) return;
    if (app.encoder_wait) {
        UnregisterWaitEx(app.encoder_wait, INVALID_HANDLE_VALUE);
        app.encoder_wait = NULL;
    }
    DWORD code = 1;
    GetExitCodeProcess(app.encoder, &code);
    CloseHandle(app.encoder);
    app.encoder = NULL;
    wchar_t name[MAX_PATH], temp[MAX_PATH], destination[MAX_PATH];
    to_wide(app.encoding, name, MAX_PATH);
    swprintf(temp, MAX_PATH, L"%ls\\tmp\\%ls", app.variants, name);
    swprintf(destination, MAX_PATH, L"%ls\\%ls", app.variants, name);
    char produced_path[WP_PATH_MAX];
    to_utf8(temp, produced_path, sizeof produced_path);
    double expected = video_length(app.encoding_source), produced = video_length(produced_path);
    if (code == 0 && expected > 0 && fabs(produced - expected) < 0.01 &&
        MoveFileExW(temp, destination, MOVEFILE_REPLACE_EXISTING)) {
        prune_variants(destination);
        load_tiers();
    } else {
        DeleteFileW(temp);
        if (!app.encoder_stopped) refuse(app.encoding);
    }
    app.encoding[0] = 0;
}

static void stop_encoder(void) {
    if (!app.encoder) return;
    app.encoder_stopped = true;
    TerminateProcess(app.encoder, 1);
    WaitForSingleObject(app.encoder, 2000);
    finish_encoder();
}

static void apply_state(void) {
    char path[WP_PATH_MAX];
    bool image = false;
    bool have = choose_content(path, sizeof path, &image);
    double rate = image ? 1.0 : wp_rate(&app.config.prefs, &app.system);
    app.rate = rate;
    if (app.encoder && !wp_fitted_allowed(&app.system)) stop_encoder();
    bool covered = !wp_frozen(&app.system) && current_covered();
    wp_want want = wp_decide(&app.system, covered);
    if (want == WP_WANT_PLAY && !app.mpv && now() < app.retry_at) want = WP_WANT_RELEASE;
    wp_action action = wp_surface_step(&app.surface, want, rate, now(), WP_RELEASE_DELAY);
    if (action.build && have && !app.mpv) start_mpv(path, image, action.start);
    else if (have && app.mpv && strcmp(path, app.playing) != 0) {
        switch_file(path, image, tier_index(app.playing) >= 0 && tier_index(path) >= 0);
    }
    if (have) consider_fitted(path);
    if (action.play && app.mpv) {
        char value[32];
        snprintf(value, sizeof value, "%.4f", action.rate);
        ipc_set("speed", value);
        ipc_set("pause", "no");
        sync_system_wallpaper();
    }
    if (action.pause || action.release) ipc_set("pause", "yes");
    bool unseen = app.system.display_asleep || app.system.session_inactive;
    if (app.surface.state == WP_RELEASED && app.mpv && unseen) {
        wp_surface_hold(&app.surface, loop_position());
        stop_mpv();
    }
    schedule();
}

static void schedule(void) {
    double t = now();
    double next = -1;
    if (!wp_frozen(&app.system)) {
        next = wp_next_wake(t, next, app.last_scan + WP_COVERAGE_POLL);
        next = wp_next_wake(t, next, app.scan_at);
    }
    next = wp_next_wake(t, next, wp_surface_deadline(&app.surface, WP_RELEASE_DELAY));
    if (!app.mpv && app.retry_at > t) next = wp_next_wake(t, next, app.retry_at);
    if (app.fit_name[0] && !app.encoder) next = wp_next_wake(t, next, app.fit_due);
    KillTimer(app.control, TIMER_WAKE);
    if (next >= 0) SetCoalescableTimer(app.control, TIMER_WAKE, (UINT)((next - t) * 1000.0) + 1, NULL, 1000);
}

static void request_scan(void) {
    if (app.scan_at < 0) app.scan_at = now() + SCAN_DELAY;
    schedule();
}

static void CALLBACK on_window_event(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time) {
    (void)hook;
    (void)event;
    (void)window;
    (void)child;
    (void)thread;
    (void)time;
    if (object == OBJID_WINDOW) request_scan();
}

static bool login_enabled(void) {
    wchar_t value[MAX_PATH];
    DWORD size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", APP,
                        RRF_RT_REG_SZ, NULL, value, &size) == ERROR_SUCCESS;
}

static void toggle_login(void) {
    const wchar_t *key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (login_enabled()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, key, APP);
        return;
    }
    wchar_t exe[MAX_PATH], value[MAX_PATH + 4];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(value, MAX_PATH + 4, L"\"%ls\"", exe);
    RegSetKeyValueW(HKEY_CURRENT_USER, key, APP, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void choose_file(void) {
    wchar_t chosen[MAX_PATH] = {0};
    OPENFILENAMEW open = {0};
    open.lStructSize = sizeof open;
    open.hwndOwner = app.control;
    open.lpstrFilter = L"Videos and images\0*.mp4;*.mov;*.m4v;*.mkv;*.webm;*.jpg;*.jpeg;*.png;*.heic;*.bmp;*.gif;*.webp\0All files\0*.*\0";
    open.lpstrFile = chosen;
    open.nMaxFile = MAX_PATH;
    open.lpstrTitle = L"Choose Background";
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&open)) return;
    to_utf8(chosen, app.config.path, WP_PATH_MAX);
    app.config.content = wp_is_image_name(app.config.path) ? WP_CONTENT_IMAGE : WP_CONTENT_VIDEO;
    save_config();
    apply_state();
}

static void show_speed(HWND dialog, double speed) {
    wchar_t text[32];
    swprintf(text, 32, L"%g", speed);
    SetDlgItemTextW(dialog, 10, text);
    SendDlgItemMessageW(dialog, 11, TBM_SETPOS, TRUE, (LPARAM)lround(wp_slider_position(speed) * SLIDER_STEPS));
}

static INT_PTR CALLBACK speed_proc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    static double before;
    if (message == WM_INITDIALOG) {
        before = app.config.prefs.speed;
        SendDlgItemMessageW(dialog, 11, TBM_SETRANGE, FALSE, MAKELPARAM(0, SLIDER_STEPS));
        SendDlgItemMessageW(dialog, 11, TBM_SETTICFREQ, SLIDER_STEPS / 4, 0);
        show_speed(dialog, before);
        return TRUE;
    }
    if (message == WM_HSCROLL && (HWND)l == GetDlgItem(dialog, 11)) {
        double position = (double)SendDlgItemMessageW(dialog, 11, TBM_GETPOS, 0, 0) / SLIDER_STEPS;
        app.config.prefs.speed = wp_slider_speed(position);
        if (LOWORD(w) == TB_ENDTRACK) show_speed(dialog, app.config.prefs.speed);
        else {
            wchar_t text[32];
            swprintf(text, 32, L"%g", app.config.prefs.speed);
            SetDlgItemTextW(dialog, 10, text);
        }
        apply_state();
        return TRUE;
    }
    if (message != WM_COMMAND || (LOWORD(w) != IDOK && LOWORD(w) != IDCANCEL)) return FALSE;
    wchar_t text[32];
    char typed[64];
    double speed;
    GetDlgItemTextW(dialog, 10, text, 32);
    to_utf8(text, typed, sizeof typed);
    if (LOWORD(w) == IDCANCEL) app.config.prefs.speed = before;
    else if (wp_speed_parse(typed, &speed)) app.config.prefs.speed = wp_speed_clamp(speed);
    EndDialog(dialog, LOWORD(w));
    return TRUE;
}

static void show_menu(void) {
    wp_prefs *p = &app.config.prefs;
    HMENU menu = CreatePopupMenu();
    wchar_t label[64];
    AppendMenuW(menu, MF_STRING, ID_PAUSE, app.system.manual_pause ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_CHOOSE, L"Choose Video or Image…");
    AppendMenuW(menu, MF_STRING | (app.config.content == WP_CONTENT_BUILTIN ? MF_CHECKED : 0), ID_BUILTIN, L"Use Built-in Gargantua");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    char path[WP_PATH_MAX];
    bool image = false;
    bool still = choose_content(path, sizeof path, &image) && image;
    if (still) wcscpy(label, L"Speed: not available for images");
    else swprintf(label, 64, L"Speed: %g×…", p->speed);
    AppendMenuW(menu, MF_STRING | (still ? MF_GRAYED : 0), ID_SPEED, label);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Fit");
    const wchar_t *fits[] = {L"Fill Screen", L"Fit Inside"};
    for (int i = 0; i < 2; i++) AppendMenuW(menu, MF_STRING | ((int)p->fit == i ? MF_CHECKED : 0), (UINT_PTR)(ID_FIT + i), fits[i]);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Battery");
    AppendMenuW(menu, MF_STRING | (p->half_speed_on_battery ? MF_CHECKED : 0), ID_BATTERY, L"Half Speed on Battery");
    AppendMenuW(menu, MF_STRING | (p->half_speed_in_low_power ? MF_CHECKED : 0), ID_LOW_POWER, L"Half Speed with Battery Saver");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (login_enabled() ? MF_CHECKED : 0), ID_LOGIN, L"Open at Login");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit");
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(app.control);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, app.control, NULL);
    PostMessageW(app.control, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void command(UINT id) {
    wp_prefs *p = &app.config.prefs;
    if (id == ID_QUIT) {
        DestroyWindow(app.control);
        return;
    }
    if (id == ID_PAUSE) app.system.manual_pause = !app.system.manual_pause;
    else if (id == ID_CHOOSE) {
        choose_file();
        return;
    }
    else if (id == ID_BUILTIN) {
        app.config.content = WP_CONTENT_BUILTIN;
        app.config.path[0] = 0;
    }
    else if (id == ID_LOGIN) toggle_login();
    else if (id == ID_BATTERY) p->half_speed_on_battery = !p->half_speed_on_battery;
    else if (id == ID_LOW_POWER) p->half_speed_in_low_power = !p->half_speed_in_low_power;
    else if (id == ID_SPEED) DialogBoxW(app.instance, MAKEINTRESOURCEW(2), app.control, speed_proc);
    else if (id >= ID_FIT && id < ID_FIT + 2) {
        p->fit = (wp_fit)(id - ID_FIT);
        stop_mpv();
        wp_surface_init(&app.surface);
    }
    save_config();
    app.scan_at = now();
    apply_state();
}

static LRESULT CALLBACK control_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    static UINT taskbar_created;
    switch (message) {
    case WM_CREATE:
        taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) show_menu();
        return 0;
    case WM_COMMAND:
        command(LOWORD(w));
        return 0;
    case WM_TIMER:
        if (w == TIMER_WAKE) {
            double t = now();
            if (!app.mpv || WaitForSingleObject(app.mpv, 0) == WAIT_OBJECT_0) {
                if (app.mpv && now() - app.mpv_started < 3.0) app.retry_at = now() + RETRY_DELAY;
                if (app.mpv) CloseHandle(app.mpv);
                app.mpv = NULL;
                app.playing[0] = 0;
                if (app.surface.state != WP_RELEASED) wp_surface_init(&app.surface);
            }
            if (app.encoder && WaitForSingleObject(app.encoder, 0) == WAIT_OBJECT_0) finish_encoder();
            if (app.fit_name[0] && !app.encoder && t >= app.fit_due && wp_fitted_allowed(&app.system)) start_encoder();
            apply_state();
        }
        return 0;
    case WM_ENCODED:
        if (app.encoder && WaitForSingleObject(app.encoder, 0) == WAIT_OBJECT_0) {
            finish_encoder();
            apply_state();
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (message == WM_SETTINGCHANGE && w != SPI_SETWORKAREA) break;
        measure_screens();
        request_scan();
        apply_state();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMPOWERSTATUSCHANGE) read_power();
        if (w == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *setting = (POWERBROADCAST_SETTING *)l;
            if (IsEqualGUID(&setting->PowerSetting, &GUID_CONSOLE_DISPLAY_STATE))
                app.system.display_asleep = *(DWORD *)setting->Data == 0;
            else read_power();
        }
        if (w == PBT_APMSUSPEND) app.system.display_asleep = true;
        if (w == PBT_APMRESUMEAUTOMATIC) app.system.display_asleep = false;
        apply_state();
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK || w == WTS_CONSOLE_DISCONNECT) app.system.locked = true;
        if (w == WTS_SESSION_UNLOCK || w == WTS_CONSOLE_CONNECT) app.system.locked = false;
        apply_state();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (message == taskbar_created && taskbar_created) {
            Shell_NotifyIconW(NIM_ADD, &app.tray);
            return 0;
        }
    }
    return DefWindowProcW(window, message, w, l);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR line, int show) {
    (void)previous;
    (void)line;
    (void)show;
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\event_horizon_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    app.instance = instance;
    INITCOMMONCONTROLSEX controls = {(DWORD)sizeof controls, ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    resolve_paths();
    load_config();
    if (!app.mpv_exe[0]) {
        MessageBoxW(NULL, L"mpv.exe was not found. Place it next to this program or add it to PATH.", NAME, MB_ICONERROR);
        return 1;
    }
    if (app.tier_count == 0 && !app.preview[0] && app.config.content == WP_CONTENT_BUILTIN) {
        MessageBoxW(NULL, L"No background videos were found next to this program.", NAME, MB_ICONERROR);
        return 1;
    }
    app.job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (app.job) SetInformationJobObject(app.job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    WNDCLASSW type = {0};
    type.lpfnWndProc = control_proc;
    type.hInstance = instance;
    type.lpszClassName = L"event_horizon_control";
    RegisterClassW(&type);
    app.control = CreateWindowExW(0, L"event_horizon_control", APP, 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!attach_to_desktop()) {
        MessageBoxW(NULL, L"The desktop window could not be found.", NAME, MB_ICONERROR);
        return 1;
    }
    WTSRegisterSessionNotification(app.control, NOTIFY_FOR_THIS_SESSION);
    RegisterPowerSettingNotification(app.control, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    RegisterPowerSettingNotification(app.control, &power_saving_status, DEVICE_NOTIFY_WINDOW_HANDLE);
    RegisterPowerSettingNotification(app.control, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    DWORD events[][2] = {{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                         {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
                         {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND}};
    for (int i = 0; i < 3; i++)
        app.hooks[i] = SetWinEventHook(events[i][0], events[i][1], NULL, on_window_event, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    app.tray.cbSize = sizeof app.tray;
    app.tray.hWnd = app.control;
    app.tray.uID = 1;
    app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.tray.uCallbackMessage = WM_TRAY;
    wchar_t icon[MAX_PATH];
    swprintf(icon, MAX_PATH, L"%ls\\icon.ico", app.exe_dir);
    app.tray.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!app.tray.hIcon) app.tray.hIcon = (HICON)LoadImageW(NULL, icon, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!app.tray.hIcon) app.tray.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(app.tray.szTip, NAME);
    Shell_NotifyIconW(NIM_ADD, &app.tray);

    wp_surface_init(&app.surface);
    app.rate = -1;
    app.last_scan = -1e9;
    app.scan_at = -1;
    read_power();
    apply_state();

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    for (int i = 0; i < 3; i++) if (app.hooks[i]) UnhookWinEvent(app.hooks[i]);
    stop_encoder();
    stop_mpv();
    Shell_NotifyIconW(NIM_DELETE, &app.tray);
    if (app.host) DestroyWindow(app.host);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, NULL, SPIF_SENDCHANGE);
    if (app.job) CloseHandle(app.job);
    ReleaseMutex(single);
    CoUninitialize();
    return 0;
}
