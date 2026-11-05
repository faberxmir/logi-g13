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
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#ifndef G13MAP_NO_MAIN
#include <shellapi.h>
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
#define MR_BIT 32
#define NO_TARGET (-1)
#define STICK_TARGET_BASE KEY_BITS /* record targets: key bits, then stick directions */
#define RECORD_TIMEOUT_MS 30000
#define RECORD_PULSE_MS 1000

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

static const wchar_t *const DIR_NAMES[DIR_COUNT] = {L"up", L"down", L"left", L"right"};

typedef struct {
    Binding keys[KEY_BITS];
    Binding stick[DIR_COUNT];
    int center_x, center_y;
    int press_threshold, release_threshold;
    int invert_y;
    int analog; /* stick drives a virtual Xbox 360 pad instead of keys */
    int deadzone;
    int show_screen;
    int has_idle_color;
    BYTE idle_color[3];
} Config;

typedef void (*KeySink)(WORD code, int down);
typedef void (*PadSink)(SHORT x, SHORT y);
typedef void (*RecordModeHandler)(int on);
typedef void (*RecordedHandler)(int target, const Binding *binding);

static Config g_cfg;
static KeySink g_sink;
static PadSink g_pad_sink;
static SHORT g_pad_x, g_pad_y;
static CRITICAL_SECTION g_lock;
static unsigned long long g_prev_bits;
static unsigned long long g_pressed_bits; /* keys whose binding is currently held down */
static int g_dir_active[DIR_COUNT];
static int g_refs[CODE_COUNT]; /* lets two inputs share a key, e.g. SHIFT */

static int g_record_enabled;
static int g_recording;
static int g_record_target = NO_TARGET;
static WORD g_record_mods[MAX_COMBO - 1];
static int g_record_mod_count;
static BYTE g_record_swallow_up[CODE_COUNT]; /* keys whose release must also be hidden */
static RecordModeHandler g_on_record_mode;
static RecordedHandler g_on_recorded;
static ULONGLONG g_record_last_touch;
static ULONGLONG (WINAPI *g_clock)(void) = GetTickCount64;

/* ---- key output ---- */

static const wchar_t *code_name(WORD code)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(KEY_CODES); i++)
        if (KEY_CODES[i].code == code) return KEY_CODES[i].name;
    return NULL;
}

static void sink_sendinput(WORD code, int down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)(code & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (code & EXTENDED) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(1, &in, sizeof in) != 1)
        fwprintf(stderr, L"SendInput failed, error %lu\n", GetLastError());
}

static void sink_print(WORD code, int down)
{
    const wchar_t *name = code_name(code);
    if (name) wprintf(L"%ls %ls\n", down ? L"down" : L"up  ", name);
}

static void sink_send_and_print(WORD code, int down)
{
    sink_print(code, down);
    sink_sendinput(code, down);
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

/* ---- record mode ---- */

/* The keys that can be given a new binding: G1-G22 and the three stick buttons. */
static int is_recordable_key(int bit)
{
    return bit <= 21 || (bit >= 33 && bit <= 35);
}

static void record_clear_target(void)
{
    g_record_target = NO_TARGET;
    g_record_mod_count = 0;
}

static void record_touch(void)
{
    g_record_last_touch = g_clock();
}

/* Works out the record mode light: steady dark green, or rolling between dark
 * and light green while a G13 key is waiting for a keyboard key. */
static void record_color(ULONGLONG now, int waiting, BYTE *rgb)
{
    static const BYTE dark[3] = {0, 60, 0}, light[3] = {140, 255, 140};
    int i, phase = (int)(now % RECORD_PULSE_MS);
    int level = waiting ? (phase < RECORD_PULSE_MS / 2 ? phase : RECORD_PULSE_MS - phase) : 0;

    for (i = 0; i < 3; i++)
        rgb[i] = (BYTE)(dark[i] + (light[i] - dark[i]) * level / (RECORD_PULSE_MS / 2));
}

/* The caller holds g_lock. */
static void record_start(void)
{
    int i;
    for (i = 0; i < KEY_BITS; i++) {
        if (g_pressed_bits >> i & 1) binding_release(&g_cfg.keys[i]);
    }
    g_pressed_bits = 0;
    for (i = 0; i < DIR_COUNT; i++) {
        if (g_dir_active[i]) binding_release(&g_cfg.stick[i]);
        g_dir_active[i] = 0;
    }
    g_recording = 1;
    record_clear_target();
    record_touch();
    if (g_on_record_mode) g_on_record_mode(1);
}

/* The caller holds g_lock. */
static void record_stop(void)
{
    int i;
    if (!g_recording) return;
    g_recording = 0;
    record_clear_target();
    /* A stick still held presses its key on the next report, like a fresh push. */
    for (i = 0; i < DIR_COUNT; i++) g_dir_active[i] = 0;
    if (g_on_record_mode) g_on_record_mode(0);
}

/* Leaves record mode if nothing has been touched for a while. Normal typing
 * doesn't count, as it isn't aimed at the G13. */
static void record_check_timeout(void)
{
    EnterCriticalSection(&g_lock);
    if (g_recording && g_clock() - g_record_last_touch >= RECORD_TIMEOUT_MS) record_stop();
    LeaveCriticalSection(&g_lock);
}

static int is_modifier(WORD code)
{
    return code == 0x1D || code == 0x2A || code == 0x38 || code == 0x36 ||
           code == (EXTENDED | 0x1D) || code == (EXTENDED | 0x38);
}

/* Converts a keyboard hook's scan code and extended flag to a KEY_CODES code, or
 * 0 if it has none. Windows marks the fake left Ctrl sent with AltGr by adding
 * 0x200 to its scan code, and flags right Shift as extended. Extended keys with
 * no entry of their own, such as numpad Enter, fall back to the plain key. */
static WORD hook_code(DWORD scan, int extended)
{
    WORD code;
    if (scan == 0 || scan > 0xFF) return 0;
    if (scan == 0x36) extended = 0;
    code = (WORD)(scan | (extended ? EXTENDED : 0));
    if (code_name(code)) return code;
    code = (WORD)scan;
    return code_name(code) ? code : 0;
}

/* The caller holds g_lock. */
static void record_commit(WORD key, int *target, Binding *binding)
{
    Binding *dest;
    int i;

    binding->count = 0;
    for (i = 0; i < g_record_mod_count; i++) binding->codes[binding->count++] = g_record_mods[i];
    if (key) binding->codes[binding->count++] = key;

    *target = g_record_target;
    dest = *target >= STICK_TARGET_BASE ? &g_cfg.stick[*target - STICK_TARGET_BASE]
                                        : &g_cfg.keys[*target];
    *dest = *binding;
    record_clear_target();
}

/* Handles a physical keyboard event in record mode. Returns nonzero if the key
 * should be hidden from other programs. */
static int record_key_event(WORD code, int down)
{
    int swallow = 0, committed = 0, target = NO_TARGET, i, known = 0;
    Binding binding;

    binding.count = 0;
    EnterCriticalSection(&g_lock);
    if (down) {
        if (g_record_swallow_up[code]) {
            swallow = 1; /* auto-repeat of a key that's already been captured */
        } else if (g_recording && g_record_target != NO_TARGET) {
            swallow = 1;
            g_record_swallow_up[code] = 1;
            record_touch();
            if (!is_modifier(code)) {
                record_commit(code, &target, &binding);
                committed = 1;
            } else {
                for (i = 0; i < g_record_mod_count; i++) known |= g_record_mods[i] == code;
                if (!known && g_record_mod_count < MAX_COMBO - 1)
                    g_record_mods[g_record_mod_count++] = code;
            }
        }
    } else {
        swallow = g_record_swallow_up[code];
        g_record_swallow_up[code] = 0;
        /* A modifier released with no other key pressed is the binding itself. */
        if (g_recording && g_record_target != NO_TARGET && g_record_mod_count > 0 &&
            is_modifier(code)) {
            record_touch();
            record_commit(0, &target, &binding);
            committed = 1;
        }
    }
    LeaveCriticalSection(&g_lock);

    if (committed && g_on_recorded) g_on_recorded(target, &binding);
    return swallow;
}

/* Writes a binding as config text, such as "SHIFT+1". */
static void binding_to_text(const Binding *b, wchar_t *text, size_t count)
{
    int i;
    text[0] = L'\0';
    for (i = 0; i < b->count; i++) {
        const wchar_t *name = code_name(b->codes[i]);
        if (!name) continue;
        if (i > 0) wcscat_s(text, count, L"+");
        wcscat_s(text, count, name);
    }
}

/* ---- report decoding ---- */

static void update_direction(int dir, int amount)
{
    int threshold = g_dir_active[dir] ? g_cfg.release_threshold : g_cfg.press_threshold;
    int active = amount >= threshold;
    if (active == g_dir_active[dir]) return;
    g_dir_active[dir] = active;
    if (g_recording) {
        if (active) {
            record_touch();
            record_clear_target();
            g_record_target = STICK_TARGET_BASE + dir;
        }
        return;
    }
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
        int down = (int)(bits >> bit & 1);
        if (!(changed >> bit & 1)) continue;
        if (bit == MR_BIT) {
            if (down && g_record_enabled) {
                if (g_recording) record_stop();
                else record_start();
            }
        } else if (down) {
            if (g_recording) {
                record_touch();
                if (is_recordable_key(bit)) {
                    record_clear_target();
                    g_record_target = bit;
                }
            } else {
                binding_press(&g_cfg.keys[bit]);
                g_pressed_bits |= 1ULL << bit;
            }
        } else if (g_pressed_bits >> bit & 1) {
            binding_release(&g_cfg.keys[bit]);
            g_pressed_bits &= ~(1ULL << bit);
        }
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
    record_stop();
    for (i = 0; i < KEY_BITS; i++)
        if (g_pressed_bits >> i & 1) binding_release(&g_cfg.keys[i]);
    g_pressed_bits = 0;
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

/* Parses "255,110,0". */
static int parse_color(const wchar_t *text, BYTE *rgb)
{
    int r, g, b;
    wchar_t extra;

    if (swscanf_s(text, L"%d , %d , %d %lc", &r, &g, &b, &extra, 1) != 3) return 0;
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return 0;
    rgb[0] = (BYTE)r;
    rgb[1] = (BYTE)g;
    rgb[2] = (BYTE)b;
    return 1;
}

static int load_config(const wchar_t *path, Config *cfg)
{
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
    cfg->keys[MR_BIT].count = 0; /* MR always toggles record mode */
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
    cfg->show_screen = GetPrivateProfileIntW(L"screen", L"show", 0, path);
    GetPrivateProfileStringW(L"screen", L"color", L"", text, ARRAYSIZE(text), path);
    if (text[0]) {
        if (!parse_color(text, cfg->idle_color)) {
            fwprintf(stderr, L"Config: color must be red,green,blue from 0-255, not '%ls'\n", text);
            return 0;
        }
        cfg->has_idle_color = 1;
    }
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

static HANDLE open_g13(DWORD access)
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
                result = CreateFileW(detail->DevicePath, access,
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
    if (!g_pad) return;
    memset(&report, 0, sizeof report);
    report.sThumbLX = x;
    report.sThumbLY = y;
    vigem_target_x360_update(g_vigem, g_pad, report);
}

static void pad_sink_print(SHORT x, SHORT y)
{
    wprintf(L"stick x=%6d y=%6d\n", x, y);
}

static int connect_vigem(void)
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
    return 1;
}

/* The virtual controller exists only while the G13 is connected, so other games
 * don't see an Xbox controller the rest of the time. */
static int add_virtual_pad(void)
{
    VIGEM_ERROR err;
    PVIGEM_TARGET pad = vigem_target_x360_alloc();

    err = vigem_target_add(g_vigem, pad);
    if (!VIGEM_SUCCESS(err)) {
        fwprintf(stderr, L"Can't create the virtual controller (error 0x%08X).\n", err);
        vigem_target_free(pad);
        return 0;
    }
    EnterCriticalSection(&g_lock);
    g_pad = pad;
    /* A new controller doesn't start at neutral: XInput reads a small stick
     * offset (-3356, -1869) until the first update, which could move a
     * character. The driver ignores an all-zero first update, so nudge first. */
    pad_sink_vigem(1, 1);
    pad_sink_vigem(0, 0);
    LeaveCriticalSection(&g_lock);
    return 1;
}

static void remove_virtual_pad(void)
{
    EnterCriticalSection(&g_lock);
    vigem_target_remove(g_vigem, g_pad);
    vigem_target_free(g_pad);
    g_pad = NULL;
    LeaveCriticalSection(&g_lock);
}

/* ---- LCD ---- */

/* The output report is a report ID, 31 more header bytes, then 160 columns by 6
 * rows of bytes, each byte being 8 vertical pixels with bit 0 on top. Only 43 of
 * the 48 pixel rows are visible. A set bit is a dark pixel. */
#define LCD_WIDTH 160
#define LCD_HEIGHT 43
#define LCD_REPORT_LEN 992
#define LCD_REPORT_ID 3
#define LCD_HEADER_LEN 32

static const wchar_t LCD_TEXT[] = L"superpadde";

/* A toad: wide head, bulging eyes, warts and a big smile with its tongue out. */
static void lcd_draw_toad(HDC dc)
{
    static const POINT warts[] = {{6, 22}, {10, 18}, {39, 22}, {35, 18}, {12, 36}, {33, 36}};
    RECT tongue_top;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    size_t i;

    SelectObject(dc, pen);
    SelectObject(dc, white);
    Ellipse(dc, 2, 11, 45, 42);

    Ellipse(dc, 6, 1, 21, 16);
    Ellipse(dc, 26, 1, 41, 16);
    SelectObject(dc, black);
    Ellipse(dc, 10, 5, 18, 13);
    Ellipse(dc, 30, 5, 38, 13);
    SetPixelV(dc, 12, 7, RGB(255, 255, 255));
    SetPixelV(dc, 13, 7, RGB(255, 255, 255));
    SetPixelV(dc, 32, 7, RGB(255, 255, 255));
    SetPixelV(dc, 33, 7, RGB(255, 255, 255));

    SetPixelV(dc, 21, 20, RGB(0, 0, 0));
    SetPixelV(dc, 25, 20, RGB(0, 0, 0));
    for (i = 0; i < ARRAYSIZE(warts); i++) {
        SetPixelV(dc, warts[i].x, warts[i].y, RGB(0, 0, 0));
        SetPixelV(dc, warts[i].x + 1, warts[i].y, RGB(0, 0, 0));
    }

    SelectObject(dc, white);
    Ellipse(dc, 26, 26, 34, 37);
    SetRect(&tongue_top, 25, 25, 35, 30);
    FillRect(dc, &tongue_top, white);
    Arc(dc, 9, 12, 38, 31, 9, 22, 38, 22);

    SelectObject(dc, GetStockObject(BLACK_PEN));
    DeleteObject(pen);
}

/* Fills an output report with the startup screen. */
static void lcd_build_report(BYTE *report)
{
    BITMAPINFO bmi;
    void *bits;
    DWORD *pixels;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp, old_bmp;
    HFONT font = CreateFontW(-19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    RECT rect;
    int x, y;

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = LCD_WIDTH;
    bmi.bmiHeader.biHeight = -LCD_HEIGHT; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    pixels = (DWORD *)bits;
    old_bmp = (HBITMAP)SelectObject(dc, bmp);

    SetRect(&rect, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    FillRect(dc, &rect, (HBRUSH)GetStockObject(WHITE_BRUSH));
    lcd_draw_toad(dc);

    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    SetRect(&rect, 48, 0, LCD_WIDTH, LCD_HEIGHT);
    DrawTextW(dc, LCD_TEXT, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    GdiFlush();

    memset(report, 0, LCD_REPORT_LEN);
    report[0] = LCD_REPORT_ID;
    for (y = 0; y < LCD_HEIGHT; y++) {
        for (x = 0; x < LCD_WIDTH; x++) {
            if ((pixels[y * LCD_WIDTH + x] & 0xFFFFFF) == 0)
                report[LCD_HEADER_LEN + x + (y / 8) * LCD_WIDTH] |= (BYTE)(1 << (y % 8));
        }
    }

    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
}

/* Runs on its own thread so a stalled write can never hold up the keys. */
static DWORD WINAPI lcd_thread(LPVOID unused)
{
    static BYTE report[LCD_REPORT_LEN];
    DWORD written = 0;
    HANDLE device = open_g13(GENERIC_READ | GENERIC_WRITE);

    (void)unused;
    if (device == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"Can't open the G13 screen, error %lu\n", GetLastError());
        return 1;
    }
    lcd_build_report(report);
    if (!WriteFile(device, report, LCD_REPORT_LEN, &written, NULL))
        fwprintf(stderr, L"Can't write to the G13 screen, error %lu\n", GetLastError());
    CloseHandle(device);
    return 0;
}

static void lcd_show(void)
{
    HANDLE thread = CreateThread(NULL, 0, lcd_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

/* ---- record mode: light, keyboard hook and saving ---- */

#define FEATURE_BACKLIGHT 7
#define FEATURE_BUFFER_LEN 258 /* the largest feature report, as the G13 reports it */
#define LIGHT_STEP_MS 40
#define SAVE_ERROR_MS 2000

static BYTE g_light_normal[3];
static int g_light_have_normal;
static wchar_t g_config_path[MAX_PATH];
static HANDLE g_hook_stop, g_light_stop;
static volatile ULONGLONG g_save_error_until;

typedef struct {
    int target;
    Binding binding;
} RecordedJob;

/* Report 7 sets the shared backlight of the keys and the screen. */
static void light_set(HANDLE device, const BYTE *rgb)
{
    BYTE report[5];

    report[0] = FEATURE_BACKLIGHT;
    report[1] = rgb[0];
    report[2] = rgb[1];
    report[3] = rgb[2];
    report[4] = 0;
    if (!HidD_SetFeature(device, report, sizeof report))
        fwprintf(stderr, L"Can't set the backlight, error %lu\n", GetLastError());
}

/* Remembers the color to go back to. Kept until the G13 disconnects, so a quick
 * re-entry can never mistake a record mode color for the normal one. */
static void light_remember_normal(void)
{
    BYTE report[FEATURE_BUFFER_LEN];
    HANDLE device;

    if (g_light_have_normal) return;
    memset(report, 0, sizeof report);
    report[0] = FEATURE_BACKLIGHT;
    device = open_g13(GENERIC_READ | GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE) return;
    if (HidD_GetFeature(device, report, sizeof report)) {
        memcpy(g_light_normal, report + 1, 3);
        g_light_have_normal = 1;
        wprintf(L"Normal backlight color: %u %u %u\n", report[1], report[2], report[3]);
    } else {
        fwprintf(stderr, L"Can't read the backlight color, error %lu\n", GetLastError());
    }
    CloseHandle(device);
}

/* Drives the light for one stay in record mode, then restores the normal color.
 * It also checks the timeout, as it's the one thing that runs the whole time. */
static DWORD WINAPI record_light_thread(LPVOID stop_event)
{
    static const BYTE save_error[3] = {255, 0, 0};
    HANDLE stop = (HANDLE)stop_event;
    HANDLE device = open_g13(GENERIC_READ | GENERIC_WRITE);
    BYTE shown[3] = {0, 0, 0}, rgb[3];
    int first = 1;

    while (WaitForSingleObject(stop, first ? 0 : LIGHT_STEP_MS) == WAIT_TIMEOUT) {
        ULONGLONG now = GetTickCount64();

        if (now < g_save_error_until) memcpy(rgb, save_error, 3);
        else record_color(now, g_record_target != NO_TARGET, rgb);
        if (device != INVALID_HANDLE_VALUE && (first || memcmp(rgb, shown, 3) != 0)) {
            light_set(device, rgb);
            memcpy(shown, rgb, 3);
        }
        first = 0;
        record_check_timeout();
    }
    if (device != INVALID_HANDLE_VALUE) {
        if (g_light_have_normal) light_set(device, g_light_normal);
        CloseHandle(device);
    }
    CloseHandle(stop);
    return 0;
}

static LRESULT CALLBACK record_hook(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)lparam;
        if (!(key->flags & LLKHF_INJECTED)) {
            WORD mapped = hook_code(key->scanCode, (key->flags & LLKHF_EXTENDED) != 0);
            int down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
            if (mapped && record_key_event(mapped, down)) return 1;
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

/* The hook exists only while in record mode, on a thread that does nothing else:
 * Windows drops a keyboard hook that's slow to respond. */
static DWORD WINAPI record_hook_thread(LPVOID stop_event)
{
    HANDLE stop = (HANDLE)stop_event;
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, record_hook, GetModuleHandleW(NULL), 0);

    if (!hook) fwprintf(stderr, L"Can't watch the keyboard, error %lu\n", GetLastError());
    while (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    if (hook) UnhookWindowsHookEx(hook);
    CloseHandle(stop);
    return 0;
}

static void on_record_mode(int on)
{
    if (on) {
        HANDLE thread;
        light_remember_normal();
        g_hook_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        thread = CreateThread(NULL, 0, record_hook_thread, g_hook_stop, 0, NULL);
        if (thread) CloseHandle(thread);
        g_light_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        thread = CreateThread(NULL, 0, record_light_thread, g_light_stop, 0, NULL);
        if (thread) CloseHandle(thread);
    } else {
        /* Each thread closes its own handle once it has stopped. */
        if (g_hook_stop) SetEvent(g_hook_stop);
        if (g_light_stop) SetEvent(g_light_stop);
        g_hook_stop = NULL;
        g_light_stop = NULL;
    }
    wprintf(L"Record mode %ls.\n", on ? L"on" : L"off");
    fflush(stdout);
}

static int record_save(const RecordedJob *job)
{
    wchar_t text[128];
    const wchar_t *section, *name;

    binding_to_text(&job->binding, text, ARRAYSIZE(text));
    if (job->target >= STICK_TARGET_BASE) {
        section = L"stick";
        name = DIR_NAMES[job->target - STICK_TARGET_BASE];
    } else {
        section = L"keys";
        name = KEY_NAMES[job->target];
    }
    wprintf(L"Recorded %ls = %ls\n", name, text);
    fflush(stdout);
    if (!WritePrivateProfileStringW(section, name, text, g_config_path)) {
        fwprintf(stderr, L"Can't save to %ls, error %lu\n", g_config_path, GetLastError());
        return 0;
    }
    return 1;
}

/* Runs on its own thread, because saving would stall the hook. */
static DWORD WINAPI record_done_thread(LPVOID param)
{
    RecordedJob *job = (RecordedJob *)param;

    /* A failed save shows red: it's mapped for now, but won't survive a restart. */
    if (!record_save(job)) g_save_error_until = GetTickCount64() + SAVE_ERROR_MS;
    free(job);
    return 0;
}

static void on_recorded(int target, const Binding *binding)
{
    RecordedJob *job = (RecordedJob *)malloc(sizeof *job);
    HANDLE thread;

    if (!job) return;
    job->target = target;
    job->binding = *binding;
    thread = CreateThread(NULL, 0, record_done_thread, job, 0, NULL);
    if (thread) CloseHandle(thread);
    else free(job);
}

/* The G13 forgets its color when it loses power, so this runs on every connect. */
static void light_apply_idle_color(void)
{
    HANDLE device;

    if (!g_cfg.has_idle_color) return;
    memcpy(g_light_normal, g_cfg.idle_color, 3);
    g_light_have_normal = 1;
    device = open_g13(GENERIC_READ | GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE) return;
    light_set(device, g_light_normal);
    CloseHandle(device);
}

/* ---- device loop ---- */

#define WM_APP_TRAY (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_FATAL (WM_APP + 3)

static int g_dry_run;
static int g_no_console;
static HWND g_tray_hwnd;
static wchar_t g_log_path[MAX_PATH];

static void set_connected(int connected)
{
    if (g_tray_hwnd) PostMessageW(g_tray_hwnd, WM_APP_STATUS, connected, 0);
}

/* Runs until a fatal error, which it returns as an exit code. */
static int device_loop(void)
{
    int announced_wait = 0;

    for (;;) {
        BYTE report[REPORT_LEN];
        DWORD got;
        HANDLE device = open_g13(GENERIC_READ);

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
        if (g_cfg.analog && !g_dry_run && !add_virtual_pad()) {
            CloseHandle(device);
            return 4;
        }
        wprintf(L"G13 connected.\n");
        fflush(stdout);
        set_connected(1);
        if (!g_dry_run) light_apply_idle_color();
        if (!g_dry_run && g_cfg.show_screen) lcd_show();

        while (ReadFile(device, report, REPORT_LEN, &got, NULL)) {
            if (got == REPORT_LEN) {
                process_report(report);
                fflush(stdout);
            }
        }

        release_all();
        g_light_have_normal = 0; /* the G13 resets its color when it loses power */
        if (g_cfg.analog && !g_dry_run) remove_virtual_pad();
        CloseHandle(device);
        wprintf(L"G13 disconnected.\n");
        fflush(stdout);
        set_connected(0);
    }
}

/* ---- tray icon (background build only) ---- */

enum { MENU_OPEN_LOG = 1, MENU_QUIT };

static int g_tray_connected;
static HICON g_icon_connected, g_icon_waiting;
static UINT g_taskbar_created;

/* Draws a colored rounded square with a white G. GDI leaves alpha at zero, so
 * every pixel it touched is made opaque afterwards. */
static HICON make_icon(int size, COLORREF color)
{
    BITMAPINFO bmi;
    ICONINFO info;
    void *bits;
    BYTE *mask_bits;
    DWORD *pixels;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color_bmp, mask_bmp, old_bmp;
    HBRUSH brush = CreateSolidBrush(color);
    HFONT font = CreateFontW(-(size * 7 / 8), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    RECT rect;
    HICON icon;
    int i, radius = size / 4;

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    color_bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    pixels = (DWORD *)bits;
    old_bmp = (HBITMAP)SelectObject(dc, color_bmp);

    SelectObject(dc, brush);
    SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, 0, 0, size + 1, size + 1, radius * 2, radius * 2);

    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetRect(&rect, 0, 0, size, size);
    DrawTextW(dc, L"G", 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    for (i = 0; i < size * size; i++)
        if (pixels[i] & 0xFFFFFF) pixels[i] |= 0xFF000000;

    SelectObject(dc, old_bmp);
    mask_bits = (BYTE *)calloc((size_t)((size + 15) / 16 * 2 * size), 1);
    mask_bmp = CreateBitmap(size, size, 1, 1, mask_bits);
    info.fIcon = TRUE;
    info.xHotspot = 0;
    info.yHotspot = 0;
    info.hbmMask = mask_bmp;
    info.hbmColor = color_bmp;
    icon = CreateIconIndirect(&info);

    DeleteObject(mask_bmp);
    free(mask_bits);
    DeleteObject(color_bmp);
    DeleteObject(font);
    DeleteObject(brush);
    DeleteDC(dc);
    return icon;
}

static void tray_status_text(wchar_t *text, size_t count)
{
    swprintf_s(text, count, L"G13 %ls (stick: %ls)",
               g_tray_connected ? L"connected" : L"not connected",
               g_cfg.analog ? L"analog" : L"keys");
}

static void tray_update(DWORD operation)
{
    NOTIFYICONDATAW nid;

    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_tray_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_tray_connected ? g_icon_connected : g_icon_waiting;
    tray_status_text(nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(operation, &nid);
}

static void tray_show_menu(void)
{
    wchar_t status[64];
    POINT at;
    HMENU menu = CreatePopupMenu();

    tray_status_text(status, ARRAYSIZE(status));
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_OPEN_LOG, L"Open log");
    AppendMenuW(menu, MF_STRING, MENU_QUIT, L"Quit");

    GetCursorPos(&at);
    /* Both steps are required for the menu to close when clicking elsewhere. */
    SetForegroundWindow(g_tray_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, g_tray_hwnd, NULL);
    PostMessageW(g_tray_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static __declspec(noreturn) void tray_quit(void)
{
    release_all();
    tray_update(NIM_DELETE);
    ExitProcess(0);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    /* Sent when Explorer starts or restarts, which removes existing icons. */
    if (msg == g_taskbar_created) {
        tray_update(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_APP_TRAY:
        if (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP) tray_show_menu();
        return 0;
    case WM_APP_STATUS:
        g_tray_connected = (int)wparam;
        tray_update(NIM_MODIFY);
        return 0;
    case WM_APP_FATAL:
        MessageBoxW(hwnd, L"g13map stopped because of an error.\nDetails are in g13map.log.",
                    L"g13map", MB_OK | MB_ICONERROR);
        tray_quit();
    case WM_COMMAND:
        if (LOWORD(wparam) == MENU_OPEN_LOG) {
            wchar_t args[MAX_PATH + 2];
            swprintf_s(args, ARRAYSIZE(args), L"\"%ls\"", g_log_path);
            ShellExecuteW(NULL, L"open", L"notepad.exe", args, NULL, SW_SHOWNORMAL);
        } else if (LOWORD(wparam) == MENU_QUIT) {
            tray_quit();
        }
        return 0;
    case WM_CLOSE: /* sent by a plain taskkill, which install.bat uses */
        tray_quit();
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI device_thread(LPVOID unused)
{
    (void)unused;
    device_loop();
    PostMessageW(g_tray_hwnd, WM_APP_FATAL, 0, 0);
    return 0;
}

static int run_tray(void)
{
    WNDCLASSW wc;
    MSG msg;
    HANDLE thread;
    int size;

    SetProcessDPIAware();
    size = GetSystemMetrics(SM_CXSMICON);
    g_icon_connected = make_icon(size, RGB(0x1F, 0x9D, 0x55));
    g_icon_waiting = make_icon(size, RGB(0x6B, 0x72, 0x80));
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"g13map_tray";
    RegisterClassW(&wc);
    /* A hidden top-level window: a message-only one wouldn't get TaskbarCreated. */
    g_tray_hwnd = CreateWindowExW(0, wc.lpszClassName, L"g13map", WS_OVERLAPPED,
                                  0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    tray_update(NIM_ADD);

    thread = CreateThread(NULL, 0, device_thread, NULL, 0, NULL);
    if (!thread) return 4;
    CloseHandle(thread);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ---- startup ---- */

/* Without a console (the background build), output goes to a log beside the exe. */
static void log_to_file_if_no_console(const wchar_t *exe_path)
{
    wchar_t *slash;

    if (GetConsoleWindow()) return;
    g_no_console = 1;
    wcscpy_s(g_log_path, MAX_PATH, exe_path);
    slash = wcsrchr(g_log_path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_log_path), L"g13map.log");
    /* Not the _s variants: those lock the file, which would block reading the
     * log and block stderr from sharing it. */
    _wfreopen(g_log_path, L"w", stdout);
    _wfreopen(g_log_path, L"a", stderr);
}

/* The background build has nowhere to show a console error, so it pops one up. */
static int startup_failed(int code)
{
    if (g_no_console)
        MessageBoxW(NULL, L"g13map couldn't start.\nDetails are in g13map.log.",
                    L"g13map", MB_OK | MB_ICONERROR);
    return code;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t config_path[MAX_PATH];
    wchar_t *slash;
    int verbose = 0, i;
    wchar_t exe_path[MAX_PATH];
    HANDLE single_instance;

    /* Checked before opening the log, which would wipe the running copy's log. */
    single_instance = CreateMutexW(NULL, FALSE, L"Local\\g13map");
    if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (GetConsoleWindow()) fwprintf(stderr, L"g13map is already running.\n");
        return 3;
    }

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    log_to_file_if_no_console(exe_path);

    wcscpy_s(config_path, MAX_PATH, L"");
    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--dry-run") == 0) {
            g_dry_run = 1;
        } else if (wcscmp(argv[i], L"--verbose") == 0) {
            verbose = 1;
        } else if (GetFullPathNameW(argv[i], MAX_PATH, config_path, NULL) == 0) {
            fwprintf(stderr, L"Bad config path: %ls\n", argv[i]);
            return startup_failed(2);
        }
    }
    if (!config_path[0]) {
        wcscpy_s(config_path, MAX_PATH, exe_path);
        slash = wcsrchr(config_path, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - config_path), L"g13.ini");
    }
    if (!load_config(config_path, &g_cfg)) return startup_failed(2);

    wcscpy_s(g_config_path, MAX_PATH, config_path);
    InitializeCriticalSection(&g_lock);
    g_record_enabled = !g_dry_run;
    g_on_record_mode = on_record_mode;
    g_on_recorded = on_recorded;
    g_sink = g_dry_run ? sink_print : verbose ? sink_send_and_print : sink_sendinput;
    if (g_cfg.analog) {
        if (g_dry_run) g_pad_sink = pad_sink_print;
        else if (connect_vigem()) g_pad_sink = pad_sink_vigem;
        else return startup_failed(4);
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);

    wprintf(L"Config: %ls\nStick: %ls\n%ls", config_path,
            g_cfg.analog ? L"analog (virtual Xbox 360 controller)" : L"keys",
            g_dry_run ? L"Dry run: printing events instead of sending them.\n" : L"");
    if (!g_no_console) wprintf(L"Press Ctrl+C to quit.\n");
    fflush(stdout);

    return g_no_console ? run_tray() : device_loop();
}
#endif
