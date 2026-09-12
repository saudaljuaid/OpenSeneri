/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Task Manager.
 *
 * Windows 10's is the shape this copies: a tab strip over a table, the
 * numeric columns HEAT-MAPPED so a machine under load is legible at a
 * glance rather than by reading numbers, an aggregate percentage in each
 * column heading, and End task in the bottom right.  Its Processes tab
 * groups rows under "Apps" and "Background processes" and sorts by any
 * column, which is most of what makes it usable rather than merely
 * informative.
 *
 * Three tabs, not seven.  Windows has Processes, Performance, App history,
 * Startup, Users, Details and Services; four of those are views this shell
 * has nothing behind - there is no per-app history, no second user, no
 * service manager - and a tab that opens onto an apology is worse than a
 * tab that is not there.  Processes, Performance and Startup are the three
 * with something real to show.
 *
 * WHAT IT TAKES FROM ELSEWHERE, and why each one earns its place:
 *
 *   From WINDOWS 10: the whole frame.  Tabs, the grouped table, the heat
 *   map, the per-column aggregate in the heading, End task pinned bottom
 *   right, and "Fewer details" bottom left.  This is a copy of that window
 *   before it is anything else.
 *
 *   From macOS's Activity Monitor: the FOOTER.  Windows tells you a
 *   process count and nothing else; Activity Monitor keeps a live summary
 *   bar across the bottom, so the totals are readable without reading the
 *   table.  That bar is here, with the counts and the two headline
 *   percentages.
 *
 *   From htop: the PER-CORE METERS.  Windows 10's Performance tab can show
 *   logical processors, but only by right-clicking into a mode that
 *   replaces the main graph; htop shows every core at once, always, as a
 *   row of small bars, and it is the single fastest way to see that one
 *   core is pinned while seven idle.  They sit under the graph here rather
 *   than instead of it.
 *
 *   From PHIPIA, which is this shell's own: processes carry COLOUR by
 *   kind, the way File Explorer's rows carry it by file type - an
 *   application, a background task and a system process are three
 *   different colours rather than three identical grey lines.  And a
 *   selected row takes the accent bar every other list in this shell uses.
 *
 * THE DATA IS THE CALLER'S.  This window has no scheduler to ask and does
 * not pretend to: every row, every percentage and every point of graph
 * history arrives through taskmgr_set_process(), taskmgr_set_meter() and
 * taskmgr_set_core().  Nothing here is invented, sampled or estimated, and
 * a caller that sets nothing gets an empty table that says so rather than
 * a plausible-looking machine.
 *
 * Percentages are TENTHS of a per cent - 0 to 1000 - because src/kernel is
 * built with -msoft-float and no floating point, and "23.4%" has to come
 * from somewhere.
 */
#ifndef PHIPIA_TASKMGR_H
#define PHIPIA_TASKMGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cursor.h>
#include <phipia/surface.h>
#include <phipia/ui.h>

#define TASKMGR_MAX_PROCESSES 28U
#define TASKMGR_MAX_CORES 16U
#define TASKMGR_MAX_STARTUP 10U
/* Windows 10's graph is sixty seconds wide, one sample a second. */
#define TASKMGR_HISTORY 60U
#define TASKMGR_NAME_BYTES 40U
#define TASKMGR_FIELD_BYTES 24U

enum taskmgr_status {
    TASKMGR_STATUS_OK = 0,
    TASKMGR_STATUS_NULL_ARGUMENT,
    TASKMGR_STATUS_NOT_INITIALIZED,
    TASKMGR_STATUS_BAD_INDEX,
    TASKMGR_STATUS_UNSUPPORTED_GEOMETRY,
    TASKMGR_STATUS_SURFACE_FAILURE
};

enum taskmgr_tab {
    TASKMGR_TAB_PROCESSES = 0,
    TASKMGR_TAB_PERFORMANCE,
    TASKMGR_TAB_STARTUP,
    TASKMGR_TAB_COUNT
};

/*
 * What a process IS, which decides its colour.  Windows draws all three
 * the same and separates them only by which group they are listed under;
 * this keeps the groups and colours them too, so the kind survives being
 * sorted into a flat list by CPU.
 */
enum taskmgr_kind {
    TASKMGR_APP = 0,          /* something with a window */
    TASKMGR_BACKGROUND,       /* running, no window */
    TASKMGR_SYSTEM,           /* the kernel's own */
    TASKMGR_KIND_COUNT
};

/* Which column the table is sorted by.  Windows sorts by any of them and
 * marks the one in use with an arrow in its heading; so does this. */
enum taskmgr_column {
    TASKMGR_COLUMN_NAME = 0,
    TASKMGR_COLUMN_STATUS,
    TASKMGR_COLUMN_CPU,
    TASKMGR_COLUMN_MEMORY,
    TASKMGR_COLUMN_DISK,
    TASKMGR_COLUMN_NETWORK,
    TASKMGR_COLUMN_COUNT
};

/* The four resources the Performance tab graphs, in the order Windows
 * stacks them down its left rail. */
enum taskmgr_resource {
    TASKMGR_RESOURCE_CPU = 0,
    TASKMGR_RESOURCE_MEMORY,
    TASKMGR_RESOURCE_DISK,
    TASKMGR_RESOURCE_NETWORK,
    TASKMGR_RESOURCE_COUNT
};

/*
 * One row.  `heading` makes it a group divider instead - "Apps (4)" - which
 * is how Windows separates windowed processes from the rest, and the
 * divider carries no numbers of its own.
 */
struct taskmgr_process {
    bool present;
    bool heading;
    enum taskmgr_kind kind;
    char name[TASKMGR_NAME_BYTES];
    char status[TASKMGR_FIELD_BYTES];
    /* Application artwork by name, resolved through taskbar_artwork(); a
     * Lucide mark by name is the fallback when there is no picture. */
    const char *art;
    const char *glyph;
    uint32_t pid;
    uint16_t threads;
    uint16_t cpu_tenths;      /* 0 - 1000, so 234 is 23.4% */
    uint32_t memory_kb;
    uint32_t disk_kb_s;
    uint32_t network_kb_s;
};

/* One resource's live figure and its sixty seconds of history.  `history`
 * is whole per cent, newest LAST, which is the direction the graph is
 * drawn and the direction a ring buffer fills. */
struct taskmgr_meter {
    bool present;
    char detail[TASKMGR_FIELD_BYTES];   /* "8.0 GB", "1 Gbps" */
    uint16_t percent_tenths;
    uint32_t used;                       /* in the unit `detail` names */
    uint32_t total;
    uint8_t history[TASKMGR_HISTORY];
};

/* A Startup row.  Impact is the four-way Windows grades it in. */
enum taskmgr_impact {
    TASKMGR_IMPACT_NONE = 0,
    TASKMGR_IMPACT_LOW,
    TASKMGR_IMPACT_MEDIUM,
    TASKMGR_IMPACT_HIGH,
    TASKMGR_IMPACT_COUNT
};

struct taskmgr_startup {
    bool present;
    bool enabled;
    enum taskmgr_impact impact;
    char name[TASKMGR_NAME_BYTES];
    char publisher[TASKMGR_NAME_BYTES];
    const char *art;
    const char *glyph;
};

const char *taskmgr_status_string(enum taskmgr_status status);

enum taskmgr_status taskmgr_initialize(struct surface *canvas,
    struct ui_rect frame);
enum taskmgr_status taskmgr_set_frame(struct ui_rect frame);
struct ui_rect taskmgr_bounds(void);

enum taskmgr_status taskmgr_set_process(size_t index,
    const struct taskmgr_process *process);
enum taskmgr_status taskmgr_set_meter(enum taskmgr_resource resource,
    const struct taskmgr_meter *meter);
enum taskmgr_status taskmgr_set_startup(size_t index,
    const struct taskmgr_startup *entry);
/* How many logical processors to draw meters for, and each one's load.
 * Setting a count of zero leaves the per-core row out entirely rather than
 * drawing an empty one. */
enum taskmgr_status taskmgr_set_core_count(size_t count);
enum taskmgr_status taskmgr_set_core(size_t index, uint16_t percent_tenths);
enum taskmgr_status taskmgr_set_uptime(uint32_t seconds);
enum taskmgr_status taskmgr_set_focus(bool focused);

/*
 * Open and close.
 *
 * Task Manager is the one window here a person opens and closes rather than
 * one that is simply there, and the close mark in its caption now does what
 * it is drawn as.  A closed window draws nothing, takes no pointer and
 * reports the rectangle it vacated as damage.  It keeps its tab, its sort
 * and its selection across a close, the way Windows' does.
 *
 * The taskbar button is the COMPOSITOR'S to add and remove: this window
 * knows whether it is open, and taskbar_set_app() is how that becomes a
 * button.  Nothing here reaches into the bar, because a window that puts
 * itself on the taskbar is a window that can lie about being there.
 */
enum taskmgr_status taskmgr_open(struct ui_rect *damage);
enum taskmgr_status taskmgr_close(struct ui_rect *damage);
bool taskmgr_is_open(void);

enum taskmgr_status taskmgr_set_tab(enum taskmgr_tab tab);
enum taskmgr_tab taskmgr_get_tab(void);
/* Which resource the big graph is showing.  The left rail's tiles pick it;
 * this is the same choice made from code. */
enum taskmgr_status taskmgr_set_resource(enum taskmgr_resource resource);
/*
 * Sort the table.  Asking for the column it is already sorted by REVERSES
 * it, which is what clicking a heading twice does in Windows and in every
 * other table worth using.  Name sorts A-Z first; every numeric column
 * sorts heaviest-first, because "what is eating the machine" is the
 * question the window is open to answer.
 */
enum taskmgr_status taskmgr_sort_by(enum taskmgr_column column);
enum taskmgr_column taskmgr_sort_column(void);
bool taskmgr_sort_descending(void);

/*
 * End task.  Removes the selected row and reports which name went, so a
 * compositor can act on it - this window has no scheduler to kill anything
 * in, and pretending otherwise would make the button a lie.  Returns false
 * when nothing was selected, or when the selection is a group heading,
 * which is not a task and cannot be ended.
 */
bool taskmgr_end_task(char *name_out, size_t name_bytes,
    struct ui_rect *damage);
/* Pointer-driven End task presses are queued here for the compositor.  The
 * row disappears immediately, and the caller consumes its stable name once
 * to terminate the corresponding real application. */
bool taskmgr_take_ended_task(char *name_out, size_t name_bytes);
/* Which row is selected, or SIZE_MAX for none. */
size_t taskmgr_selection(void);

/* Which pointer belongs at a point - the same question every window in this
 * shell answers for itself; see explorer_cursor_at(). */
enum cursor_kind taskmgr_cursor_at(struct ui_point point);

enum taskmgr_status taskmgr_pointer_move(struct ui_point point,
    struct ui_rect *damage);
enum taskmgr_status taskmgr_pointer_press(struct ui_point point,
    struct ui_rect *damage);
enum taskmgr_status taskmgr_key_escape(struct ui_rect *damage);

bool taskmgr_animate(struct ui_rect *damage);
bool taskmgr_animating(void);

enum taskmgr_status taskmgr_draw(struct ui_rect damage);

bool taskmgr_self_test(void);
const char *taskmgr_self_test_failure(void);

#endif
