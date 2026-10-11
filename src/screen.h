#ifndef ROC_SCREEN_H
#define ROC_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roc_config.h"

#include "field.h"
#include "filo.h"
#include "tree.h"
#include "vfs.h"

/* A screen is a Filo program that paints. The shell clears the canvas, runs
   the program, and only then turns cells into bytes: a script never emits an
   escape, and a run that fails is discarded whole, so a broken screen leaves
   the previous frame on the terminal instead of a half-drawn one.

   The shell owns every global a script may touch. W and H carry the terminal
   size before each run, and A_BOLD, A_DIM, A_REV and C_DEFAULT name what the
   pen takes. A script declares the rest in its own init and the shell then
   seals the set, so a name typed wrong fails to compile instead of reading
   as undefined at three in the morning. */

enum {
    SCR_MEM_PERSISTENT = ROC_CFG_SCR_MEM_PERSISTENT, /* compiled programs, globals, symbols */
    SCR_MEM_RUN = ROC_CFG_SCR_MEM_RUN,               /* values and frames, reset every run */
    SCR_STEPS_DRAW = 200000,                         /* painting a wide screen visits a lot */
    SCR_STEPS_EVENT = 20000,                         /* an event only moves state around */
    /* except the tick of a screen that animates, which is where it paints:
       the frame of the donut is the same kind of work as a draw, and the
       draw that follows it already gets the larger count */
    SCR_STEPS_TICK = SCR_STEPS_DRAW,
    SCR_STEPS_INIT = 200000, /* once per load: an init may build its data (news sorts the areas) */
    SCR_ERROR_MAX = 192,
    SCR_NAME_MAX = 32,
    SCR_TRAIL = 8,       /* screens remembered on the way in */
    SCR_ENTRY_MAX = 16,  /* a hook's entry: "draw", "input" */
    SCR_INPUT_MAX = 128, /* bytes a field may hold */
    SCR_FX_MAX = 4,      /* effects a move between screens may chain */
    SCR_FX_NAME = 16,
    SCR_UNIT_FILES = 16, /* the .filo files one screen's unit may hold */
};

/* The screens compiled ahead of time: the tree carries one bundle next to
   the sources, a unit per screen named by the screen, and the shell runs a
   screen's unit instead of compiling its files. */
#define SCR_BUNDLE "screens.fbb"

/* The hooks a program may have: the entries named for them (draw.filo is
   "draw"). The shape of a program is fixed, not configured. */
typedef enum {
    SCR_HOOK_DRAW,
    SCR_HOOK_KEY,
    SCR_HOOK_INPUT,
    /* Time passing. A screen that registers it is animating, and gets MS
       with the milliseconds since the last call — the same shape a door
       written in C gets. Without it a screen only moves when a key does,
       which is what every screen wanted until one of them was a game. */
    SCR_HOOK_TICK,
    SCR_HOOK_COUNT,
} scr_hook_id;

/* A hook is the program's entry of that name — draw.filo is "draw" — run
   from its unit, or compiled from the tree when the screen runs from its
   source. */
typedef struct {
    char entry[SCR_ENTRY_MAX];
    filo_prog prog;
    bool loaded;
    bool bc; /* an entry of the program's unit, not a program compiled here */
} scr_hook;

typedef struct roc roc;

enum { SCR_CURSOR_UNKNOWN = -1, SCR_CURSOR_HIDDEN = 0, SCR_CURSOR_SHOWN = 1 };

typedef struct {
    filo_ctx ctx;
    uint8_t persistent[SCR_MEM_PERSISTENT];
    uint8_t run[SCR_MEM_RUN];
    scr_hook hooks[SCR_HOOK_COUNT];
    const filo_unit *unit;   /* the screen's unit, when the tree carries one */
    uint32_t key_cp;         /* the key the event hook is running for */
    uint64_t seed;           /* the session's random stream, for (random n) */
    char name[SCR_NAME_MAX]; /* the screen now loaded */
    /* the screen that led here, which is where ESC goes. Loading it again
       is the step back itself, so the trail is cleared then: a games list
       reached from the main screen and a games list reached by backing
       out of a game both leave to the front screen. */
    char back[SCR_NAME_MAX];
    /* the screens that led here, nearest last: back is the nearest. A
       round trip (a door over a screen, and back) must not lose the way
       on from the screen beneath. */
    char trail[SCR_TRAIL][SCR_NAME_MAX];
    uint8_t ntrail;
    char pending[VFS_PATH_MAX];       /* what exec asked for, "name:arg" */
    char fx[SCR_FX_MAX][SCR_FX_NAME]; /* the effects it asked to play on the way */
    size_t nfx;

    /* The field the draw hook placed, if it placed one. The shell owns the
       text: a script reads it with input-text and never has to keep it. */
    field in;
    bool in_seeded;  /* input-set ran during the input hook: keep its text */
    bool has_cursor; /* cursor-at: the caret without a field, an editor's */
    /* what the terminal was last told about the cursor. An app over the
       screen paints as it likes, so after one closes this says nothing
       and the next paint states it again. */
    int8_t cursor_shown;
    int32_t caret_row;
    int32_t caret_col;
    /* An app: a program compiled elsewhere, carried whole as NAME.fbb; its
       files are named by their entries alone, not by a screen's directory. */
    bool app;
    /* keep-canvas: what a hook drew stays until it is drawn over, as video
       memory did, so a screen can keep its state in the cells and read it
       back. fresh says the next paint must blank it anyway — the screen was
       just loaded, the size changed, or an app above painted over it. */
    bool keep_canvas;
    bool fresh;

    bool ready;
    uint32_t tick_ms; /* toward the next once-a-second repaint of the clock */
    bool quiet_load;  /* the boot show builds a screen without sending it */
    char error[SCR_ERROR_MAX];
} screen_state;

/* What the program running asks the shell to do once its run is over, as
   exec does: "edt:PATH", "man:PATH", "fight". False when it is too long. */
bool screen_pend(roc *m, const char *target);

/* Starts a context with the shell's builtins and globals. The arenas never
   free, so reloading a screen means starting over here, not patching. */
void screen_reset(roc *m);

/* ctx as a screen's or an app's is, in the arenas given: the builtins and
   the globals a program with draw sees. screen_reset makes the shell's own
   this way; filo check makes one to hold an app to. */
void screen_context_init(roc *m, filo_ctx *ctx, uint8_t *persistent, size_t plen, uint8_t *run,
                         size_t rlen);

/* Loads a screen from the tree: the shell's common file, then the screen's
   own, then its init, which is where it declares its state and names its
   hooks. The globals are sealed when init returns, so the hooks compile
   against a closed set and a name typed wrong fails here rather than at the
   first keystroke. Then it paints. */
bool screen_load(roc *m, const char *name);
bool screen_load_with(roc *m, const char *name, const char *arg);

/* Compiles a screen into the unit the bundle carries for it: the shell's
   common file, the screen's own, its init, and every other .filo of its
   directory as the hooks init may name — each an entry named by its path in
   the tree. It loads the screen as screen_load does — init run, then the
   globals sealed before the hooks compile — so a name typed wrong fails the
   build. */
bool screen_build(roc *m, const char *name, uint8_t *dst, size_t cap, size_t *len);

/* Loads a screen and builds it in the canvas without sending a byte: the
   boot show needs the finished screen as the target of its effects. */
bool screen_compose(roc *m, const char *name);

/* Puts the screen app on top of an already loaded screen. */
void screen_attach(roc *m);

/* Pushes the screen app and loads name into it. */
void screen_enter(roc *m, const char *name);

/* True while a screen is the app on top: the editor loads itself in its
   place instead of stacking a second one over the same state. */
bool screen_is_top(const roc *m);

/* Places the field a draw hook asked for and puts the caret in it. */
void scr_field_at(roc *m, int32_t row, int32_t col, int32_t width, uint32_t most, bool secret);

/* Runs the key hook, paints, and follows exec if the hook asked. */
bool screen_key(roc *m, uint32_t cp);

/* Compiles the program that paints, bypassing the tree. Errors are reported
   now, at load, not on the first key the user presses. */
bool screen_set_draw(roc *m, const uint8_t *src, size_t len);

/* Runs it and, when it succeeds, sends the difference to the terminal. */
bool screen_paint(roc *m);

const char *screen_error(const roc *m);

#endif
