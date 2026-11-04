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
    test_parse();

    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures != 0;
}
