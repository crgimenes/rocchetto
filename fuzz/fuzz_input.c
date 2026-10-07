#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/roc.h"

/* Fuzzes the two trust boundaries: keyboard bytes and fed file bytes. The
   first data byte routes: even -> keyboard, odd -> index+cat feeds. */

static roc M;

static uint32_t last_req;
static void req(void *ctx, uint32_t req_id, const char *path) {
    (void)ctx;
    (void)path;
    last_req = req_id;
}

static void stream_nop(void *ctx) {
    (void)ctx;
}

static void resize_nop(void *ctx, uint16_t cols, uint16_t rows) {
    (void)ctx;
    (void)cols;
    (void)rows;
}

static void *scratch(void *ctx, size_t need) {
    static uint8_t region[64U << 20U];
    (void)ctx;
    if (need > sizeof(region)) {
        return NULL;
    }
    return region;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) {
        return 0;
    }
    roc_host host = {
        .filo_extend = roc_host_extend,
        .ctx = NULL,
        .request = req,
        .stream_open = stream_nop,
        .stream_close = stream_nop,
        .term_resize = resize_nop,
        .scratch = scratch,
    };
    roc_init(&M, &host, 80, 24, ROC_F_NO_SPLASH);
    uint8_t drainbuf[TERM_OUT_CAP];

    if (data[0] % 4 == 3) {
        /* the layer's stream: arbitrary bytes from the network (a BBS's
           live frame parser, in its build; here only the handing over) */
        roc_feed_eof(&M, last_req);
        term_out_read(&M.t, drainbuf, sizeof(drainbuf));
        roc_input(&M, (const uint8_t *)"live\r", 5);
        roc_stream_event(&M, 1); /* connected */
        term_out_read(&M.t, drainbuf, sizeof(drainbuf));
        size_t i = 1;
        while (i < size) {
            size_t chunk = size - i < 11 ? size - i : 11;
            roc_stream_data(&M, data + i, chunk);
            term_out_read(&M.t, drainbuf, sizeof(drainbuf));
            i += chunk;
        }
        roc_input(&M, (const uint8_t *)"q", 1);
        term_out_read(&M.t, drainbuf, sizeof(drainbuf));
        return 0;
    }

    if (data[0] % 2 == 0) {
        /* boot with a tiny index, then everything is keyboard input */
        const char *idx = "/pub/\t0\t-\t\n/pub/a.md\t3\t-\tA\n";
        roc_feed(&M, last_req, (const uint8_t *)idx, strlen(idx));
        roc_feed_eof(&M, last_req);
        term_out_read(&M.t, drainbuf, sizeof(drainbuf));
        size_t i = 1;
        while (i < size) {
            size_t chunk = size - i < 7 ? size - i : 7;
            roc_input(&M, data + i, chunk);
            roc_tick(&M, ROC_ESC_TIMEOUT_MS);
            term_out_read(&M.t, drainbuf, sizeof(drainbuf));
            i += chunk;
        }
        return 0;
    }

    /* fuzz the index parser and the cat/less readers with arbitrary bytes */
    size_t half = size / 2;
    roc_feed(&M, last_req, data + 1, half > 0 ? half - 1 : 0);
    roc_feed_eof(&M, last_req);
    term_out_read(&M.t, drainbuf, sizeof(drainbuf));
    roc_input(&M, (const uint8_t *)"s\r", 2); /* menu -> shell */
    term_out_read(&M.t, drainbuf, sizeof(drainbuf));
    const char *cmd = data[0] % 4 == 1 ? "cat /pub/a.md\r" : "less /pub/a.md\r";
    roc_input(&M, (const uint8_t *)cmd, strlen(cmd));
    term_out_read(&M.t, drainbuf, sizeof(drainbuf));
    roc_feed(&M, last_req, data + half, size - half);
    roc_feed_eof(&M, last_req);
    term_out_read(&M.t, drainbuf, sizeof(drainbuf));
    /* poke the pager if it opened, re-wrapping at an arbitrary width: that
       is where rendered escapes meet the column counter */
    roc_resize(&M, (uint16_t)(2 + (data[1] % 40)), 24);
    term_out_read(&M.t, drainbuf, sizeof(drainbuf));
    roc_input(&M, (const uint8_t *)"jG gq", 5);
    term_out_read(&M.t, drainbuf, sizeof(drainbuf));
    return 0;
}
