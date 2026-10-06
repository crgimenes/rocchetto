/* wasm32 host shim: exports a byte-in/byte-out ABI over static staging
   buffers; the JS glue moves bytes and serves file requests via fetch.
   Contract mirrors the POSIX host: never call back into the core from the
   request import (fetch is async, which enforces it naturally). */
#include <string.h>

#include "../../src/roc.h"

#define WASM_EXPORT(nm) __attribute__((export_name(nm)))

__attribute__((import_module("env"), import_name("host_request"))) void
host_request(uint32_t req_id, const char *path, size_t len);

__attribute__((import_module("env"), import_name("host_live_open"))) void host_live_open(void);

__attribute__((import_module("env"), import_name("host_live_close"))) void host_live_close(void);

__attribute__((import_module("env"), import_name("host_term_resize"))) void
host_term_resize(uint32_t cols, uint32_t rows);

__attribute__((import_module("env"), import_name("host_pick_file"))) void
host_pick_file(const char *dest, size_t len);

__attribute__((import_module("env"), import_name("host_store_put"))) uint32_t
host_store_put(const uint8_t *data, size_t len);
__attribute__((import_module("env"), import_name("host_store_claim"))) void host_store_claim(void);

/* Date.now() and the local offset east of UTC, in minutes. */
__attribute__((import_module("env"), import_name("host_now"))) double host_now(void);
__attribute__((import_module("env"), import_name("host_tz"))) int32_t host_tz(void);

__attribute__((import_module("env"), import_name("host_download"))) void
host_download(const char *name, size_t name_len, const uint8_t *data, size_t len);

/* The site's files for scripts (file-open, read-file): host_site_wait
   answers once the site does: -3 when the file is coming (its size known
   only at its end), -1 when the site has none, -2 when the person stopped
   the wait. host_site_read copies bytes from off: how many, 0 at the end,
   -1 on an error, -2 stopped; bytes not arrived yet are waited for. Both
   may not return at once: under Asyncify the core's stack unwinds, the
   page fetches, and the call comes back with the answer. host_site_drop
   lets the file go. */
__attribute__((import_module("env"), import_name("host_site_wait"))) double
host_site_wait(const char *path, size_t len);
__attribute__((import_module("env"), import_name("host_site_read"))) int32_t
host_site_read(const char *path, size_t len, double off, uint8_t *buf, size_t n);
__attribute__((import_module("env"), import_name("host_site_drop"))) void
host_site_drop(const char *path, size_t len);

/* A | larger than the core holds, spooled in the page's memory: a number
   names each (0: none could be made). */
__attribute__((import_module("env"), import_name("host_spool_new"))) uint32_t host_spool_new(void);
__attribute__((import_module("env"), import_name("host_spool_write"))) uint32_t
host_spool_write(uint32_t id, const uint8_t *data, size_t n);
__attribute__((import_module("env"), import_name("host_spool_read"))) uint32_t
host_spool_read(uint32_t id, double off, uint8_t *buf, size_t n);
__attribute__((import_module("env"), import_name("host_spool_free"))) void
host_spool_free(uint32_t id);

/* A script running long hands the page its turn now and then: host_yield
   comes back once the page has run what was waiting (keys, drawing), 0, or
   -2 when one of those keys was a Ctrl-C. host_ticks: milliseconds, only
   to space the turns. */
__attribute__((import_module("env"), import_name("host_yield"))) double host_yield(void);
__attribute__((import_module("env"), import_name("host_ticks"))) double host_ticks(void);

static roc M;
static uint8_t iobuf[ROC_FEED_MAX];
static uint8_t outbuf[ROC_FEED_MAX * 4];

static void req_cb(void *ctx, uint32_t req_id, const char *path) {
    (void)ctx;
    host_request(req_id, path, strlen(path));
}

static void live_open_cb(void *ctx) {
    (void)ctx;
    host_live_open();
}

static void live_close_cb(void *ctx) {
    (void)ctx;
    host_live_close();
}

static void term_resize_cb(void *ctx, uint16_t cols, uint16_t rows) {
    (void)ctx;
    host_term_resize(cols, rows);
}

static void pick_file_cb(void *ctx, const char *dest) {
    (void)ctx;
    host_pick_file(dest, strlen(dest));
}

static uint32_t store_put_cb(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    return host_store_put(data, len);
}

enum { YIELD_EVERY_MS = 100 };

static bool interrupted_cb(void *ctx) {
    (void)ctx;
    static double last;
    double now = host_ticks();
    if (now - last < (double)YIELD_EVERY_MS) {
        return false;
    }
    last = now;
    bool stop = host_yield() == -2;
    last = host_ticks(); /* the page's turn is not the script's time */
    return stop;
}

static void store_claim_cb(void *ctx) {
    (void)ctx;
    host_store_claim();
}

static void download_cb(void *ctx, const char *name, const uint8_t *data, size_t len) {
    (void)ctx;
    host_download(name, strlen(name), data, len);
}

/* A file of the site's index that the page has to fetch: not the home's,
   not one the core holds (the shell's tree, the store). */
static bool site_file(const char *path) {
    const uint8_t *data = NULL;
    size_t len = 0;
    if (strncmp(path, "/home/", 6) == 0 || roc_find_file(&M, path, &data, &len)) {
        return false;
    }
    const vfs_node *node = vfs_lookup(&M.fs, path);
    return node != NULL && !node->dir;
}

enum { SITE_OPEN_MAX = 16 };
static char site_open[SITE_OPEN_MAX][VFS_PATH_MAX];

static int fh_open_cb(void *ctx, const char *path, int mode, void **h, uint64_t *size) {
    (void)ctx;
    if (mode != ROC_FH_READ || !site_file(path)) {
        return ROC_HOST_NOT_MINE;
    }
    size_t slot = 0;
    while (slot < SITE_OPEN_MAX && site_open[slot][0] != '\0') {
        slot++;
    }
    if (slot == SITE_OPEN_MAX) {
        return ROC_HOST_NO;
    }
    double r = host_site_wait(path, strlen(path));
    if (r == -2) {
        return ROC_HOST_CANCELLED;
    }
    if (r < 0 && r != -3) {
        return ROC_HOST_NO;
    }
    memcpy(site_open[slot], path, strlen(path) + 1);
    *h = &site_open[slot];
    *size = r == -3 ? ROC_FH_SIZE_UNKNOWN : (uint64_t)r;
    return ROC_HOST_YES;
}

static long long fh_read_cb(void *ctx, void *h, uint64_t off, uint8_t *buf, size_t n) {
    (void)ctx;
    const char *path = h;
    int32_t k = host_site_read(path, strlen(path), (double)off, buf, n);
    return k < 0 ? -1 : (long long)k;
}

static bool fh_close_cb(void *ctx, void *h, bool commit) {
    (void)ctx;
    (void)commit;
    char *path = h;
    host_site_drop(path, strlen(path));
    path[0] = '\0';
    return true;
}

static void *spool_new_cb(void *ctx) {
    (void)ctx;
    return (void *)(uintptr_t)host_spool_new();
}

static bool spool_write_cb(void *ctx, void *s, const uint8_t *data, size_t n) {
    (void)ctx;
    return host_spool_write((uint32_t)(uintptr_t)s, data, n) != 0;
}

static long long spool_read_cb(void *ctx, void *s, uint64_t off, uint8_t *buf, size_t n) {
    (void)ctx;
    return (long long)host_spool_read((uint32_t)(uintptr_t)s, (double)off, buf, n);
}

static void spool_free_cb(void *ctx, void *s) {
    (void)ctx;
    host_spool_free((uint32_t)(uintptr_t)s);
}

/* Where Asyncify keeps the core's stack while a wait is out: two words
   (where the stack goes, where it must end), then the stack. The page
   writes the two words before each unwind. Deep enough for a script in a
   map in a nested run. */
static uint32_t async_data[(256U * 1024U) / 4U];

WASM_EXPORT("roc_w_async_data") uint32_t *roc_w_async_data(void) {
    return async_data;
}

WASM_EXPORT("roc_w_async_size") uint32_t roc_w_async_size(void) {
    return (uint32_t)sizeof(async_data);
}

WASM_EXPORT("roc_w_iobuf") uint8_t *roc_w_iobuf(void) {
    return iobuf;
}

WASM_EXPORT("roc_w_iobuf_cap") uint32_t roc_w_iobuf_cap(void) {
    return (uint32_t)sizeof(iobuf);
}

WASM_EXPORT("roc_w_outbuf") uint8_t *roc_w_outbuf(void) {
    return outbuf;
}

static bool clock_cb(void *ctx, int64_t *secs, int32_t *tz_minutes) {
    (void)ctx;
    *secs = (int64_t)(host_now() / 1000.0);
    *tz_minutes = host_tz();
    return true;
}

/* The scratch starts where the memory ended the first time a tool asked
   and grows the memory as tools need more: nothing else grows it, so the
   base never moves. It never shrinks (wasm cannot); the next tool reuses
   it. */
static void *scratch_cb(void *ctx, size_t need) {
    static uint8_t *base;
    static size_t have;
    (void)ctx;
    if (base == NULL) {
        base = (uint8_t *)(__builtin_wasm_memory_size(0) * 65536U);
    }
    if (need > have) {
        size_t pages = ((need - have) + 65535U) / 65536U;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) {
            return NULL;
        }
        have += pages * 65536U;
    }
    return base;
}

/* Who this site is, from the page: iobuf holds "NAME\nBASE" (idlen bytes;
   0: nameless, links left as they are), the host name for the prompt and
   the origin for the pager's links. */
static char site_name[128];
static char site_base[256];

static void site_from(uint32_t idlen) {
    site_name[0] = '\0';
    site_base[0] = '\0';
    uint32_t n = idlen < sizeof(iobuf) ? idlen : (uint32_t)sizeof(iobuf);
    uint32_t i = 0;
    uint32_t k = 0;
    while (i < n && iobuf[i] != '\n' && k + 1 < sizeof(site_name)) {
        site_name[k++] = (char)iobuf[i++];
    }
    site_name[k] = '\0';
    while (i < n && iobuf[i] != '\n') {
        i++;
    }
    i++;
    k = 0;
    while (i < n && k + 1 < sizeof(site_base)) {
        site_base[k++] = (char)iobuf[i++];
    }
    site_base[k] = '\0';
}

WASM_EXPORT("roc_w_init")
void roc_w_init(uint32_t cols, uint32_t rows, uint32_t flags, uint32_t idlen) {
    site_from(idlen);
    roc_host h = {
        .filo_extend = roc_host_extend,
        .ctx = 0,
        .host_name = site_name,
        .site_base = site_base,
        .request = req_cb,
        .live_open = live_open_cb,
        .live_close = live_close_cb,
        .term_resize = term_resize_cb,
        .pick_file = pick_file_cb,
        .store_put = store_put_cb,
        .store_claim = store_claim_cb,
        .interrupted = interrupted_cb,
        .download = download_cb,
        .clock = clock_cb,
        .scratch = scratch_cb,
        .fh_open = fh_open_cb,
        .fh_read = fh_read_cb,
        .fh_close = fh_close_cb,
        .spool_new = spool_new_cb,
        .spool_write = spool_write_cb,
        .spool_read = spool_read_cb,
        .spool_free = spool_free_cb,
    };
    roc_init(&M, &h, (uint16_t)cols, (uint16_t)rows, flags);
}

WASM_EXPORT("roc_w_input") void roc_w_input(uint32_t len) {
    if (len > sizeof(iobuf)) {
        len = (uint32_t)sizeof(iobuf);
    }
    roc_input(&M, iobuf, len);
}

WASM_EXPORT("roc_w_resize") void roc_w_resize(uint32_t cols, uint32_t rows) {
    roc_resize(&M, (uint16_t)cols, (uint16_t)rows);
}

WASM_EXPORT("roc_w_tick") void roc_w_tick(uint32_t ms) {
    roc_tick(&M, ms);
}

/* The columns the shell gives a character, for the terminal to give it the
   same: xterm's own table (Unicode 6) draws emoji one column wide where the
   board, as every terminal of today, gives them two, and a line with one
   in it then lands a column off. */
WASM_EXPORT("roc_w_width") uint32_t roc_w_width(uint32_t cp) {
    int w = utf8_width(cp);
    return w < 0 ? 0U : (uint32_t)w;
}

WASM_EXPORT("roc_w_feed") void roc_w_feed(uint32_t req_id, uint32_t len) {
    if (len > sizeof(iobuf)) {
        len = (uint32_t)sizeof(iobuf);
    }
    roc_feed(&M, req_id, iobuf, len);
}

WASM_EXPORT("roc_w_feed_eof") void roc_w_feed_eof(uint32_t req_id) {
    roc_feed_eof(&M, req_id);
}

WASM_EXPORT("roc_w_feed_fail") void roc_w_feed_fail(uint32_t req_id) {
    roc_feed_fail(&M, req_id);
}

WASM_EXPORT("roc_w_out_read") uint32_t roc_w_out_read(void) {
    return (uint32_t)term_out_read(&M.t, outbuf, sizeof(outbuf));
}

WASM_EXPORT("roc_w_exited") uint32_t roc_w_exited(void) {
    return roc_exited(&M) ? 1U : 0U;
}

#if ROC_APP_LIVE
WASM_EXPORT("roc_w_live_data") void roc_w_live_data(uint32_t len) {
    if (len > sizeof(iobuf)) {
        len = (uint32_t)sizeof(iobuf);
    }
    roc_live_data(&M, iobuf, len);
}

WASM_EXPORT("roc_w_live_event") void roc_w_live_event(uint32_t event) {
    roc_live_event(&M, event);
}
#endif

/* A file from the page: name then dest in iobuf, bytes through iobuf in
   chunks, then end. Returns 0 when refused (nothing more is expected). */
static char up_name[256];
static char up_dest[VFS_PATH_MAX];

WASM_EXPORT("roc_w_upload_begin")
uint32_t roc_w_upload_begin(uint32_t name_len, uint32_t dest_len, uint32_t size) {
    if (name_len >= sizeof(up_name) || dest_len >= sizeof(up_dest) ||
        name_len + dest_len > sizeof(iobuf)) {
        return 0;
    }
    memcpy(up_name, iobuf, name_len);
    up_name[name_len] = '\0';
    memcpy(up_dest, iobuf + name_len, dest_len);
    up_dest[dest_len] = '\0';
    return roc_upload_begin(&M, up_name, up_dest, size) ? 1U : 0U;
}

WASM_EXPORT("roc_w_upload_data") void roc_w_upload_data(uint32_t len) {
    if (len > sizeof(iobuf)) {
        len = (uint32_t)sizeof(iobuf);
    }
    roc_upload_data(&M, iobuf, len);
}

WASM_EXPORT("roc_w_upload_end") void roc_w_upload_end(void) {
    roc_upload_end(&M);
}
