/* g13map: maps Logitech G13 keys and thumbstick to keyboard input on Windows.
 *
 * The G13 appears as a vendor-defined HID device. It sends 8-byte reports, and
 * only when something changes:
 *
 *   byte 0     report ID (always 1)
 *   byte 1, 2  stick X, Y (0-255)
 *   byte 3-7   key bitmask, little-endian (see KEY_NAMES for the layout)
 *
 * Bit 23 (backlight state), bit 38 and bit 39 (toggles constantly) are state
 * bits rather than keys, so they are never bound.
 */
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#ifndef G13MAP_NO_MAIN
#include <ViGEm/Client.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define REPORT_LEN 8
#define REPORT_ID 1
#define KEY_BITS 40
#define MAX_COMBO 4
#define EXTENDED 0x100
#define CODE_COUNT 0x200
#define RECONNECT_MS 2000

/* Index is the bit number in the key bitmask. */
static const wchar_t *const KEY_NAMES[KEY_BITS] = {
    L"G1",  L"G2",  L"G3",  L"G4",  L"G5",  L"G6",  L"G7",  L"G8",
    L"G9",  L"G10", L"G11", L"G12", L"G13", L"G14", L"G15", L"G16",
    L"G17", L"G18", L"G19", L"G20", L"G21", L"G22", NULL,   NULL,
    L"BD",  L"L1",  L"L2",  L"L3",  L"L4",  L"M1",  L"M2",  L"M3",
    L"MR",  L"LEFT", L"DOWN", L"STICK", NULL, L"LIGHT", NULL, NULL,
};

typedef struct {
    const wchar_t *name;
    WORD code; /* set-1 scan code, optionally with EXTENDED */
} KeyCode;

/* Scan codes identify physical key positions, so the symbols on the keys can
 * differ on non-US layouts. */
static const KeyCode KEY_CODES[] = {
    {L"ESC", 0x01}, {L"1", 0x02}, {L"2", 0x03}, {L"3", 0x04}, {L"4", 0x05},
    {L"5", 0x06}, {L"6", 0x07}, {L"7", 0x08}, {L"8", 0x09}, {L"9", 0x0A},
    {L"0", 0x0B}, {L"-", 0x0C}, {L"=", 0x0D}, {L"BACKSPACE", 0x0E}, {L"TAB", 0x0F},
    {L"Q", 0x10}, {L"W", 0x11}, {L"E", 0x12}, {L"R", 0x13}, {L"T", 0x14},
    {L"Y", 0x15}, {L"U", 0x16}, {L"I", 0x17}, {L"O", 0x18}, {L"P", 0x19},
    {L"[", 0x1A}, {L"]", 0x1B}, {L"ENTER", 0x1C}, {L"CTRL", 0x1D}, {L"A", 0x1E},
    {L"S", 0x1F}, {L"D", 0x20}, {L"F", 0x21}, {L"G", 0x22}, {L"H", 0x23},
    {L"J", 0x24}, {L"K", 0x25}, {L"L", 0x26}, {L";", 0x27}, {L"'", 0x28},
    {L"`", 0x29}, {L"SHIFT", 0x2A}, {L"\\", 0x2B}, {L"Z", 0x2C}, {L"X", 0x2D},
    {L"C", 0x2E}, {L"V", 0x2F}, {L"B", 0x30}, {L"N", 0x31}, {L"M", 0x32},
    {L",", 0x33}, {L".", 0x34}, {L"/", 0x35}, {L"RSHIFT", 0x36}, {L"NUM*", 0x37},
    {L"ALT", 0x38}, {L"SPACE", 0x39}, {L"CAPSLOCK", 0x3A},
    {L"F1", 0x3B}, {L"F2", 0x3C}, {L"F3", 0x3D}, {L"F4", 0x3E}, {L"F5", 0x3F},
    {L"F6", 0x40}, {L"F7", 0x41}, {L"F8", 0x42}, {L"F9", 0x43}, {L"F10", 0x44},
    {L"F11", 0x57}, {L"F12", 0x58},
    {L"NUM7", 0x47}, {L"NUM8", 0x48}, {L"NUM9", 0x49}, {L"NUM-", 0x4A},
    {L"NUM4", 0x4B}, {L"NUM5", 0x4C}, {L"NUM6", 0x4D}, {L"NUMPLUS", 0x4E},
    {L"NUM1", 0x4F}, {L"NUM2", 0x50}, {L"NUM3", 0x51}, {L"NUM0", 0x52}, {L"NUM.", 0x53},
    {L"RCTRL", EXTENDED | 0x1D}, {L"RALT", EXTENDED | 0x38},
    {L"HOME", EXTENDED | 0x47}, {L"UP", EXTENDED | 0x48}, {L"PGUP", EXTENDED | 0x49},
    {L"LEFT", EXTENDED | 0x4B}, {L"RIGHT", EXTENDED | 0x4D}, {L"END", EXTENDED | 0x4F},
    {L"DOWN", EXTENDED | 0x50}, {L"PGDN", EXTENDED | 0x51}, {L"INSERT", EXTENDED | 0x52},
    {L"DELETE", EXTENDED | 0x53},
};

typedef struct {
    WORD codes[MAX_COMBO];
    int count;
} Binding;

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_COUNT };

typedef struct {
    Binding keys[KEY_BITS];
    Binding stick[DIR_COUNT];
    int center_x, center_y;
    int press_threshold, release_threshold;
    int invert_y;
    int analog; /* stick drives a virtual Xbox 360 pad instead of keys */
    int deadzone;
} Config;

typedef void (*KeySink)(WORD code, int down);
typedef void (*PadSink)(SHORT x, SHORT y);

static Config g_cfg;
static KeySink g_sink;
static PadSink g_pad_sink;
static SHORT g_pad_x, g_pad_y;
static CRITICAL_SECTION g_lock;
static unsigned long long g_prev_bits;
static int g_dir_active[DIR_COUNT];
static int g_refs[CODE_COUNT]; /* lets two inputs share a key, e.g. SHIFT */

/* ---- key output ---- */

static void sink_sendinput(WORD code, int down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)(code & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (code & EXTENDED) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
}

static void sink_print(WORD code, int down)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(KEY_CODES); i++) {
        if (KEY_CODES[i].code == code) {
            wprintf(L"%ls %ls\n", down ? L"down" : L"up  ", KEY_CODES[i].name);
            return;
        }
    }
}

static void binding_press(const Binding *b)
{
    int i;
    for (i = 0; i < b->count; i++)
        if (g_refs[b->codes[i]]++ == 0) g_sink(b->codes[i], 1);
}

static void binding_release(const Binding *b)
{
    int i;
    for (i = b->count - 1; i >= 0; i--)
        if (--g_refs[b->codes[i]] == 0) g_sink(b->codes[i], 0);
}

/* ---- report decoding ---- */

static void update_direction(int dir, int amount)
{
    int threshold = g_dir_active[dir] ? g_cfg.release_threshold : g_cfg.press_threshold;
    int active = amount >= threshold;
    if (active == g_dir_active[dir]) return;
    g_dir_active[dir] = active;
    if (active) binding_press(&g_cfg.stick[dir]);
    else binding_release(&g_cfg.stick[dir]);
}

/* Converts raw stick position to Xbox axes, where up is positive Y. Each side
 * of center scales separately because the center isn't at the midpoint. The
 * dead zone is radial and the output is rescaled so movement starts near zero. */
static void stick_to_axes(int raw_x, int raw_y, SHORT *x, SHORT *y)
{
    int dx = raw_x - g_cfg.center_x;
    int dy = raw_y - g_cfg.center_y;
    double nx = dx / (double)(dx < 0 ? g_cfg.center_x : 255 - g_cfg.center_x);
    double ny = -dy / (double)(dy < 0 ? g_cfg.center_y : 255 - g_cfg.center_y);
    double magnitude = sqrt(nx * nx + ny * ny);
    double dz = g_cfg.deadzone / 127.0;
    double scaled;

    if (g_cfg.invert_y) ny = -ny;
    if (magnitude <= dz) {
        *x = 0;
        *y = 0;
        return;
    }
    scaled = (magnitude - dz) / (1.0 - dz);
    if (scaled > 1.0) scaled = 1.0;
    *x = (SHORT)lround(nx / magnitude * scaled * 32767.0);
    *y = (SHORT)lround(ny / magnitude * scaled * 32767.0);
}

static void update_pad(SHORT x, SHORT y)
{
    if (x == g_pad_x && y == g_pad_y) return;
    g_pad_x = x;
    g_pad_y = y;
    g_pad_sink(x, y);
}

static void process_report(const BYTE *r)
{
    unsigned long long bits, changed;
    int dx, dy, bit;

    if (r[0] != REPORT_ID) return;

    EnterCriticalSection(&g_lock);

    bits = 0;
    for (bit = 0; bit < 5; bit++) bits |= (unsigned long long)r[3 + bit] << (8 * bit);
    changed = bits ^ g_prev_bits;
    for (bit = 0; bit < KEY_BITS; bit++) {
        if (!(changed >> bit & 1)) continue;
        if (bits >> bit & 1) binding_press(&g_cfg.keys[bit]);
        else binding_release(&g_cfg.keys[bit]);
    }
    g_prev_bits = bits;

    if (g_cfg.analog) {
        SHORT x, y;
        stick_to_axes(r[1], r[2], &x, &y);
        update_pad(x, y);
    } else {
        dx = r[1] - g_cfg.center_x;
        dy = r[2] - g_cfg.center_y;
        if (g_cfg.invert_y) dy = -dy;
        update_direction(DIR_UP, -dy);
        update_direction(DIR_DOWN, dy);
        update_direction(DIR_LEFT, -dx);
        update_direction(DIR_RIGHT, dx);
    }

    LeaveCriticalSection(&g_lock);
}

/* Releases everything held, so a key can't stay stuck after a disconnect or exit. */
static void release_all(void)
{
    int i;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < KEY_BITS; i++)
        if (g_prev_bits >> i & 1) binding_release(&g_cfg.keys[i]);
    g_prev_bits = 0;
    for (i = 0; i < DIR_COUNT; i++) {
        if (g_dir_active[i]) binding_release(&g_cfg.stick[i]);
        g_dir_active[i] = 0;
    }
    if (g_cfg.analog) update_pad(0, 0);
    LeaveCriticalSection(&g_lock);
}

/* ---- config ---- */

static int lookup_code(const wchar_t *name, WORD *code)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(KEY_CODES); i++) {
        if (_wcsicmp(KEY_CODES[i].name, name) == 0) {
            *code = KEY_CODES[i].code;
            return 1;
        }
    }
    return 0;
}

/* Parses "Q", "SHIFT+1" or an empty string (unbound). "+" only separates keys,
 * so the numpad plus key is named NUMPLUS. */
static int parse_binding(const wchar_t *text, Binding *b, const wchar_t *what)
{
    wchar_t buf[128];
    wchar_t *ctx = NULL, *tok;

    b->count = 0;
    wcsncpy_s(buf, ARRAYSIZE(buf), text, _TRUNCATE);
    for (tok = wcstok_s(buf, L"+ \t", &ctx); tok; tok = wcstok_s(NULL, L"+ \t", &ctx)) {
        if (b->count == MAX_COMBO) {
            fwprintf(stderr, L"Config: %ls has more than %d keys\n", what, MAX_COMBO);
            return 0;
        }
        if (!lookup_code(tok, &b->codes[b->count])) {
            fwprintf(stderr, L"Config: %ls uses unknown key name '%ls'\n", what, tok);
            return 0;
        }
        b->count++;
    }
    return 1;
}

static int load_config(const wchar_t *path, Config *cfg)
{
    static const wchar_t *const DIR_NAMES[DIR_COUNT] = {L"up", L"down", L"left", L"right"};
    wchar_t text[128];
    int i;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Config file not found: %ls\n", path);
        return 0;
    }
    memset(cfg, 0, sizeof *cfg);
    for (i = 0; i < KEY_BITS; i++) {
        if (!KEY_NAMES[i]) continue;
        GetPrivateProfileStringW(L"keys", KEY_NAMES[i], L"", text, ARRAYSIZE(text), path);
        if (!parse_binding(text, &cfg->keys[i], KEY_NAMES[i])) return 0;
    }
    for (i = 0; i < DIR_COUNT; i++) {
        GetPrivateProfileStringW(L"stick", DIR_NAMES[i], L"", text, ARRAYSIZE(text), path);
        if (!parse_binding(text, &cfg->stick[i], DIR_NAMES[i])) return 0;
    }
    cfg->center_x = GetPrivateProfileIntW(L"stick", L"center_x", 125, path);
    cfg->center_y = GetPrivateProfileIntW(L"stick", L"center_y", 130, path);
    cfg->press_threshold = GetPrivateProfileIntW(L"stick", L"press_threshold", 50, path);
    cfg->release_threshold = GetPrivateProfileIntW(L"stick", L"release_threshold", 40, path);
    cfg->invert_y = GetPrivateProfileIntW(L"stick", L"invert_y", 0, path);
    cfg->deadzone = GetPrivateProfileIntW(L"stick", L"deadzone", 12, path);
    if (cfg->release_threshold <= 0 || cfg->release_threshold > cfg->press_threshold) {
        fwprintf(stderr, L"Config: need 0 < release_threshold <= press_threshold\n");
        return 0;
    }
    if (cfg->center_x < 1 || cfg->center_x > 254 || cfg->center_y < 1 || cfg->center_y > 254) {
        fwprintf(stderr, L"Config: center_x and center_y must be 1-254\n");
        return 0;
    }
    if (cfg->deadzone < 0 || cfg->deadzone > 100) {
        fwprintf(stderr, L"Config: deadzone must be 0-100\n");
        return 0;
    }
    GetPrivateProfileStringW(L"stick", L"mode", L"wasd", text, ARRAYSIZE(text), path);
    if (_wcsicmp(text, L"analog") == 0) {
        cfg->analog = 1;
    } else if (_wcsicmp(text, L"wasd") != 0) {
        fwprintf(stderr, L"Config: mode must be wasd or analog, not '%ls'\n", text);
        return 0;
    }
    return 1;
}

/* ---- device ---- */

static HANDLE open_g13(void)
{
    GUID guid;
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA iface;
    HANDLE result = INVALID_HANDLE_VALUE;
    DWORD i;

    HidD_GetHidGuid(&guid);
    devs = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return result;

    iface.cbSize = sizeof iface;
    for (i = 0; result == INVALID_HANDLE_VALUE &&
                SetupDiEnumDeviceInterfaces(devs, NULL, &guid, i, &iface); i++) {
        DWORD need = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;

        SetupDiGetDeviceInterfaceDetailW(devs, &iface, NULL, 0, &need, NULL);
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!detail) break;
        detail->cbSize = sizeof *detail;
        if (SetupDiGetDeviceInterfaceDetailW(devs, &iface, detail, need, NULL, NULL)) {
            _wcslwr_s(detail->DevicePath, wcslen(detail->DevicePath) + 1);
            if (wcsstr(detail->DevicePath, L"vid_046d&pid_c21c")) {
                result = CreateFileW(detail->DevicePath, GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                     OPEN_EXISTING, 0, NULL);
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return result;
}

static BOOL WINAPI on_console_event(DWORD event)
{
    (void)event;
    release_all();
    return FALSE; /* let Windows end the process as usual */
}

#ifndef G13MAP_NO_MAIN
static PVIGEM_CLIENT g_vigem;
static PVIGEM_TARGET g_pad;

static void pad_sink_vigem(SHORT x, SHORT y)
{
    XUSB_REPORT report;
    memset(&report, 0, sizeof report);
    report.sThumbLX = x;
    report.sThumbLY = y;
    vigem_target_x360_update(g_vigem, g_pad, report);
}

static void pad_sink_print(SHORT x, SHORT y)
{
    wprintf(L"stick x=%6d y=%6d\n", x, y);
}

static int open_virtual_pad(void)
{
    VIGEM_ERROR err;

    g_vigem = vigem_alloc();
    if (!g_vigem) {
        fwprintf(stderr, L"Out of memory creating the ViGEm client.\n");
        return 0;
    }
    err = vigem_connect(g_vigem);
    if (!VIGEM_SUCCESS(err)) {
        fwprintf(stderr, L"Can't reach the ViGEmBus driver (error 0x%08X). Install it with\n"
                         L"'winget install ViGEm.ViGEmBus', or set mode=wasd in the config.\n", err);
        return 0;
    }
    g_pad = vigem_target_x360_alloc();
    err = vigem_target_add(g_vigem, g_pad);
    if (!VIGEM_SUCCESS(err)) {
        fwprintf(stderr, L"Can't create the virtual controller (error 0x%08X).\n", err);
        return 0;
    }
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t config_path[MAX_PATH];
    wchar_t *slash;
    int dry_run = 0, announced_wait = 0, i;
    HANDLE single_instance;

    wcscpy_s(config_path, MAX_PATH, L"");
    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--dry-run") == 0) {
            dry_run = 1;
        } else if (GetFullPathNameW(argv[i], MAX_PATH, config_path, NULL) == 0) {
            fwprintf(stderr, L"Bad config path: %ls\n", argv[i]);
            return 2;
        }
    }
    if (!config_path[0]) {
        GetModuleFileNameW(NULL, config_path, MAX_PATH);
        slash = wcsrchr(config_path, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - config_path), L"g13.ini");
    }
    if (!load_config(config_path, &g_cfg)) return 2;

    single_instance = CreateMutexW(NULL, FALSE, L"Local\\g13map");
    if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        fwprintf(stderr, L"g13map is already running.\n");
        return 3;
    }

    InitializeCriticalSection(&g_lock);
    g_sink = dry_run ? sink_print : sink_sendinput;
    if (g_cfg.analog) {
        if (dry_run) g_pad_sink = pad_sink_print;
        else if (open_virtual_pad()) g_pad_sink = pad_sink_vigem;
        else return 4;
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);

    wprintf(L"Config: %ls\nStick: %ls\n%ls", config_path,
            g_cfg.analog ? L"analog (virtual Xbox 360 controller)" : L"keys",
            dry_run ? L"Dry run: printing events instead of sending them.\n" : L"");
    wprintf(L"Press Ctrl+C to quit.\n");
    fflush(stdout);

    for (;;) {
        BYTE report[REPORT_LEN];
        DWORD got;
        HANDLE device = open_g13();

        if (device == INVALID_HANDLE_VALUE) {
            if (!announced_wait) {
                wprintf(L"Waiting for G13...\n");
                fflush(stdout);
                announced_wait = 1;
            }
            Sleep(RECONNECT_MS);
            continue;
        }
        announced_wait = 0;
        wprintf(L"G13 connected.\n");
        fflush(stdout);

        while (ReadFile(device, report, REPORT_LEN, &got, NULL)) {
            if (got == REPORT_LEN) {
                process_report(report);
                if (dry_run) fflush(stdout);
            }
        }

        release_all();
        CloseHandle(device);
        wprintf(L"G13 disconnected.\n");
        fflush(stdout);
    }
}
#endif
