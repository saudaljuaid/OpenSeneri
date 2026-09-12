/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_UI_H
#define PHIPIA_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/keyboard.h>
#include <phipia/surface.h>

#define UI_DOCK_ITEM_COUNT 8U
#define UI_EVENT_QUEUE_CAPACITY 64U
#define UI_CURSOR_WIDTH 18U
#define UI_CURSOR_HEIGHT 25U
#define UI_CURSOR_HOTSPOT_X 0U
#define UI_CURSOR_HOTSPOT_Y 0U
#define UI_NATIVE_WINDOW_COUNT 4U
#define UI_NATIVE_TITLE_BYTES 32U

enum ui_status {
    UI_STATUS_OK = 0,
    UI_STATUS_NULL_ARGUMENT,
    UI_STATUS_ALREADY_INITIALIZED,
    UI_STATUS_NOT_INITIALIZED,
    UI_STATUS_NOT_ACTIVE,
    UI_STATUS_UNSUPPORTED_GEOMETRY,
    UI_STATUS_RECTANGLE_OVERFLOW,
    UI_STATUS_RECTANGLE_OUT_OF_BOUNDS,
    UI_STATUS_DUPLICATE_ELEMENT_ID,
    UI_STATUS_DOCK_OVERLAP,
    UI_STATUS_EMPTY_PANEL_CLIENT,
    UI_STATUS_TEXT_BASELINE_OUT_OF_BOUNDS,
    UI_STATUS_BAD_CURSOR_HOTSPOT,
    UI_STATUS_HIT_TEST_AMBIGUOUS,
    UI_STATUS_EVENT_QUEUE_FULL,
    UI_STATUS_BAD_EVENT,
    UI_STATUS_BAD_ELEMENT,
    UI_STATUS_BAD_PANEL,
    UI_STATUS_FONT_FAILURE,
    UI_STATUS_SURFACE_FAILURE,
    UI_STATUS_LOGO_FAILURE,
    UI_STATUS_MEDIA_EDITOR_ICON_FAILURE,
    UI_STATUS_APP_ICON_FAILURE,
    UI_STATUS_WALLPAPER_FAILURE,
    UI_STATUS_FILESYSTEM_FAILURE,
    UI_STATUS_SCREEN_FAILURE,
    UI_STATUS_INSTALLED_PROOF_FAILURE
};

enum ui_event_type {
    UI_EVENT_NONE = 0,
    UI_EVENT_POINTER_MOVEMENT,
    UI_EVENT_POINTER_BUTTON_PRESS,
    UI_EVENT_POINTER_BUTTON_RELEASE,
    UI_EVENT_KEYBOARD_FOCUS_NEXT,
    UI_EVENT_KEYBOARD_FOCUS_PREVIOUS,
    UI_EVENT_KEYBOARD_ACTIVATION,
    UI_EVENT_PANEL_CLOSE,
    UI_EVENT_TEXT_INPUT,
    UI_EVENT_TASK_MANAGER,
    UI_EVENT_REDRAW_REQUEST,
    UI_EVENT_TYPE_COUNT
};

enum ui_element_id {
    UI_ELEMENT_NONE = 0,
    UI_ELEMENT_DOCK_FILES,
    UI_ELEMENT_DOCK_TERMINAL,
    UI_ELEMENT_DOCK_NOTES,
    UI_ELEMENT_DOCK_MEDIA_EDITOR,
    UI_ELEMENT_DOCK_CAMERA,
    UI_ELEMENT_DOCK_CANVAS,
    UI_ELEMENT_DOCK_STORE,
    UI_ELEMENT_DOCK_SETTINGS,
    UI_ELEMENT_MENU_SEARCH,
    UI_ELEMENT_LAUNCHER_SEARCH,
    UI_ELEMENT_LAUNCHER_DISMISS,
    UI_ELEMENT_LAUNCHER_APP_0,
    UI_ELEMENT_LAUNCHER_APP_1,
    UI_ELEMENT_LAUNCHER_APP_2,
    UI_ELEMENT_LAUNCHER_APP_3,
    UI_ELEMENT_LAUNCHER_APP_4,
    UI_ELEMENT_LAUNCHER_APP_5,
    UI_ELEMENT_LAUNCHER_APP_6,
    UI_ELEMENT_LAUNCHER_APP_7,
    UI_ELEMENT_LAUNCHER_PAGE_0,
    UI_ELEMENT_LAUNCHER_PAGE_1,
    UI_ELEMENT_LAUNCHER_PAGE_2,
    UI_ELEMENT_LAUNCHER_PAGE_3,
    UI_ELEMENT_WINDOW_CLOSE,
    UI_ELEMENT_WINDOW_MAXIMIZE,
    UI_ELEMENT_WINDOW_MINIMIZE,
    UI_ELEMENT_FILES_UP,
    UI_ELEMENT_FILES_NEW_FILE,
    UI_ELEMENT_FILES_NEW_FOLDER,
    UI_ELEMENT_FILES_REFRESH,
    UI_ELEMENT_FILES_SYNC,
    UI_ELEMENT_FILES_ROOT,
    UI_ELEMENT_FILES_ENTRY_0,
    UI_ELEMENT_FILES_ENTRY_1,
    UI_ELEMENT_FILES_ENTRY_2,
    UI_ELEMENT_FILES_ENTRY_3,
    UI_ELEMENT_FILES_ENTRY_4,
    UI_ELEMENT_FILES_ENTRY_5,
    UI_ELEMENT_FILES_ENTRY_6,
    UI_ELEMENT_FILES_ENTRY_7,
    UI_ELEMENT_FILES_ENTRY_8,
    UI_ELEMENT_FILES_ENTRY_9,
    UI_ELEMENT_FILES_ENTRY_10,
    UI_ELEMENT_FILES_ENTRY_11,
    UI_ELEMENT_NOTES_NEW,
    UI_ELEMENT_MEDIA_EDITOR_NEW,
    UI_ELEMENT_MEDIA_EDITOR_IMPORT,
    UI_ELEMENT_MEDIA_EDITOR_TRIM,
    UI_ELEMENT_MEDIA_EDITOR_SAVE,
    UI_ELEMENT_MEDIA_EDITOR_EXPORT,
    UI_ELEMENT_MEDIA_EDITOR_TIMELINE,
    UI_ELEMENT_SETTINGS_BACK,
    UI_ELEMENT_SETTINGS_CATEGORY_0,
    UI_ELEMENT_SETTINGS_CATEGORY_1,
    UI_ELEMENT_SETTINGS_CATEGORY_2,
    UI_ELEMENT_SETTINGS_CATEGORY_3,
    UI_ELEMENT_SETTINGS_CATEGORY_4,
    UI_ELEMENT_SETTINGS_CATEGORY_5,
    UI_ELEMENT_SETTINGS_CATEGORY_6,
    UI_ELEMENT_SETTINGS_CATEGORY_7,
    UI_ELEMENT_SETTINGS_CATEGORY_8,
    UI_ELEMENT_SETTINGS_CATEGORY_9,
    UI_ELEMENT_SETTINGS_CATEGORY_10,
    UI_ELEMENT_SETTINGS_CATEGORY_11,
    UI_ELEMENT_SETTINGS_APPEARANCE_LIGHT,
    UI_ELEMENT_SETTINGS_APPEARANCE_DARK,
    UI_ELEMENT_SETTINGS_OPTION_0,
    UI_ELEMENT_SETTINGS_OPTION_1,
    UI_ELEMENT_SETTINGS_OPTION_2,
    UI_ELEMENT_SETTINGS_WALLPAPER_0,
    UI_ELEMENT_SETTINGS_WALLPAPER_1,
    UI_ELEMENT_SETTINGS_WALLPAPER_2,
    UI_ELEMENT_SETTINGS_WALLPAPER_3,
    UI_ELEMENT_SETTINGS_WALLPAPER_4,
    UI_ELEMENT_SETTINGS_WALLPAPER_5,
    UI_ELEMENT_SETTINGS_WALLPAPER_6,
    UI_ELEMENT_SETTINGS_WALLPAPER_7,
    UI_ELEMENT_SETTINGS_WALLPAPER_8,
    UI_ELEMENT_SETTINGS_WALLPAPER_9,
    UI_ELEMENT_SETTINGS_WALLPAPER_10,
    UI_ELEMENT_SETTINGS_WALLPAPER_11,
    UI_ELEMENT_SETTINGS_WALLPAPER_12,
    UI_ELEMENT_SETTINGS_WALLPAPER_13,
    UI_ELEMENT_CAMERA_CAPTURE,
    UI_ELEMENT_CAMERA_EFFECTS,
    UI_ELEMENT_CAMERA_BACKGROUNDS,
    UI_ELEMENT_CAMERA_CHOICE_0,
    UI_ELEMENT_CAMERA_CHOICE_1,
    UI_ELEMENT_CAMERA_CHOICE_2,
    UI_ELEMENT_CAMERA_CHOICE_3,
    UI_ELEMENT_CAMERA_CHOICE_4,
    UI_ELEMENT_CAMERA_CHOICE_5,
    UI_ELEMENT_CAMERA_CHOICE_6,
    UI_ELEMENT_CAMERA_CHOICE_7,
    UI_ELEMENT_CAMERA_CHOICE_8,
    UI_ELEMENT_STORE_SEARCH,
    UI_ELEMENT_STORE_NAV_0,
    UI_ELEMENT_STORE_NAV_1,
    UI_ELEMENT_STORE_NAV_2,
    UI_ELEMENT_STORE_NAV_3,
    UI_ELEMENT_STORE_NAV_4,
    UI_ELEMENT_STORE_NAV_5,
    UI_ELEMENT_STORE_NAV_6,
    UI_ELEMENT_STORE_NAV_7,
    UI_ELEMENT_STORE_NAV_8,
    UI_ELEMENT_STORE_NAV_9,
    UI_ELEMENT_STORE_NAV_10,
    UI_ELEMENT_STORE_NAV_11,
    UI_ELEMENT_STORE_NAV_12,
    UI_ELEMENT_STORE_PACKAGE_ACTION,
    UI_ELEMENT_COUNT
};

enum ui_panel_id {
    UI_PANEL_NONE = 0,
    UI_PANEL_FILES,
    UI_PANEL_TERMINAL,
    UI_PANEL_NOTES,
    UI_PANEL_MEDIA_EDITOR,
    UI_PANEL_CAMERA,
    UI_PANEL_PAINT,
    UI_PANEL_STORE,
    UI_PANEL_SETTINGS,
    UI_PANEL_TASKMGR,
    UI_PANEL_NATIVE_0,
    UI_PANEL_NATIVE_1,
    UI_PANEL_NATIVE_2,
    UI_PANEL_NATIVE_3,
    UI_PANEL_COUNT
};

enum ui_native_event_type {
    UI_NATIVE_EVENT_KEY = 1,
    UI_NATIVE_EVENT_POINTER_MOVE,
    UI_NATIVE_EVENT_POINTER_BUTTON,
    UI_NATIVE_EVENT_FOCUS,
    UI_NATIVE_EVENT_CLOSE
};

struct ui_native_event {
    enum ui_native_event_type type;
    uint64_t monotonic_ns;
    int32_t x;
    int32_t y;
    int32_t delta_x;
    int32_t delta_y;
    uint32_t code;
    uint32_t value;
    uint32_t modifiers;
};

typedef void (*ui_native_event_fn)(
    uint32_t slot,
    const struct ui_native_event *event,
    void *context
);

enum ui_action {
    UI_ACTION_NONE = 0,
    UI_ACTION_OPEN_FILES,
    UI_ACTION_OPEN_TERMINAL,
    UI_ACTION_OPEN_NOTES,
    UI_ACTION_OPEN_MEDIA_EDITOR,
    UI_ACTION_OPEN_CAMERA,
    UI_ACTION_OPEN_CANVAS,
    UI_ACTION_OPEN_STORE,
    UI_ACTION_OPEN_SETTINGS,
    UI_ACTION_COUNT
};

enum ui_pointer_button {
    UI_POINTER_BUTTON_NONE = 0,
    UI_POINTER_BUTTON_LEFT,
    UI_POINTER_BUTTON_MIDDLE,
    UI_POINTER_BUTTON_RIGHT,
    UI_POINTER_BUTTON_COUNT
};

struct ui_point {
    int32_t x;
    int32_t y;
};

/* Half-open: left/top belong to the rectangle; right/bottom do not. */
struct ui_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct ui_event {
    enum ui_event_type type;
    struct ui_point point;
    enum ui_pointer_button button;
    char character;
    bool control;
};

struct ui_theme {
    uint32_t white;
    uint32_t ink;
    uint32_t desktop_dark;
    uint32_t desktop_light;
    uint32_t title_active;
    uint32_t title_inactive;
    uint32_t accent_teal;
    uint32_t accent_gold;
    uint32_t accent_green;
    uint32_t accent_red;
    uint32_t accent_violet;
    uint32_t shadow;
    uint32_t window_face;
};

struct ui_dock_item {
    enum ui_element_id id;
    const char *label;
    struct ui_rect bounds;
    struct ui_rect icon_bounds;
    enum ui_action action;
    enum ui_panel_id panel;
};

struct ui_layout {
    struct ui_rect surface;
    struct ui_rect menu_bar;
    struct ui_rect workspace_bar;
    struct ui_rect hero_window;
    struct ui_rect logo;
    struct ui_rect wordmark;
    struct ui_rect motto;
    struct ui_rect version_label;
    struct ui_rect dock;
    struct ui_dock_item dock_items[UI_DOCK_ITEM_COUNT];
    struct ui_rect panel;
    struct ui_rect panel_client;
    uint32_t menu_baseline;
    uint32_t hero_title_baseline;
    uint32_t title_baseline;
    uint32_t motto_baseline;
    uint32_t version_baseline;
    uint32_t dock_label_baseline;
    uint32_t panel_title_baseline;
    uint32_t panel_text_baseline;
};

struct ui_event_counters {
    uint64_t accepted;
    uint64_t drained;
    uint64_t coalesced;
    uint64_t dropped;
};

struct ui_render_counters {
    uint64_t full_draws;
    uint64_t damaged_draws;
    uint64_t pixels_copied;
    uint64_t cursor_moves;
    uint64_t dock_state_changes;
    uint64_t panel_transitions;
    uint64_t damage_rectangles;
    uint64_t glyphs;
};

struct ui_state {
    bool initialized;
    bool active;
    bool pointer_present;
    bool ledger_pass;
    struct ui_point pointer;
    enum ui_element_id focus;
    enum ui_element_id hover;
    enum ui_element_id pressed;
    enum ui_panel_id active_panel;
    struct ui_theme theme;
    struct ui_layout layout;
    struct ui_event_counters events;
    struct ui_render_counters renders;
    uint64_t stable_render_hash;
};

struct ui_proof {
    uint32_t width;
    uint32_t height;
    uint32_t dock_items;
    uint64_t events;
    uint64_t panels;
    uint64_t cursor_moves;
    uint64_t damage_rectangles;
    uint64_t glyphs;
    uint64_t ledger_fingerprint;
    uint64_t render_hash;
};

enum ui_status ui_layout_build(
    uint32_t width,
    uint32_t height,
    struct ui_layout *layout
);
enum ui_status ui_layout_validate(const struct ui_layout *layout);
enum ui_status ui_hit_test(
    const struct ui_layout *layout,
    struct ui_point point,
    enum ui_element_id *element
);

enum ui_status ui_construct(bool pointer_present);
enum ui_status ui_activate(void);
enum ui_status ui_terminal_draw_logo(void);
bool ui_is_active(void);
void ui_animation_attach(void);
bool ui_animation_active(void);
const struct ui_state *ui_get_state(void);

enum ui_status ui_event_publish(const struct ui_event *event);
enum ui_status ui_handle_keyboard(const struct keyboard_event *event);
enum ui_status ui_process_events(void);
enum ui_status ui_flush(void);

enum ui_status ui_native_window_open(
    uint32_t slot,
    const char *title,
    const uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    ui_native_event_fn event_handler,
    void *context
);
enum ui_status ui_native_window_close(uint32_t slot);
enum ui_status ui_native_window_damage(
    uint32_t slot,
    const struct ui_rect *rectangles,
    size_t rectangle_count
);
enum ui_status ui_native_pointer_capture(uint32_t slot, bool capture);
bool ui_native_window_is_open(uint32_t slot);
bool ui_application_launch_dequeue(char *manifest_path, size_t capacity);

bool ui_self_test(void);
const char *ui_self_test_failure(void);
enum ui_status ui_verify_installed(struct ui_proof *proof);
const char *ui_installed_proof_failure(void);
const char *ui_status_string(enum ui_status status);
const char *ui_panel_name(enum ui_panel_id panel);
const char *ui_element_name(enum ui_element_id element);

#endif
