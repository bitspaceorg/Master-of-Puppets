/*
 * Master of Puppets — Examples
 * ui_toolbar.h — Header-only SDL3 left-sidebar button toolbar
 *
 * Usage:
 *   #define UI_TOOLBAR_IMPLEMENTATION
 *   #include "ui_toolbar.h"     // in exactly one .c file
 *
 *   // or just include it — all functions are static inline
 *
 * Button types:
 *   UI_BTN_TOGGLE     on/off toggle (retains state)
 *   UI_BTN_MOMENTARY  fires once per click, does not retain state
 *   UI_BTN_RADIO      mutually exclusive within a radio_group
 *
 * Event consumption:
 *   ui_toolbar_event() returns true when the mouse is inside the sidebar
 *   area, preventing the event from being forwarded to MOP (no accidental
 *   camera orbits when clicking buttons).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_TOOLBAR_H
#define UI_TOOLBAR_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define UI_SIDEBAR_WIDTH   180
#define UI_BTN_HEIGHT       28
#define UI_SECTION_HEIGHT   24
#define UI_BTN_MARGIN        3
#define UI_BTN_PAD_LEFT     10
#define UI_BTN_PAD_TOP      10
#define UI_TEXT_SCALE      1.5f

#define UI_MAX_BUTTONS      48
#define UI_MAX_SECTIONS     16

/* -------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

typedef enum {
    UI_BTN_TOGGLE,
    UI_BTN_MOMENTARY,
    UI_BTN_RADIO
} UiBtnType;

typedef struct {
    const char *label;
    UiBtnType   type;
    int         radio_group;   /* only for UI_BTN_RADIO */
    bool        on;            /* current state (toggle/radio) */
    bool        fired;         /* momentary: true for one frame after click */
    float       y;             /* computed y position */
} UiButton;

typedef struct {
    const char *label;
    float       y;             /* computed y position */
} UiSection;

typedef struct {
    UiButton   buttons[UI_MAX_BUTTONS];
    int        button_count;
    UiSection  sections[UI_MAX_SECTIONS];
    int        section_count;

    /* Layout items in order (for rendering + hit testing) */
    enum { UI_ITEM_SECTION, UI_ITEM_BUTTON } items[UI_MAX_BUTTONS + UI_MAX_SECTIONS];
    int        item_idx[UI_MAX_BUTTONS + UI_MAX_SECTIONS]; /* index into buttons[] or sections[] */
    int        item_count;

    int        hovered;        /* button index under cursor, -1 if none */
    int        sidebar_height; /* total content height */
} UiToolbar;

/* -------------------------------------------------------------------------
 * API (all static inline)
 * ------------------------------------------------------------------------- */

static inline void ui_toolbar_init(UiToolbar *tb) {
    memset(tb, 0, sizeof(*tb));
    tb->hovered = -1;
}

/* Add a section header. */
static inline void ui_toolbar_section(UiToolbar *tb, const char *label) {
    if (tb->section_count >= UI_MAX_SECTIONS) return;
    int si = tb->section_count++;
    tb->sections[si].label = label;

    int ii = tb->item_count++;
    tb->items[ii]    = UI_ITEM_SECTION;
    tb->item_idx[ii] = si;
}

/* Add a button. Returns its index (for querying state). */
static inline int ui_toolbar_button(UiToolbar *tb, const char *label,
                                     UiBtnType type, int radio_group,
                                     bool initial_on) {
    if (tb->button_count >= UI_MAX_BUTTONS) return -1;
    int bi = tb->button_count++;
    tb->buttons[bi].label       = label;
    tb->buttons[bi].type        = type;
    tb->buttons[bi].radio_group = radio_group;
    tb->buttons[bi].on          = initial_on;
    tb->buttons[bi].fired       = false;

    int ii = tb->item_count++;
    tb->items[ii]    = UI_ITEM_BUTTON;
    tb->item_idx[ii] = bi;

    return bi;
}

/* Compute layout positions. Call once after adding all items, or after
 * window resize. */
static inline void ui_toolbar_layout(UiToolbar *tb) {
    float y = (float)UI_BTN_PAD_TOP;

    for (int i = 0; i < tb->item_count; i++) {
        if (tb->items[i] == UI_ITEM_SECTION) {
            tb->sections[tb->item_idx[i]].y = y;
            y += (float)UI_SECTION_HEIGHT;
        } else {
            tb->buttons[tb->item_idx[i]].y = y;
            y += (float)UI_BTN_HEIGHT + (float)UI_BTN_MARGIN;
        }
    }
    tb->sidebar_height = (int)(y + (float)UI_BTN_PAD_TOP);
}

/* Query button state. */
static inline bool ui_toolbar_is_on(const UiToolbar *tb, int btn_idx) {
    if (btn_idx < 0 || btn_idx >= tb->button_count) return false;
    return tb->buttons[btn_idx].on;
}

/* Query momentary fire (auto-clears after reading). */
static inline bool ui_toolbar_fired(UiToolbar *tb, int btn_idx) {
    if (btn_idx < 0 || btn_idx >= tb->button_count) return false;
    bool f = tb->buttons[btn_idx].fired;
    tb->buttons[btn_idx].fired = false;
    return f;
}

/* Set button state programmatically. */
static inline void ui_toolbar_set(UiToolbar *tb, int btn_idx, bool on) {
    if (btn_idx < 0 || btn_idx >= tb->button_count) return;
    tb->buttons[btn_idx].on = on;
}

/* Toggle a button (respects type: toggle flips, radio selects, momentary fires). */
static inline void ui_toolbar_toggle(UiToolbar *tb, int btn_idx) {
    if (btn_idx < 0 || btn_idx >= tb->button_count) return;
    UiButton *btn = &tb->buttons[btn_idx];
    switch (btn->type) {
    case UI_BTN_TOGGLE:
        btn->on = !btn->on;
        break;
    case UI_BTN_MOMENTARY:
        btn->fired = true;
        break;
    case UI_BTN_RADIO:
        for (int j = 0; j < tb->button_count; j++) {
            if (tb->buttons[j].type == UI_BTN_RADIO &&
                tb->buttons[j].radio_group == btn->radio_group)
                tb->buttons[j].on = false;
        }
        btn->on = true;
        break;
    }
}

/* Select a radio button by index (turns off others in the same group). */
static inline void ui_toolbar_radio_select(UiToolbar *tb, int btn_idx) {
    if (btn_idx < 0 || btn_idx >= tb->button_count) return;
    UiButton *btn = &tb->buttons[btn_idx];
    if (btn->type != UI_BTN_RADIO) return;
    for (int j = 0; j < tb->button_count; j++) {
        if (tb->buttons[j].type == UI_BTN_RADIO &&
            tb->buttons[j].radio_group == btn->radio_group)
            tb->buttons[j].on = false;
    }
    btn->on = true;
}

/* Process an SDL event. Returns true if the event was consumed by the
 * sidebar (mouse was in the sidebar area). */
static inline bool ui_toolbar_event(UiToolbar *tb, const SDL_Event *ev) {
    float mx = 0, my = 0;

    switch (ev->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        mx = ev->button.x; my = ev->button.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        mx = ev->button.x; my = ev->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mx = ev->motion.x; my = ev->motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        SDL_GetMouseState(&mx, &my);
        break;
    default:
        return false;
    }

    /* Is mouse inside sidebar? */
    if (mx >= (float)UI_SIDEBAR_WIDTH) {
        tb->hovered = -1;
        return false;
    }

    /* Find hovered button */
    tb->hovered = -1;
    for (int i = 0; i < tb->button_count; i++) {
        float by = tb->buttons[i].y;
        if (my >= by && my < by + (float)UI_BTN_HEIGHT &&
            mx >= (float)UI_BTN_PAD_LEFT &&
            mx < (float)(UI_SIDEBAR_WIDTH - UI_BTN_PAD_LEFT)) {
            tb->hovered = i;
            break;
        }
    }

    /* Handle clicks */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT && tb->hovered >= 0) {

        UiButton *btn = &tb->buttons[tb->hovered];
        switch (btn->type) {
        case UI_BTN_TOGGLE:
            btn->on = !btn->on;
            break;
        case UI_BTN_MOMENTARY:
            btn->fired = true;
            break;
        case UI_BTN_RADIO:
            /* Turn off all others in same group */
            for (int j = 0; j < tb->button_count; j++) {
                if (tb->buttons[j].type == UI_BTN_RADIO &&
                    tb->buttons[j].radio_group == btn->radio_group) {
                    tb->buttons[j].on = false;
                }
            }
            btn->on = true;
            break;
        }
    }

    return true; /* consumed — don't forward to MOP */
}

/* Render the sidebar. Call after MOP blit, before SDL_RenderPresent. */
static inline void ui_toolbar_render(const UiToolbar *tb, SDL_Renderer *r,
                                      int win_h) {
    /* Sidebar background */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_FRect bg = { 0, 0, (float)UI_SIDEBAR_WIDTH, (float)win_h };
    SDL_SetRenderDrawColor(r, 28, 28, 32, 230);
    SDL_RenderFillRect(r, &bg);

    /* Sidebar right edge */
    SDL_SetRenderDrawColor(r, 60, 60, 68, 255);
    SDL_RenderLine(r, (float)UI_SIDEBAR_WIDTH - 1, 0,
                       (float)UI_SIDEBAR_WIDTH - 1, (float)win_h);

    for (int i = 0; i < tb->item_count; i++) {
        if (tb->items[i] == UI_ITEM_SECTION) {
            /* Section header */
            const UiSection *s = &tb->sections[tb->item_idx[i]];
            SDL_SetRenderDrawColor(r, 140, 140, 155, 255);
            SDL_SetRenderScale(r, UI_TEXT_SCALE, UI_TEXT_SCALE);
            SDL_RenderDebugText(r,
                (float)UI_BTN_PAD_LEFT / UI_TEXT_SCALE,
                (s->y + 4.0f) / UI_TEXT_SCALE,
                s->label);
            SDL_SetRenderScale(r, 1.0f, 1.0f);
        } else {
            /* Button */
            int bi = tb->item_idx[i];
            const UiButton *btn = &tb->buttons[bi];

            SDL_FRect br = {
                (float)UI_BTN_PAD_LEFT,
                btn->y,
                (float)(UI_SIDEBAR_WIDTH - 2 * UI_BTN_PAD_LEFT),
                (float)UI_BTN_HEIGHT
            };

            /* Button background */
            if (btn->on && (btn->type == UI_BTN_TOGGLE || btn->type == UI_BTN_RADIO)) {
                /* Active state — bright accent */
                if (bi == tb->hovered)
                    SDL_SetRenderDrawColor(r, 75, 130, 195, 255);
                else
                    SDL_SetRenderDrawColor(r, 55, 110, 175, 255);
            } else if (bi == tb->hovered) {
                SDL_SetRenderDrawColor(r, 55, 55, 62, 255);
            } else {
                SDL_SetRenderDrawColor(r, 42, 42, 48, 255);
            }
            SDL_RenderFillRect(r, &br);

            /* Button border */
            SDL_SetRenderDrawColor(r, 70, 70, 78, 255);
            SDL_RenderRect(r, &br);

            /* Button label */
            if (btn->on && (btn->type == UI_BTN_TOGGLE || btn->type == UI_BTN_RADIO))
                SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            else
                SDL_SetRenderDrawColor(r, 180, 180, 190, 255);

            SDL_SetRenderScale(r, UI_TEXT_SCALE, UI_TEXT_SCALE);
            SDL_RenderDebugText(r,
                ((float)UI_BTN_PAD_LEFT + 8.0f) / UI_TEXT_SCALE,
                (btn->y + 8.0f) / UI_TEXT_SCALE,
                btn->label);
            SDL_SetRenderScale(r, 1.0f, 1.0f);
        }
    }
}

#endif /* UI_TOOLBAR_H */
