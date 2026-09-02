/* schedule.cpp - see schedule.h.
 *
 *   Part 1  Data model + NVS persistence
 *   Part 2  Time base (local = UTC + tz) + evaluation (schedule_on/calendar_on)
 *   Part 3  Structured entry points (shared by console + JSON protocol)
 *   Part 4  Console dispatch + clear listing
 *   Part 5  Config backup / restore (JSON)
 */
#include "schedule.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <strings.h>          /* strcasecmp */

#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "sched";

/* ===================================================================== *
 *  Part 1 - data model + persistence
 * ===================================================================== */
struct sched_event_t { uint16_t start_min; uint16_t end_min; };   /* inclusive */
struct day_sched_t   { uint8_t n; sched_event_t ev[SCHED_MAX_EV]; };
struct scheduler_t   { uint8_t used; uint8_t cal_control; day_sched_t day[SCHED_DAYS]; };
struct holiday_t     { uint8_t day; uint8_t month; };             /* 0 = wildcard */

static scheduler_t g_sched[SCHED_MAX];
static holiday_t   g_hol[CAL_MAX];
static uint8_t     g_n_hol;
static int16_t     g_tz_min;      /* local = UTC + tz (minutes east of UTC) */

#define NVS_NS   "matterhub"      /* same namespace as main.cpp / logic / bindings */
#define K_SCHED  "sched"          /* blob: scheduler_t[SCHED_MAX]                   */
#define K_CAL    "calendar"       /* blob: cal_nvs_t                                */
#define K_TZ     "tz"             /* i32: tz offset minutes                         */

struct cal_nvs_t { uint8_t n; holiday_t hol[CAL_MAX]; };

static void sched_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, K_SCHED, g_sched, sizeof(g_sched));
    cal_nvs_t c; memset(&c, 0, sizeof(c));
    c.n = g_n_hol; memcpy(c.hol, g_hol, sizeof(g_hol));
    nvs_set_blob(h, K_CAL, &c, sizeof(c));
    nvs_set_i32(h, K_TZ, g_tz_min);
    nvs_commit(h);
    nvs_close(h);
}

void schedule_load(void)
{
    memset(g_sched, 0, sizeof(g_sched));
    memset(g_hol, 0, sizeof(g_hol));
    g_n_hol = 0; g_tz_min = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 0;
    /* Exact-size blobs only (like logic/bindings) - a layout change is ignored. */
    if (nvs_get_blob(h, K_SCHED, nullptr, &sz) == ESP_OK && sz == sizeof(g_sched)) {
        sz = sizeof(g_sched); nvs_get_blob(h, K_SCHED, g_sched, &sz);
    }
    sz = 0;
    if (nvs_get_blob(h, K_CAL, nullptr, &sz) == ESP_OK && sz == sizeof(cal_nvs_t)) {
        cal_nvs_t c; sz = sizeof(c); nvs_get_blob(h, K_CAL, &c, &sz);
        g_n_hol = (c.n <= CAL_MAX) ? c.n : 0;
        memcpy(g_hol, c.hol, sizeof(g_hol));
    }
    int32_t tz = 0;
    if (nvs_get_i32(h, K_TZ, &tz) == ESP_OK) g_tz_min = (int16_t)tz;
    nvs_close(h);

    int ns = 0;
    for (int i = 0; i < SCHED_MAX; ++i) if (g_sched[i].used) ++ns;
    if (ns || g_n_hol)
        ESP_LOGI(TAG, "Loaded %d scheduler(s), %d holiday date(s), tz=%+dmin", ns, g_n_hol, g_tz_min);
}

/* ===================================================================== *
 *  Part 2 - time base + evaluation
 * ===================================================================== */
/* Clock is "set" once it advances past 2020-01-01 (settime writes real time;
 * an un-set clock sits near the epoch). */
#define CLOCK_SET_THRESHOLD 1577836800LL   /* 2020-01-01 UTC */

bool schedule_clock_ready(void) { return (int64_t)time(nullptr) >= CLOCK_SET_THRESHOLD; }

/* Fill `out` with the current LOCAL broken-down time. Returns false if the clock
 * isn't set yet (fail-safe: callers treat this as "everything off"). */
static bool local_tm(struct tm *out)
{
    time_t t = time(nullptr);
    if ((int64_t)t < CLOCK_SET_THRESHOLD) return false;
    time_t loc = t + (time_t)g_tz_min * 60;   /* shift, then read as if UTC = local */
    gmtime_r(&loc, out);
    return true;
}

static bool date_is_holiday(int day, int month)
{
    for (int i = 0; i < g_n_hol; ++i) {
        const holiday_t &e = g_hol[i];
        if ((e.day == 0 || e.day == day) && (e.month == 0 || e.month == month)) return true;
    }
    return false;
}

bool calendar_on(void)
{
    struct tm tm;
    if (!local_tm(&tm)) return false;
    return date_is_holiday(tm.tm_mday, tm.tm_mon + 1);
}

bool schedule_on(int n)
{
    if (n < 0 || n >= SCHED_MAX || !g_sched[n].used) return false;
    struct tm tm;
    if (!local_tm(&tm)) return false;
    int dow  = (tm.tm_wday + 6) % 7;   /* tm_wday: Sun=0..Sat=6 -> Mon=0..Sun=6 */
    int didx = (g_sched[n].cal_control && date_is_holiday(tm.tm_mday, tm.tm_mon + 1))
             ? SCHED_HOLIDAY : dow;
    int mins = tm.tm_hour * 60 + tm.tm_min;
    const day_sched_t &d = g_sched[n].day[didx];
    for (int i = 0; i < d.n; ++i)
        if (mins >= d.ev[i].start_min && mins <= d.ev[i].end_min) return true;
    return false;
}

int  schedule_tz_offset_min(void) { return g_tz_min; }
void schedule_set_tz(int minutes) { g_tz_min = (int16_t)minutes; sched_save(); }

/* ===================================================================== *
 *  Part 3 - parse helpers + structured entry points
 * ===================================================================== */
int sched_parse_hhmm(const char *s)
{
    if (!s) return -1;
    int H = -1, M = -1;
    if (sscanf(s, "%d:%d", &H, &M) != 2) return -1;
    if (H < 0 || H > 23 || M < 0 || M > 59) return -1;
    return H * 60 + M;
}

bool sched_parse_ddmm(const char *s, int *day, int *month)
{
    if (!s) return false;
    const char *slash = strchr(s, '/');
    if (!slash) return false;
    char a[8], b[8];
    size_t la = (size_t)(slash - s);
    if (la == 0 || la >= sizeof(a)) return false;
    memcpy(a, s, la); a[la] = '\0';
    strncpy(b, slash + 1, sizeof(b) - 1); b[sizeof(b) - 1] = '\0';
    int d, m;
    if (!strcmp(a, "**")) d = 0; else { d = atoi(a); if (d < 1 || d > 31) return false; }
    if (!strcmp(b, "**")) m = 0; else { m = atoi(b); if (m < 1 || m > 12) return false; }
    if (d == 0 && m == 0) return false;   /* wildcard on both fields = every day: disallow */
    *day = d; *month = m;
    return true;
}

int sched_day_index(const char *t)
{
    static const char *k[7] = { "mon","tue","wed","thu","fri","sat","sun" };
    for (int i = 0; i < 7; ++i) if (!strcasecmp(t, k[i])) return i;
    if (!strcasecmp(t, "holiday") || !strcasecmp(t, "hol")) return SCHED_HOLIDAY;
    return -1;
}

static bool ranges_overlap(int a1, int b1, int a2, int b2) { return a1 <= b2 && a2 <= b1; }

bool sched_add_event(int n, int day, int start, int end, char *err, size_t cap)
{
    if (n < 0 || n >= SCHED_MAX) { if (err) snprintf(err, cap, "scheduler must be 1..%d", SCHED_MAX); return false; }
    if (day < 0 || day >= SCHED_DAYS) { if (err) snprintf(err, cap, "bad day"); return false; }
    if (start < 0 || start > 1439 || end < 0 || end > 1439 || start > end)
        { if (err) snprintf(err, cap, "bad time range (need start <= end within a day)"); return false; }
    day_sched_t &d = g_sched[n].day[day];
    if (d.n >= SCHED_MAX_EV) { if (err) snprintf(err, cap, "day full (max %d events)", SCHED_MAX_EV); return false; }
    for (int i = 0; i < d.n; ++i)
        if (ranges_overlap(start, end, d.ev[i].start_min, d.ev[i].end_min))
            { if (err) snprintf(err, cap, "overlaps an existing event that day"); return false; }
    d.ev[d.n].start_min = (uint16_t)start;
    d.ev[d.n].end_min   = (uint16_t)end;
    d.n++;
    g_sched[n].used = 1;
    sched_save();
    return true;
}

bool sched_rm_event(int n, int day, int idx, char *err, size_t cap)
{
    if (n < 0 || n >= SCHED_MAX || day < 0 || day >= SCHED_DAYS)
        { if (err) snprintf(err, cap, "bad scheduler/day"); return false; }
    day_sched_t &d = g_sched[n].day[day];
    if (idx < 0 || idx >= d.n) { if (err) snprintf(err, cap, "no event [%d]", idx); return false; }
    for (int i = idx; i < d.n - 1; ++i) d.ev[i] = d.ev[i + 1];
    d.n--;
    sched_save();
    return true;
}

bool sched_set_cal(int n, bool on, char *err, size_t cap)
{
    if (n < 0 || n >= SCHED_MAX) { if (err) snprintf(err, cap, "scheduler must be 1..%d", SCHED_MAX); return false; }
    g_sched[n].cal_control = on ? 1 : 0;
    g_sched[n].used = 1;
    sched_save();
    return true;
}

bool calendar_add(int day, int month, char *err, size_t cap)
{
    if (g_n_hol >= CAL_MAX) { if (err) snprintf(err, cap, "calendar full (max %d)", CAL_MAX); return false; }
    for (int i = 0; i < g_n_hol; ++i)
        if (g_hol[i].day == day && g_hol[i].month == month) { if (err) snprintf(err, cap, "duplicate date"); return false; }
    g_hol[g_n_hol].day = (uint8_t)day;
    g_hol[g_n_hol].month = (uint8_t)month;
    g_n_hol++;
    sched_save();
    return true;
}

bool calendar_rm(int idx, char *err, size_t cap)
{
    if (idx < 0 || idx >= g_n_hol) { if (err) snprintf(err, cap, "no calendar entry [%d]", idx); return false; }
    for (int i = idx; i < g_n_hol - 1; ++i) g_hol[i] = g_hol[i + 1];
    g_n_hol--;
    sched_save();
    return true;
}

/* ===================================================================== *
 *  Part 4 - console dispatch + listing
 * ===================================================================== */
static const char *DOW_NAME[SCHED_DAYS] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun","Holiday" };

static void fmt_hhmm(int min, char *buf, size_t cap) { snprintf(buf, cap, "%02d:%02d", min / 60, min % 60); }
static void fmt_ddmm(const holiday_t &e, char *buf, size_t cap)
{
    char d[4], m[4];
    if (e.day == 0)   strcpy(d, "**"); else snprintf(d, sizeof(d), "%02d", e.day);
    if (e.month == 0) strcpy(m, "**"); else snprintf(m, sizeof(m), "%02d", e.month);
    snprintf(buf, cap, "%s/%s", d, m);
}

static void print_scheduler(int n)
{
    scheduler_t &s = g_sched[n];
    if (!s.used) { printf("  Scheduler %d  [unused]\r\n", n + 1); return; }
    printf("  Scheduler %d  [calendar-control: %s]  -- now: %s\r\n",
           n + 1, s.cal_control ? "ON" : "off", schedule_on(n) ? "ON" : "OFF");
    for (int day = 0; day < SCHED_DAYS; ++day) {
        day_sched_t &d = s.day[day];
        printf("    %-7s: ", DOW_NAME[day]);
        if (d.n == 0) { printf("-\r\n"); continue; }
        for (int i = 0; i < d.n; ++i) {
            char a[8], b[8];
            fmt_hhmm(d.ev[i].start_min, a, sizeof(a));
            fmt_hhmm(d.ev[i].end_min,   b, sizeof(b));
            printf("%s%s-%s", i ? ", " : "", a, b);
        }
        printf("\r\n");
    }
}

static void print_all_schedulers(void)
{
    printf("[sched] Schedulers (local time; clock %s, tz %+03d:%02d):\r\n",
           schedule_clock_ready() ? "set" : "NOT set", g_tz_min / 60, abs(g_tz_min) % 60);
    for (int n = 0; n < SCHED_MAX; ++n) print_scheduler(n);
}

static void print_calendar(void)
{
    printf("[calendar] Public holidays (shared; DD/MM, ** = any):\r\n");
    if (g_n_hol == 0) printf("  (none)\r\n");
    for (int i = 0; i < g_n_hol; ++i) { char b[8]; fmt_ddmm(g_hol[i], b, sizeof(b)); printf("  [%d] %s\r\n", i, b); }
    struct tm tm;
    if (local_tm(&tm))
        printf("  Today %02d/%02d -> holiday: %s\r\n", tm.tm_mday, tm.tm_mon + 1,
               date_is_holiday(tm.tm_mday, tm.tm_mon + 1) ? "YES" : "no");
    else
        printf("  (clock not set - schedule/calendar conditions read OFF)\r\n");
}

void schedule_print_help(void)
{
    printf("  tz <+HH:MM|-HH:MM>       set local timezone offset (local = UTC + offset)\r\n"
           "  sched                    list all %d schedulers + live on/off\r\n"
           "  sched <n> <day> add HH:MM HH:MM   add event (end = last ON minute; no overlap)\r\n"
           "  sched <n> <day> rm <idx>          remove an event (day: mon..sun, holiday)\r\n"
           "  sched <n> cal on|off     scheduler n follows the holiday calendar or not\r\n"
           "  calendar                 list shared public holidays\r\n"
           "  calendar add DD/MM       add holiday (** = any, e.g. 25/12, **/07, 01/**)\r\n"
           "  calendar rm <idx>        remove a holiday\r\n", SCHED_MAX);
}

bool schedule_handle_cmd(const char *line)
{
    char buf[160];
    strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char *argv[10]; int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 10; t = strtok(nullptr, " ")) argv[argc++] = t;
    if (argc == 0) return false;

    /* ---- tz ---- */
    if (!strcmp(argv[0], "tz")) {
        if (argc == 1) { printf("[sched] tz offset = %+03d:%02d (local = UTC + offset)\r\n", g_tz_min / 60, abs(g_tz_min) % 60); return true; }
        int sign = 1; const char *p = argv[1];
        if (*p == '+') { ++p; } else if (*p == '-') { sign = -1; ++p; }
        int H = 0, M = 0;
        if (sscanf(p, "%d:%d", &H, &M) != 2) { M = 0; if (sscanf(p, "%d", &H) != 1) { printf("[sched] Usage: tz <+HH:MM|-HH:MM>\r\n"); return true; } }
        if (H < 0 || H > 14 || M < 0 || M > 59) { printf("[sched] tz out of range\r\n"); return true; }
        schedule_set_tz(sign * (H * 60 + M));
        printf("[sched] tz set to %+03d:%02d\r\n", g_tz_min / 60, abs(g_tz_min) % 60);
        return true;
    }

    /* ---- calendar ---- */
    if (!strcmp(argv[0], "calendar") || !strcmp(argv[0], "cal")) {
        if (argc == 1) { print_calendar(); return true; }
        if (!strcmp(argv[1], "add") && argc >= 3) {
            int d, m;
            if (!sched_parse_ddmm(argv[2], &d, &m)) { printf("[calendar] bad date '%s' (use DD/MM, ** = any, e.g. 25/12, **/07, 01/**)\r\n", argv[2]); return true; }
            char err[64];
            if (calendar_add(d, m, err, sizeof(err))) { char b[8]; holiday_t e{ (uint8_t)d, (uint8_t)m }; fmt_ddmm(e, b, sizeof(b)); printf("[calendar] added [%d] %s\r\n", g_n_hol - 1, b); }
            else printf("[calendar] %s\r\n", err);
            return true;
        }
        if (!strcmp(argv[1], "rm") && argc >= 3) {
            int idx = atoi(argv[2]); char err[64];
            if (calendar_rm(idx, err, sizeof(err))) printf("[calendar] removed [%d]\r\n", idx); else printf("[calendar] %s\r\n", err);
            return true;
        }
        printf("[calendar] Usage: calendar | calendar add DD/MM | calendar rm <idx>\r\n");
        return true;
    }

    /* ---- sched ---- */
    if (!strcmp(argv[0], "sched") || !strcmp(argv[0], "schedule")) {
        if (argc == 1) { print_all_schedulers(); return true; }
        int n = atoi(argv[1]);
        if (n < 1 || n > SCHED_MAX) { printf("[sched] scheduler must be 1..%d\r\n", SCHED_MAX); return true; }
        n -= 1;
        if (argc == 2) { print_scheduler(n); return true; }

        if (!strcmp(argv[2], "cal")) {
            if (argc < 4 || (strcmp(argv[3], "on") && strcmp(argv[3], "off"))) { printf("[sched] Usage: sched %d cal on|off\r\n", n + 1); return true; }
            bool on = !strcmp(argv[3], "on"); char err[64];
            if (sched_set_cal(n, on, err, sizeof(err))) printf("[sched] scheduler %d calendar-control %s\r\n", n + 1, on ? "ON" : "off"); else printf("[sched] %s\r\n", err);
            return true;
        }

        int day = sched_day_index(argv[2]);
        if (day < 0) { printf("[sched] bad day '%s' (mon..sun, holiday)\r\n", argv[2]); return true; }
        if (argc >= 6 && !strcmp(argv[3], "add")) {
            int st = sched_parse_hhmm(argv[4]);
            int en = sched_parse_hhmm(argv[5]);
            if (st < 0 || en < 0) { printf("[sched] Usage: sched %d %s add HH:MM HH:MM  (end = last ON minute)\r\n", n + 1, argv[2]); return true; }
            char err[80];
            if (sched_add_event(n, day, st, en, err, sizeof(err))) {
                char a[8], b[8]; fmt_hhmm(st, a, sizeof(a)); fmt_hhmm(en, b, sizeof(b));
                printf("[sched] scheduler %d %s: added %s-%s (on at %s, off the minute after %s)\r\n", n + 1, DOW_NAME[day], a, b, a, b);
            } else printf("[sched] %s\r\n", err);
            return true;
        }
        if (argc >= 5 && !strcmp(argv[3], "rm")) {
            int idx = atoi(argv[4]); char err[64];
            if (sched_rm_event(n, day, idx, err, sizeof(err))) printf("[sched] scheduler %d %s: removed [%d]\r\n", n + 1, DOW_NAME[day], idx); else printf("[sched] %s\r\n", err);
            return true;
        }
        printf("[sched] Usage: sched <n> <day> add HH:MM HH:MM | sched <n> <day> rm <idx> | sched <n> cal on|off\r\n");
        return true;
    }

    return false;   /* not a schedule command */
}

/* ===================================================================== *
 *  Part 5 - config backup / restore
 * ===================================================================== */
cJSON *schedule_to_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "tz", g_tz_min);
    cJSON *sa = cJSON_AddArrayToObject(o, "schedulers");
    for (int n = 0; n < SCHED_MAX; ++n) {
        scheduler_t &s = g_sched[n];
        cJSON *so = cJSON_CreateObject();
        cJSON_AddBoolToObject(so, "used", s.used != 0);
        cJSON_AddBoolToObject(so, "cal",  s.cal_control != 0);
        cJSON *days = cJSON_AddArrayToObject(so, "days");
        for (int day = 0; day < SCHED_DAYS; ++day) {
            cJSON *evs = cJSON_CreateArray();
            for (int i = 0; i < s.day[day].n; ++i) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddNumberToObject(e, "s", s.day[day].ev[i].start_min);
                cJSON_AddNumberToObject(e, "e", s.day[day].ev[i].end_min);
                cJSON_AddItemToArray(evs, e);
            }
            cJSON_AddItemToArray(days, evs);
        }
        cJSON_AddItemToArray(sa, so);
    }
    cJSON *ca = cJSON_AddArrayToObject(o, "calendar");
    for (int i = 0; i < g_n_hol; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "d", g_hol[i].day);
        cJSON_AddNumberToObject(e, "m", g_hol[i].month);
        cJSON_AddItemToArray(ca, e);
    }
    return o;
}

void schedule_apply_json(const cJSON *obj)
{
    memset(g_sched, 0, sizeof(g_sched));
    memset(g_hol, 0, sizeof(g_hol));
    g_n_hol = 0;
    if (obj) {
        cJSON *tz = cJSON_GetObjectItem(obj, "tz");
        if (tz) g_tz_min = (int16_t)cJSON_GetNumberValue(tz);
        cJSON *sa = cJSON_GetObjectItem(obj, "schedulers"), *so;
        int n = 0;
        cJSON_ArrayForEach(so, sa) {
            if (n >= SCHED_MAX) break;
            scheduler_t &s = g_sched[n];
            s.used        = cJSON_IsTrue(cJSON_GetObjectItem(so, "used")) ? 1 : 0;
            s.cal_control = cJSON_IsTrue(cJSON_GetObjectItem(so, "cal"))  ? 1 : 0;
            cJSON *days = cJSON_GetObjectItem(so, "days"), *dd;
            int day = 0;
            cJSON_ArrayForEach(dd, days) {
                if (day >= SCHED_DAYS) break;
                cJSON *e; int cnt = 0;
                cJSON_ArrayForEach(e, dd) {
                    if (cnt >= SCHED_MAX_EV) break;
                    s.day[day].ev[cnt].start_min = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "s"));
                    s.day[day].ev[cnt].end_min   = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "e"));
                    cnt++;
                }
                s.day[day].n = (uint8_t)cnt;
                day++;
            }
            n++;
        }
        cJSON *ca = cJSON_GetObjectItem(obj, "calendar"), *ce;
        cJSON_ArrayForEach(ce, ca) {
            if (g_n_hol >= CAL_MAX) break;
            g_hol[g_n_hol].day   = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(ce, "d"));
            g_hol[g_n_hol].month = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(ce, "m"));
            g_n_hol++;
        }
    }
    sched_save();
}
