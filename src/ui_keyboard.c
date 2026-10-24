#include "ui_keyboard.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "api.h"
#include "defines.h"
#include "module_common.h"
#include "ui_fonts.h"
#include "ui_utils.h"

/* 4 rows of 10 character keys, then one row of 5 wide action keys. */
#define KB_COLS       10
#define KB_CHAR_ROWS   4
#define KB_ROWS       (KB_CHAR_ROWS + 1)
#define KB_ACTIONS     5

typedef enum { LAYOUT_LOWER = 0, LAYOUT_UPPER, LAYOUT_SYMBOL, LAYOUT_COUNT } KbLayout;

static const char *const k_layouts[LAYOUT_COUNT][KB_CHAR_ROWS] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@" },
    { "!#$%&'*+/=", "?^`{|}~()\"", ":;<>[]\\,_-", "1234567.@ " },
};

enum { ACT_LAYOUT = 0, ACT_PORT, ACT_SPACE, ACT_DEL, ACT_DONE };
static const char *const k_layout_labels[LAYOUT_COUNT] = { "ABC", "#+=", "abc" };

static void append_str(char *buf, int buf_size, const char *s)
{
    int len = (int)strlen(buf);
    int add = (int)strlen(s);
    if (len + add >= buf_size) return;
    memcpy(buf + len, s, add + 1);
}

static void backspace(char *buf)
{
    int len = (int)strlen(buf);
    if (len == 0) return;
    /* Remove a whole UTF-8 code point */
    len--;
    while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
    buf[len] = '\0';
}

static void draw_text_centered(SDL_Surface *screen, TTF_Font *font, const char *text,
                               SDL_Color color, int cx, int cy)
{
    if (!text || !*text) return;
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, color);
    if (!s) return;
    SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){cx - s->w / 2, cy - s->h / 2});
    SDL_FreeSurface(s);
}

static void render(SDL_Surface *screen, const char *title, const char *buf,
                   bool masked, uint32_t last_type_ms, KbLayout layout,
                   int row, int col, int show_setting)
{
    int hw = screen->w;
    int hh = screen->h;
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    render_screen_header(screen, title, show_setting);

    /* --- Text field --- */
    int pad     = SCALE1(PADDING);
    int field_y = SCALE1(PILL_SIZE + PADDING * 2);
    int field_h = SCALE1(PILL_SIZE);
    render_rounded_rect_bg(screen, pad, field_y, hw - pad * 2, field_h,
                           SDL_MapRGB(screen->format, 0x2A, 0x2A, 0x2A));

    char shown[512];
    if (masked) {
        int n = (int)strlen(buf), o = 0;
        bool reveal_last = n > 0 && (SDL_GetTicks() - last_type_ms) < 800;
        for (int i = 0; i < n && o < (int)sizeof(shown) - 4; i++) {
            if (reveal_last && i == n - 1) shown[o++] = buf[i];
            else { memcpy(shown + o, "\xE2\x80\xA2", 3); o += 3; }  /* bullet */
        }
        shown[o] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", buf);
    }
    strncat(shown, "_", sizeof(shown) - strlen(shown) - 1);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(Fonts_getMedium(), shown, COLOR_WHITE);
    if (ts) {
        int max_w = hw - pad * 4;
        /* Keep the end of long text visible */
        SDL_Rect src = { ts->w > max_w ? ts->w - max_w : 0, 0,
                         ts->w > max_w ? max_w : ts->w, ts->h };
        SDL_BlitSurface(ts, &src, screen,
                        &(SDL_Rect){pad * 2, field_y + (field_h - ts->h) / 2});
        SDL_FreeSurface(ts);
    }

    /* --- Keys --- */
    int kb_top    = field_y + field_h + SCALE1(PADDING);
    int kb_bottom = hh - SCALE1(PILL_SIZE + PADDING * 2);
    int gap       = SCALE1(2);
    int key_w     = (hw - pad * 2) / KB_COLS;
    int key_h     = (kb_bottom - kb_top) / KB_ROWS;
    if (key_h > SCALE1(PILL_SIZE + 4)) key_h = SCALE1(PILL_SIZE + 4);

    uint32_t key_bg = SDL_MapRGB(screen->format, 0x30, 0x30, 0x30);
    uint32_t sel_bg = SDL_MapRGB(screen->format, 0xF0, 0xF0, 0xF0);
    SDL_Color fg     = COLOR_WHITE;
    SDL_Color sel_fg = (SDL_Color){0x12, 0x12, 0x12, 0xFF};

    for (int r = 0; r < KB_CHAR_ROWS; r++) {
        const char *keys = k_layouts[layout][r];
        for (int c = 0; c < KB_COLS; c++) {
            bool sel = (r == row && c == col);
            int x = pad + c * key_w;
            int y = kb_top + r * key_h;
            render_rounded_rect_bg(screen, x + gap, y + gap, key_w - gap * 2,
                                   key_h - gap * 2, sel ? sel_bg : key_bg);
            char label[2] = { keys[c], '\0' };
            if (label[0] == ' ') { label[0] = '_'; }
            draw_text_centered(screen, Fonts_getMedium(), label, sel ? sel_fg : fg,
                               x + key_w / 2, y + key_h / 2);
        }
    }

    const char *act_labels[KB_ACTIONS] = {
        k_layout_labels[layout], ":4533", "SPACE", "DEL", "OK"
    };
    int act_w = (hw - pad * 2) / KB_ACTIONS;
    int act_y = kb_top + KB_CHAR_ROWS * key_h;
    for (int a = 0; a < KB_ACTIONS; a++) {
        bool sel = (row == KB_CHAR_ROWS && col == a);
        int x = pad + a * act_w;
        render_rounded_rect_bg(screen, x + gap, act_y + gap, act_w - gap * 2,
                               key_h - gap * 2, sel ? sel_bg : key_bg);
        draw_text_centered(screen, Fonts_getSmall(), act_labels[a], sel ? sel_fg : fg,
                           x + act_w / 2, act_y + key_h / 2);
    }

    GFX_blitButtonGroup((char*[]){"B", "DEL", "Y", "SHIFT", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"SELECT", "CANCEL", "START", "OK", NULL}, 1, screen, 1);
}

bool UIKeyboard_run(SDL_Surface *screen, const char *title,
                    char *buf, int buf_size, bool masked)
{
    char *original = strdup(buf ? buf : "");
    if (!original) return false;

    KbLayout layout = LAYOUT_LOWER;
    int row = 1, col = 0;
    int dirty = 1;
    int show_setting = 0;
    uint32_t last_type_ms = 0;
    bool result = false;

    clear_toast();

    while (1) {
        GFX_startFrame();
        PAD_poll();
        ModuleCommon_PWR_update(&dirty, &show_setting);

        if (PAD_justRepeated(BTN_UP)) {
            if (row == KB_CHAR_ROWS) col = col * KB_COLS / KB_ACTIONS;
            row = (row + KB_ROWS - 1) % KB_ROWS;
            if (row == KB_CHAR_ROWS) col = col * KB_ACTIONS / KB_COLS;
            dirty = 1;
        } else if (PAD_justRepeated(BTN_DOWN)) {
            if (row == KB_CHAR_ROWS) col = col * KB_COLS / KB_ACTIONS;
            row = (row + 1) % KB_ROWS;
            if (row == KB_CHAR_ROWS) col = col * KB_ACTIONS / KB_COLS;
            dirty = 1;
        } else if (PAD_justRepeated(BTN_LEFT)) {
            int n = (row == KB_CHAR_ROWS) ? KB_ACTIONS : KB_COLS;
            col = (col + n - 1) % n;
            dirty = 1;
        } else if (PAD_justRepeated(BTN_RIGHT)) {
            int n = (row == KB_CHAR_ROWS) ? KB_ACTIONS : KB_COLS;
            col = (col + 1) % n;
            dirty = 1;
        } else if (PAD_justPressed(BTN_A)) {
            if (row < KB_CHAR_ROWS) {
                char ch[2] = { k_layouts[layout][row][col], '\0' };
                append_str(buf, buf_size, ch);
                last_type_ms = SDL_GetTicks();
                /* One-shot shift: go back to lowercase after a capital */
                if (layout == LAYOUT_UPPER) layout = LAYOUT_LOWER;
            } else {
                switch (col) {
                case ACT_LAYOUT: layout = (KbLayout)((layout + 1) % LAYOUT_COUNT); break;
                case ACT_PORT:   append_str(buf, buf_size, ":4533"); break;
                case ACT_SPACE:  append_str(buf, buf_size, " "); last_type_ms = SDL_GetTicks(); break;
                case ACT_DEL:    backspace(buf); break;
                case ACT_DONE:   result = true; goto done;
                }
            }
            dirty = 1;
        } else if (PAD_justRepeated(BTN_B)) {
            backspace(buf);
            dirty = 1;
        } else if (PAD_justPressed(BTN_X)) {
            append_str(buf, buf_size, " ");
            last_type_ms = SDL_GetTicks();
            dirty = 1;
        } else if (PAD_justPressed(BTN_Y)) {
            layout = (KbLayout)((layout + 1) % LAYOUT_COUNT);
            dirty = 1;
        } else if (PAD_justPressed(BTN_START)) {
            result = true;
            goto done;
        } else if (PAD_justPressed(BTN_SELECT)) {
            result = false;
            goto done;
        }

        /* Re-render while the last masked character is revealed so it hides on time */
        if (masked && last_type_ms && SDL_GetTicks() - last_type_ms < 1000) dirty = 1;

        if (dirty) {
            render(screen, title, buf, masked, last_type_ms, layout, row, col, show_setting);
            GFX_flip(screen);
            dirty = 0;
        } else {
            GFX_sync();
        }
    }

done:
    if (!result) {
        strncpy(buf, original, buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
    free(original);
    return result;
}
