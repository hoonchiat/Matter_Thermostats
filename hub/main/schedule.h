/*
 * schedule.h - time-of-day scheduling for the Matter Hub (v1.8).
 *
 * Up to 4 independent SCHEDULERS, each a weekly schedule (Mon..Sun) plus a
 * HOLIDAY day-schedule, with up to 4 non-overlapping events per day. One shared
 * CALENDAR of public-holiday dates (DD/MM, with `**` wildcards). Each scheduler
 * can enable/disable "calendar control": when on, a public-holiday date makes the
 * scheduler use its holiday day-schedule instead of the weekday one.
 *
 * The logic engine (logic.cpp) consumes two booleans from here as rule
 * conditions: `schedule_on(n)` (scheduler n active at the current local time) and
 * `calendar_on()` (today is a public holiday). All times are LOCAL:
 * local = UTC (system clock, set by `settime`) + tz offset. If the clock was
 * never set this boot both read false (fail-safe - never fire on a wrong time).
 *
 * Additive: no CHIP/esp-matter types here; commissioning path untouched.
 */
#pragma once

#include <cstdint>
#include <cstddef>

struct cJSON;

/* Capacities (see also the DD/MM wildcard note on the calendar). */
#define SCHED_MAX        4      /* independent schedulers                        */
#define SCHED_DAYS       8      /* day[0..6] = Mon..Sun, day[7] = holiday        */
#define SCHED_HOLIDAY    7      /* index of the holiday day-schedule             */
#define SCHED_MAX_EV     4      /* events per day-schedule                       */
#define CAL_MAX          20     /* public-holiday dates (shared)                 */

/* --- lifecycle ------------------------------------------------------------- */
void schedule_load(void);          /* load schedulers + calendar + tz from NVS  */

/* --- evaluation (called from logic.cpp) ------------------------------------ */
/* Scheduler n (0-based) active at the current local time (holiday-aware if its
 * calendar control is on). False if n invalid/unused or the clock isn't set. */
bool schedule_on(int n);
/* Today (local) matches a calendar public-holiday entry. False if clock unset. */
bool calendar_on(void);
/* Has the system clock been set this boot (>= 2020)? */
bool schedule_clock_ready(void);

/* --- timezone (local = UTC + offset) --------------------------------------- */
int  schedule_tz_offset_min(void);        /* minutes east of UTC               */
void schedule_set_tz(int minutes);        /* set + persist                     */

/* --- console dispatch: sched / schedule / calendar / cal / tz -------------- */
bool schedule_handle_cmd(const char *line);
void schedule_print_help(void);

/* --- structured entry points (shared by the console + the JSON protocol) ----
 * day: 0..6 = Mon..Sun, 7 = holiday. Times are minutes since local midnight,
 * inclusive: an event is "on" while start <= now <= end (so 08:00..08:59 is on
 * at 08:00 and off at 09:00). All persist on success. Return true on success;
 * on failure fill `err` (may be null) and return false. */
bool sched_add_event(int n, int day, int start_min, int end_min, char *err, size_t errcap);
bool sched_rm_event(int n, int day, int idx, char *err, size_t errcap);
bool sched_set_cal(int n, bool on, char *err, size_t errcap);
/* Calendar date; day/month == 0 means wildcard (`**`) on that field. */
bool calendar_add(int day, int month, char *err, size_t errcap);
bool calendar_rm(int idx, char *err, size_t errcap);

/* Parse "HH:MM" -> minutes 0..1439 (returns -1 on error). */
int  sched_parse_hhmm(const char *s);
/* Parse "DD/MM" (with `**` wildcards) -> day/month (0 = wildcard); false on error. */
bool sched_parse_ddmm(const char *s, int *day, int *month);
/* Map a day token ("mon".."sun","holiday"/"hol") -> 0..7, or -1. */
int  sched_day_index(const char *tok);

/* --- config backup / restore ----------------------------------------------- */
/* Serialise tz + all schedulers + calendar to a cJSON object. Caller owns it. */
cJSON *schedule_to_json(void);
/* Rebuild tz + schedulers + calendar from a backup object (missing => cleared).
 * Writes NVS. */
void   schedule_apply_json(const cJSON *obj);
