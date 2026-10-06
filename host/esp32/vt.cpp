/* The terminal the board is, rather than the one it talks to: roc emits
   ANSI and something has to turn that back into a picture. That something
   is a grid of cells — which the runtime already has, with a diff that
   says what changed — so this file is only the parser that writes into the
   grid and the loop that paints the cells that moved.
 *
 * Keeping the grid means the LCD is never redrawn whole. It also means no
 * full-screen sprite, which matters: with the radio up the largest block
 * the heap will give is around 31 KB and a 240x135 16bpp sprite is 64 KB.
 */

#include "vt.h"

#include <M5Cardputer.h>
#include <string.h>

extern "C" {
#include "canvas.h"
#include "term.h"
}

namespace {

canvas shown;
canvas work;

int32_t cur_row = 0;
int32_t cur_col = 0;
cv_cell pen;
bool cursor_on = true;

/* the escape being read: 0 none, 1 after ESC, 2 inside a CSI, 3 inside an
   OSC, 4 after ESC inside one */
uint8_t esc = 0;
char csi[24];
size_t csi_len = 0;

/* The built-in 6x8 font doubled. Size 1 gives forty columns and is
   unreadable on a panel this small — this is the size the earlier
   Cardputer work settled on, and it is settled for the same reason. */
const int CELL_W = 12;
const int CELL_H = 16;

cv_cell blank() {
    cv_cell c = {};
    c.cp = ' ';
    c.fg = CV_COLOR_DEFAULT;
    c.bg = CV_COLOR_DEFAULT;
    c.attr = 0;
    return c;
}

/* xterm's 256 in RGB565: sixteen named, then the 6x6x6 cube, then the
   greys. The board only ever asks for the first sixteen and the cube. */
uint16_t rgb(int16_t idx, bool foreground) {
    if (idx == CV_COLOR_DEFAULT) {
        return foreground ? 0xC618 : 0x0000; /* light grey on black */
    }
    static const uint8_t base[16][3] = {
        {0, 0, 0},       {170, 0, 0},   {0, 170, 0},   {170, 85, 0},
        {0, 0, 170},     {170, 0, 170}, {0, 170, 170}, {170, 170, 170},
        {85, 85, 85},    {255, 85, 85}, {85, 255, 85}, {255, 255, 85},
        {85, 85, 255},   {255, 85, 255}, {85, 255, 255}, {255, 255, 255},
    };
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    if (idx < 16) {
        r = base[idx][0];
        g = base[idx][1];
        b = base[idx][2];
    } else if (idx < 232) {
        int n = idx - 16;
        static const uint8_t step[6] = {0, 95, 135, 175, 215, 255};
        r = step[(n / 36) % 6];
        g = step[(n / 6) % 6];
        b = step[n % 6];
    } else {
        uint8_t v = (uint8_t)(8 + ((idx - 232) * 10));
        r = v;
        g = v;
        b = v;
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void scroll_up() {
    for (int32_t y = 0; y + 1 < work.rows; y++) {
        memcpy(work.cells[y], work.cells[y + 1], sizeof(cv_cell) * (size_t)work.cols);
    }
    cv_cell b = blank();
    for (int32_t x = 0; x < work.cols; x++) {
        work.cells[work.rows - 1][x] = b;
    }
}

void newline() {
    cur_row++;
    if (cur_row >= work.rows) {
        cur_row = work.rows - 1;
        scroll_up();
    }
}

void put_rune(uint32_t cp) {
    if (cur_col >= work.cols) {
        cur_col = 0;
        newline();
    }
    cv_cell c = pen;
    c.cp = cp;
    work.cells[cur_row][cur_col] = c;
    cur_col++;
}

/* What an ASCII font can show of a rune it does not have. */
uint32_t ascii_for(uint32_t cp) {
    switch (cp) {
    case 0x00B7: /* · */
        return '.';
    case 0x2014: /* — */
    case 0x2013: /* – */
        return '-';
    case 0x2026: /* … */
        return '~';
    case 0x00E1:
    case 0x00E0:
    case 0x00E3:
    case 0x00E2:
        return 'a';
    case 0x00E9:
    case 0x00EA:
        return 'e';
    case 0x00ED:
        return 'i';
    case 0x00F3:
    case 0x00F5:
    case 0x00F4:
        return 'o';
    case 0x00FA:
        return 'u';
    case 0x00E7:
        return 'c';
    default:
        return cp < 0x20 ? ' ' : '?';
    }
}

/* The numbers of a CSI, defaulting to one so "ESC[C" moves by one. */
int params(int *out, int max) {
    int n = 0;
    int v = 0;
    bool any = false;
    for (size_t i = 0; i < csi_len && n < max; i++) {
        char ch = csi[i];
        if (ch >= '0' && ch <= '9') {
            v = (v * 10) + (ch - '0');
            any = true;
            continue;
        }
        if (ch == ';') {
            out[n] = any ? v : 0;
            n++;
            v = 0;
            any = false;
        }
    }
    if (n < max) {
        out[n] = any ? v : 0;
        n++;
    }
    return n;
}

void sgr() {
    int p[8];
    int n = params(p, 8);
    for (int i = 0; i < n; i++) {
        int v = p[i];
        if (v == 0) {
            pen = blank();
        } else if (v == 1) {
            pen.attr |= CV_A_BOLD;
        } else if (v == 2) {
            pen.attr |= CV_A_DIM;
        } else if (v == 7) {
            pen.attr |= CV_A_REV;
        } else if (v == 27) {
            pen.attr &= (uint64_t)~(uint64_t)CV_A_REV;
        } else if (v >= 30 && v <= 37) {
            pen.fg = (int16_t)(v - 30);
        } else if (v >= 90 && v <= 97) {
            pen.fg = (int16_t)(v - 90 + 8);
        } else if (v >= 40 && v <= 47) {
            pen.bg = (int16_t)(v - 40);
        } else if (v == 39) {
            pen.fg = CV_COLOR_DEFAULT;
        } else if (v == 49) {
            pen.bg = CV_COLOR_DEFAULT;
        } else if ((v == 38 || v == 48) && i + 2 < n && p[i + 1] == 5) {
            if (v == 38) {
                pen.fg = (int16_t)p[i + 2];
            } else {
                pen.bg = (int16_t)p[i + 2];
            }
            i += 2;
        }
    }
}

void erase_to_eol() {
    cv_cell b = blank();
    b.bg = pen.bg;
    for (int32_t x = cur_col; x < work.cols; x++) {
        work.cells[cur_row][x] = b;
    }
}

void erase_below() {
    erase_to_eol();
    cv_cell b = blank();
    b.bg = pen.bg;
    for (int32_t y = cur_row + 1; y < work.rows; y++) {
        for (int32_t x = 0; x < work.cols; x++) {
            work.cells[y][x] = b;
        }
    }
}

void clear_all() {
    cv_cell b = blank();
    for (int32_t y = 0; y < work.rows; y++) {
        for (int32_t x = 0; x < work.cols; x++) {
            work.cells[y][x] = b;
        }
    }
    cur_row = 0;
    cur_col = 0;
}

void final_byte(char cmd) {
    int p[4] = {0, 0, 0, 0};
    /* "?" sequences are modes, and this terminal keeps two of them: the
       cursor, and the alternate screen the board draws on. That one is only
       blanked, going in and coming out — keeping the shell's screen to put
       back would be another grid of RAM, and without either the menu stays
       under the prompt. Bracketed paste and the mouse wheel mean nothing
       here. */
    if (csi_len > 0 && csi[0] == '?') {
        if (cmd == 'h' || cmd == 'l') {
            if (csi_len >= 4 && csi[1] == '2' && csi[2] == '5') {
                cursor_on = cmd == 'h';
            }
            if (csi_len == 5 && memcmp(csi + 1, "1049", 4) == 0) {
                clear_all();
            }
        }
        return;
    }
    int n = params(p, 4);
    switch (cmd) {
    case 'm':
        sgr();
        return;
    case 'H':
        cur_row = n > 0 && p[0] > 0 ? p[0] - 1 : 0;
        cur_col = n > 1 && p[1] > 0 ? p[1] - 1 : 0;
        break;
    case 'A':
        cur_row -= p[0] > 0 ? p[0] : 1;
        break;
    case 'B':
        cur_row += p[0] > 0 ? p[0] : 1;
        break;
    case 'C':
        cur_col += p[0] > 0 ? p[0] : 1;
        break;
    case 'D':
        cur_col -= p[0] > 0 ? p[0] : 1;
        break;
    case 'G':
        cur_col = p[0] > 0 ? p[0] - 1 : 0;
        break;
    case 'K':
        erase_to_eol();
        return;
    case 'J':
        if (p[0] == 2) {
            clear_all();
            return;
        }
        erase_below();
        return;
    default:
        return;
    }
    if (cur_row < 0) {
        cur_row = 0;
    }
    if (cur_row >= work.rows) {
        cur_row = work.rows - 1;
    }
    if (cur_col < 0) {
        cur_col = 0;
    }
    if (cur_col > work.cols) {
        cur_col = work.cols;
    }
}

} // namespace

void vt_begin(uint16_t cols, uint16_t rows) {
    cv_reset(&work, rows, cols);
    cv_reset(&shown, rows, cols);
    /* shown starts impossible so the first paint writes every cell */
    for (int32_t y = 0; y < shown.rows; y++) {
        for (int32_t x = 0; x < shown.cols; x++) {
            shown.cells[y][x].cp = 0xFFFF;
        }
    }
    pen = blank();
    cur_row = 0;
    cur_col = 0;
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.fillScreen(0x0000);
}

void vt_feed(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (esc == 1) {
            esc = b == '[' ? 2 : 0;
            if (b == ']') {
                esc = 3;
            }
            csi_len = 0;
            continue;
        }
        /* An OSC (the pager's hyperlinks are ESC ] 8 ; ; ... ESC \) says
           nothing this panel can show: swallowed up to BEL or ESC \, or its
           body is drawn as text. */
        if (esc == 3) {
            if (b == 0x07) {
                esc = 0;
            } else if (b == 0x1B) {
                esc = 4;
            }
            continue;
        }
        if (esc == 4) {
            esc = 0;
            continue;
        }
        if (esc == 2) {
            if (b >= 0x40 && b <= 0x7E) {
                esc = 0;
                final_byte((char)b);
                continue;
            }
            if (csi_len < sizeof(csi)) {
                csi[csi_len] = (char)b;
                csi_len++;
            }
            continue;
        }
        if (b == 0x1B) {
            esc = 1;
            continue;
        }
        if (b == '\r') {
            cur_col = 0;
            continue;
        }
        if (b == '\n') {
            newline();
            continue;
        }
        if (b == '\b') {
            if (cur_col > 0) {
                cur_col--;
            }
            continue;
        }
        if (b < 0x20) {
            continue; /* bell and friends: nothing to draw */
        }
        /* UTF-8 in, one cell out. The 6x8 font on this panel is ASCII, so
           a rune it cannot draw becomes the nearest thing it can: the
           board writes "·" between words and "—" in its menus, and those
           read fine as "." and "-". Anything else says "?" rather than
           pretending. */
        if (b >= 0x80) {
            uint32_t cp = b;
            int extra = 0;
            if ((b & 0xE0) == 0xC0) {
                cp = b & 0x1FU;
                extra = 1;
            } else if ((b & 0xF0) == 0xE0) {
                cp = b & 0x0FU;
                extra = 2;
            } else if ((b & 0xF8) == 0xF0) {
                cp = b & 0x07U;
                extra = 3;
            }
            while (extra > 0 && i + 1 < n && (p[i + 1] & 0xC0) == 0x80) {
                i++;
                cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
                extra--;
            }
            put_rune(ascii_for(cp));
            continue;
        }
        put_rune(b);
    }
}

/* What the LCD is showing, as text. The screen cannot be read from here,
   so this is how a session at a desk checks that the parser built the
   right picture — and it is the same grid the panel is painted from. */
size_t vt_dump(char *out, size_t cap) {
    size_t n = 0;
    for (int32_t y = 0; y < work.rows && n + (size_t)work.cols + 4 < cap; y++) {
        for (int32_t x = 0; x < work.cols; x++) {
            uint32_t cp = (uint32_t)work.cells[y][x].cp;
            out[n] = (cp >= 0x20 && cp < 0x7F) ? (char)cp : ' ';
            n++;
        }
        while (n > 0 && out[n - 1] == ' ') {
            n--; /* trailing blanks say nothing */
        }
        out[n] = '\r';
        n++;
        out[n] = '\n';
        n++;
    }
    return n;
}

void vt_flush(void) {
    auto &d = M5Cardputer.Display;
    d.startWrite();
    for (int32_t y = 0; y < work.rows; y++) {
        for (int32_t x = 0; x < work.cols; x++) {
            cv_cell *a = &shown.cells[y][x];
            cv_cell *b = &work.cells[y][x];
            if (a->raw == b->raw) {
                continue;
            }
            int16_t f = (int16_t)b->fg;
            int16_t g = (int16_t)b->bg;
            if ((b->attr & CV_A_REV) != 0) {
                int16_t t = f;
                f = g == CV_COLOR_DEFAULT ? 0 : g;
                g = t == CV_COLOR_DEFAULT ? 7 : t;
            }
            uint16_t back = rgb(g, false);
            d.fillRect(x * CELL_W, y * CELL_H, CELL_W, CELL_H, back);
            uint32_t cp = (uint32_t)b->cp;
            if (cp != ' ' && cp != 0) {
                d.setTextColor(rgb(f, true), back);
                d.setCursor(x * CELL_W, y * CELL_H);
                d.write((char)cp);
            }
            *a = *b;
        }
    }
    d.endWrite();
}
