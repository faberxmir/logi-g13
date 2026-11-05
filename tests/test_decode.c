/* Tests the report decoding with synthetic reports, without touching hardware.
 * Byte positions come from the guided key test on a real G13. */
#define G13MAP_NO_MAIN
#include "../src/g13map.c"
#include <stdarg.h>

#define MAX_EVENTS 32

static struct { WORD code; int down; } g_events[MAX_EVENTS];
static int g_event_count;
static int g_failures;

static void sink_record(WORD code, int down)
{
    if (g_event_count < MAX_EVENTS) {
        g_events[g_event_count].code = code;
        g_events[g_event_count].down = down;
    }
    g_event_count++;
}

static void expect_events(const char *what, int count, ...)
{
    va_list ap;
    int i, ok = g_event_count == count;

    va_start(ap, count);
    for (i = 0; i < count && ok; i++) {
        int code = va_arg(ap, int), down = va_arg(ap, int);
        ok = g_events[i].code == code && g_events[i].down == down;
    }
    va_end(ap);

    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("  got %d event(s):", g_event_count);
        for (i = 0; i < g_event_count && i < MAX_EVENTS; i++)
            printf(" %#x/%d", g_events[i].code, g_events[i].down);
        printf("\n");
        g_failures++;
    }
    g_event_count = 0;
}

static void send_report(int x, int y, BYTE b3, BYTE b4, BYTE b5, BYTE b6, BYTE b7)
{
    BYTE r[REPORT_LEN];
    r[0] = REPORT_ID;
    r[1] = (BYTE)x; r[2] = (BYTE)y;
    r[3] = b3; r[4] = b4; r[5] = b5; r[6] = b6; r[7] = b7;
    process_report(r);
}

#define CX 125
#define CY 130
#define Q 0x10
#define W 0x11
#define A 0x1E
#define SHIFT 0x2A
#define ONE 0x02

static void set_key(int bit, WORD a, WORD b)
{
    g_cfg.keys[bit].codes[0] = a;
    g_cfg.keys[bit].count = 1;
    if (b) {
        g_cfg.keys[bit].codes[1] = b;
        g_cfg.keys[bit].count = 2;
    }
}

static int g_pad_calls;
static SHORT g_last_x, g_last_y;

static void pad_record(SHORT x, SHORT y)
{
    g_pad_calls++;
    g_last_x = x;
    g_last_y = y;
}

static void expect_pad(const char *what, int calls, int x, int y)
{
    int ok = g_pad_calls == calls && g_last_x == x && g_last_y == y;
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("  got %d call(s), x=%d y=%d\n", g_pad_calls, g_last_x, g_last_y);
        g_failures++;
    }
    g_pad_calls = 0;
}

static void test_analog(void)
{
    g_cfg.analog = 1;
    g_cfg.deadzone = 12;
    g_pad_sink = pad_record;
    g_event_count = 0;

    send_report(CX, CY, 0, 0, 0, 0, 0);
    expect_pad("center sends nothing", 0, 0, 0);
    send_report(CX + 5, CY - 5, 0, 0, 0, 0, 0);
    expect_pad("measured rest offset stays in dead zone", 0, 0, 0);

    send_report(CX, 0, 0, 0, 0, 0, 0);
    expect_pad("full up is +Y", 1, 0, 32767);
    expect_events("analog stick presses no keys", 0);
    send_report(CX, 255, 0, 0, 0, 0, 0);
    expect_pad("full down is -Y", 1, 0, -32767);
    send_report(0, CY, 0, 0, 0, 0, 0);
    expect_pad("full left is -X", 1, -32767, 0);
    send_report(255, CY, 0, 0, 0, 0, 0);
    expect_pad("full right is +X", 1, 32767, 0);

    send_report(255, 255, 0, 0, 0, 0, 0);
    g_pad_calls = 0;
    {
        double len = sqrt((double)g_last_x * g_last_x + (double)g_last_y * g_last_y);
        int ok = g_last_x > 0 && g_last_y < 0 && len > 32760 && len < 32775;
        printf("%s: corner is clamped to the unit circle\n", ok ? "ok  " : "FAIL");
        if (!ok) g_failures++;
    }

    send_report(CX, CY - 50, 0, 0, 0, 0, 0);
    {
        int ok = g_last_x == 0 && g_last_y > 0 && g_last_y < 32767 / 2;
        printf("%s: partial tilt gives partial output\n", ok ? "ok  " : "FAIL");
        if (!ok) g_failures++;
    }
    g_pad_calls = 0;

    g_cfg.invert_y = 1;
    send_report(CX, 0, 0, 0, 0, 0, 0);
    expect_pad("invert_y flips up to -Y", 1, 0, -32767);
    g_cfg.invert_y = 0;

    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    expect_pad("back to center sends neutral", 1, 0, 0);
    expect_events("keys still work in analog mode", 1, Q, 1);
    send_report(CX, CY, 0, 0, 0, 0, 0);
    g_event_count = 0;

    send_report(255, CY, 0, 0, 0, 0, 0);
    g_pad_calls = 0;
    release_all();
    expect_pad("release_all neutralizes the pad", 1, 0, 0);

    g_cfg.analog = 0;
}

static int g_mode_calls, g_last_mode;
static int g_recorded_calls, g_recorded_target;
static Binding g_recorded_binding;

static void mode_record(int on)
{
    g_mode_calls++;
    g_last_mode = on;
}

static void recorded_record(int target, const Binding *binding)
{
    g_recorded_calls++;
    g_recorded_target = target;
    g_recorded_binding = *binding;
}

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

static int recorded_is(int calls, int target, int count, WORD a, WORD b)
{
    int ok = g_recorded_calls == calls && g_recorded_target == target &&
             g_recorded_binding.count == count;
    if (ok && count > 0) ok = g_recorded_binding.codes[0] == a;
    if (ok && count > 1) ok = g_recorded_binding.codes[1] == b;
    g_recorded_calls = 0;
    return ok;
}

#define G1_BIT 0
#define G2_BIT 1
#define MR_BYTE7 0x01
#define KEY_E 0x12

static void mr_tap(void)
{
    send_report(CX, CY, 0, 0, 0, 0, MR_BYTE7);
    send_report(CX, CY, 0, 0, 0, 0, 0);
}

static void test_record(void)
{
    memset(g_cfg.keys, 0, sizeof g_cfg.keys);
    memset(g_cfg.stick, 0, sizeof g_cfg.stick);
    set_key(G1_BIT, Q, 0);
    g_cfg.stick[DIR_UP].codes[0] = W;
    g_cfg.stick[DIR_UP].count = 1;
    g_record_enabled = 1;
    g_on_record_mode = mode_record;
    g_on_recorded = recorded_record;
    g_event_count = 0;

    mr_tap();
    check("MR turns record mode on", g_mode_calls == 1 && g_last_mode == 1 && g_recording);
    expect_events("MR sends no key", 0);
    check("a key with no G13 key waiting passes through", !record_key_event(KEY_E, 1));
    check("  and so does its release", !record_key_event(KEY_E, 0));

    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    expect_events("G1 sends nothing while recording", 0);
    check("a keyboard key is hidden when G1 is waiting", record_key_event(KEY_E, 1));
    check("G1 is recorded as that key",
          recorded_is(1, G1_BIT, 1, KEY_E, 0) && g_cfg.keys[G1_BIT].codes[0] == KEY_E);
    check("auto-repeat is hidden after recording", record_key_event(KEY_E, 1));
    check("its release is hidden too", record_key_event(KEY_E, 0));
    check("record mode stays on after mapping", g_recording);
    check("the next key passes through again", !record_key_event(KEY_E, 1));
    record_key_event(KEY_E, 0);

    send_report(CX, CY, 0x02, 0, 0, 0, 0);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    record_key_event(SHIFT, 1);
    check("a held modifier alone records nothing yet", g_recorded_calls == 0);
    record_key_event(ONE, 1);
    check("Shift+1 records a combo", recorded_is(1, G2_BIT, 2, SHIFT, ONE));
    record_key_event(ONE, 0);
    record_key_event(SHIFT, 0);
    check("releasing after a combo records nothing more", g_recorded_calls == 0);

    send_report(CX, CY, 0x02, 0, 0, 0, 0);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    record_key_event(SHIFT, 1);
    check("tapped modifier is hidden", record_key_event(SHIFT, 0));
    check("a tapped modifier maps by itself", recorded_is(1, G2_BIT, 1, SHIFT, 0));

    send_report(CX, CY - 60, 0, 0, 0, 0, 0);
    send_report(CX, CY, 0, 0, 0, 0, 0);
    expect_events("stick sends nothing while recording", 0);
    record_key_event(KEY_E, 1);
    record_key_event(KEY_E, 0);
    check("stick up can be recorded",
          recorded_is(1, STICK_TARGET_BASE + DIR_UP, 1, KEY_E, 0) &&
          g_cfg.stick[DIR_UP].codes[0] == KEY_E);

    send_report(CX, CY, 0, 0, 0, 0, 0x02);
    send_report(CX, CY, 0, 0, 0, 0, 0);
    record_key_event(KEY_E, 1);
    record_key_event(KEY_E, 0);
    check("the button left of the stick can be recorded", recorded_is(1, 33, 1, KEY_E, 0));
    send_report(CX, CY, 0, 0, 0, 0, 0x08);
    send_report(CX, CY, 0, 0, 0, 0, 0);
    record_key_event(KEY_E, 1);
    record_key_event(KEY_E, 0);
    check("the stick press can be recorded", recorded_is(1, 35, 1, KEY_E, 0));

    send_report(CX, CY, 0, 0, 0, 0x02, 0);
    send_report(CX, CY, 0, 0, 0, 0, 0);
    check("L1 can't be recorded", !record_key_event(KEY_E, 1));
    record_key_event(KEY_E, 0);

    g_mode_calls = 0;
    mr_tap();
    check("MR turns record mode off", g_mode_calls == 1 && g_last_mode == 0 && !g_recording);
    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    expect_events("G1 sends its new key afterwards", 1, KEY_E, 1);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    expect_events("  and releases it", 1, KEY_E, 0);

    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    g_event_count = 0;
    send_report(CX, CY, 0x01, 0, 0, 0, MR_BYTE7);
    expect_events("entering record mode releases a held key", 1, KEY_E, 0);
    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    send_report(CX, CY, 0x01, 0, 0, 0, MR_BYTE7);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    expect_events("releasing a key held across record mode sends nothing", 0);
    check("key counts stay balanced", g_refs[KEY_E] == 0);

    mr_tap();
    g_mode_calls = 0;
    release_all();
    check("disconnecting leaves record mode", g_mode_calls == 1 && !g_recording);

    g_record_enabled = 0;
    mr_tap();
    check("MR does nothing when record mode is disabled", !g_recording);
    g_on_record_mode = NULL;
    g_on_recorded = NULL;

    check("right Shift isn't treated as extended", hook_code(0x36, 1) == 0x36);
    check("right Ctrl stays extended", hook_code(0x1D, 1) == (EXTENDED | 0x1D));
    check("arrow keys stay extended", hook_code(0x48, 1) == (EXTENDED | 0x48));
    check("numpad Enter falls back to Enter", hook_code(0x1C, 1) == 0x1C);
    check("AltGr's fake Ctrl is ignored", hook_code(0x21D, 0) == 0);
    check("keys with no name are ignored", hook_code(0x5B, 1) == 0);
}

static ULONGLONG g_fake_now;

static ULONGLONG WINAPI fake_clock(void)
{
    return g_fake_now;
}

static void test_record_timeout(void)
{
    BYTE rgb[3], other[3];

    g_clock = fake_clock;
    g_fake_now = 1000;
    g_record_enabled = 1;
    memset(g_cfg.keys, 0, sizeof g_cfg.keys);
    send_report(CX, CY, 0, 0, 0, 0, 0);

    mr_tap();
    g_fake_now += RECORD_TIMEOUT_MS - 1;
    record_check_timeout();
    check("record mode holds until just before the timeout", g_recording);

    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    g_fake_now += RECORD_TIMEOUT_MS - 1;
    record_check_timeout();
    check("pressing a G13 key restarts the timer", g_recording);

    record_key_event(KEY_E, 1);
    record_key_event(KEY_E, 0);
    g_fake_now += RECORD_TIMEOUT_MS - 1;
    record_check_timeout();
    check("recording a key restarts the timer", g_recording);

    send_report(CX, CY, 0, 0, 0, 0x02, 0);
    send_report(CX, CY, 0, 0, 0, 0, 0);
    g_fake_now += RECORD_TIMEOUT_MS - 1;
    record_check_timeout();
    check("pressing any G13 key restarts the timer", g_recording);

    record_key_event(KEY_E, 1);
    record_key_event(KEY_E, 0);
    g_fake_now += 1;
    record_check_timeout();
    check("normal typing doesn't restart the timer", !g_recording);

    mr_tap();
    send_report(CX, CY, 0x01, 0, 0, 0, 0);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    g_fake_now += RECORD_TIMEOUT_MS;
    record_check_timeout();
    check("a key left waiting times out too", !g_recording && g_record_target == NO_TARGET);
    release_all();

    record_color(0, 0, rgb);
    record_color(RECORD_PULSE_MS / 2, 0, other);
    check("not waiting is steady dark green",
          rgb[0] == 0 && rgb[1] == 60 && rgb[2] == 0 && memcmp(rgb, other, 3) == 0);
    record_color(0, 1, other);
    check("rolling starts at dark green", memcmp(rgb, other, 3) == 0);
    record_color(RECORD_PULSE_MS / 2, 1, rgb);
    check("rolling reaches light green halfway", rgb[0] == 140 && rgb[1] == 255 && rgb[2] == 140);
    record_color(RECORD_PULSE_MS, 1, other);
    check("rolling returns to dark green", other[0] == 0 && other[1] == 60 && other[2] == 0);

    g_record_enabled = 0;
    g_clock = GetTickCount64;
}

/* Every key name must survive being written to and read back from a config file. */
static void test_save_round_trip(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH], text[128], read_back[128];
    size_t i;
    int failed = 0;

    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"g13", 0, path);
    for (i = 0; i < ARRAYSIZE(KEY_CODES); i++) {
        Binding in, out;
        in.count = 2;
        in.codes[0] = SHIFT;
        in.codes[1] = KEY_CODES[i].code;
        binding_to_text(&in, text, ARRAYSIZE(text));
        WritePrivateProfileStringW(L"keys", L"G1", text, path);
        GetPrivateProfileStringW(L"keys", L"G1", L"", read_back, ARRAYSIZE(read_back), path);
        if (!parse_binding(read_back, &out, L"t") || out.count != 2 ||
            out.codes[1] != KEY_CODES[i].code) {
            printf("  combo failed for %ls: read back '%ls'\n", KEY_CODES[i].name, read_back);
            failed = 1;
        }
        in.count = 1;
        in.codes[0] = KEY_CODES[i].code;
        binding_to_text(&in, text, ARRAYSIZE(text));
        WritePrivateProfileStringW(L"keys", L"G1", text, path);
        GetPrivateProfileStringW(L"keys", L"G1", L"", read_back, ARRAYSIZE(read_back), path);
        if (!parse_binding(read_back, &out, L"t") || out.count != 1 ||
            out.codes[0] != KEY_CODES[i].code) {
            printf("  single failed for %ls: read back '%ls'\n", KEY_CODES[i].name, read_back);
            failed = 1;
        }
    }
    DeleteFileW(path);
    check("every key name survives saving and loading", !failed);
}

static void test_parse_color(void)
{
    BYTE rgb[3] = {0, 0, 0};

    check("parses a color", parse_color(L"255,110,0", rgb) &&
                            rgb[0] == 255 && rgb[1] == 110 && rgb[2] == 0);
    check("allows spaces in a color", parse_color(L"1, 2, 3", rgb) && rgb[2] == 3);
    check("rejects too few color values", !parse_color(L"255,110", rgb));
    check("rejects a color value above 255", !parse_color(L"256,0,0", rgb));
    check("rejects a negative color value", !parse_color(L"-1,0,0", rgb));
    check("rejects extra text after a color", !parse_color(L"1,2,3,4", rgb));
    check("rejects a color name", !parse_color(L"amber", rgb));
}

static void test_parse(void)
{
    Binding b;
    int ok = parse_binding(L"shift+1", &b, L"t") && b.count == 2 &&
             b.codes[0] == SHIFT && b.codes[1] == ONE;
    printf("%s: parses a combo case-insensitively\n", ok ? "ok  " : "FAIL");
    if (!ok) g_failures++;

    ok = parse_binding(L"", &b, L"t") && b.count == 0;
    printf("%s: empty binding is unbound\n", ok ? "ok  " : "FAIL");
    if (!ok) g_failures++;

    fwprintf(stderr, L"(the next line is an expected error)\n");
    ok = !parse_binding(L"NOPE", &b, L"t");
    printf("%s: rejects an unknown key name\n", ok ? "ok  " : "FAIL");
    if (!ok) g_failures++;
}

int main(void)
{
    InitializeCriticalSection(&g_lock);
    g_sink = sink_record;

    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.center_x = CX;
    g_cfg.center_y = CY;
    g_cfg.press_threshold = 50;
    g_cfg.release_threshold = 40;
    set_key(0, Q, 0);          /* G1 */
    set_key(21, SHIFT, ONE);   /* G22 */
    set_key(29, A, 0);         /* M1 */
    g_cfg.stick[DIR_UP].codes[0] = W;
    g_cfg.stick[DIR_UP].count = 1;

    send_report(CX, CY, 0x01, 0, 0x80, 0, 0);
    expect_events("G1 press, with backlight state bit ignored", 1, Q, 1);
    send_report(CX, CY, 0x00, 0, 0x80, 0, 0);
    expect_events("G1 release", 1, Q, 0);

    send_report(CX, CY, 0, 0, 0x80 | 0x20, 0, 0);
    expect_events("G22 combo presses modifier first", 2, SHIFT, 1, ONE, 1);
    send_report(CX, CY, 0, 0, 0x80, 0, 0);
    expect_events("G22 combo releases in reverse", 2, ONE, 0, SHIFT, 0);

    send_report(CX, CY, 0, 0, 0x80, 0x20, 0);
    expect_events("M1 in byte 6", 1, A, 1);
    send_report(CX, CY, 0, 0, 0x80, 0, 0);
    expect_events("M1 release", 1, A, 0);

    send_report(CX, CY, 0, 0, 0x80, 0, 0x80);
    send_report(CX, CY, 0, 0, 0x80, 0, 0x00);
    send_report(CX, CY, 0, 0, 0x80, 0, 0x40);
    send_report(CX, CY, 0, 0, 0x80, 0, 0x00);
    expect_events("noise bits 38 and 39 emit nothing", 0);

    send_report(CX, CY - 49, 0, 0, 0x80, 0, 0);
    expect_events("stick below press threshold does nothing", 0);
    send_report(CX, CY - 50, 0, 0, 0x80, 0, 0);
    expect_events("stick at press threshold presses", 1, W, 1);
    send_report(CX, CY - 41, 0, 0, 0x80, 0, 0);
    expect_events("hysteresis holds above release threshold", 0);
    send_report(CX, CY - 39, 0, 0, 0x80, 0, 0);
    expect_events("stick below release threshold releases", 1, W, 0);

    /* Shared key: released only when the last user lets go. */
    set_key(1, SHIFT, 0);      /* G2 */
    set_key(2, SHIFT, 0);      /* G3 */
    send_report(CX, CY, 0x02, 0, 0, 0, 0);
    expect_events("first holder of shared key presses", 1, SHIFT, 1);
    send_report(CX, CY, 0x06, 0, 0, 0, 0);
    expect_events("second holder sends nothing", 0);
    send_report(CX, CY, 0x04, 0, 0, 0, 0);
    expect_events("first release keeps shared key down", 0);
    send_report(CX, CY, 0x00, 0, 0, 0, 0);
    expect_events("last release lifts shared key", 1, SHIFT, 0);

    send_report(CX, CY - 100, 0x01, 0, 0, 0, 0);
    g_event_count = 0;
    release_all();
    expect_events("release_all lifts everything", 2, Q, 0, W, 0);

    g_event_count = 0;
    { BYTE r[REPORT_LEN] = {2, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; process_report(r); }
    expect_events("other report IDs are ignored", 0);

    test_analog();
    test_record();
    test_record_timeout();
    test_save_round_trip();
    test_parse_color();
    test_parse();

    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures != 0;
}
