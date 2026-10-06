/* roc on an ESP32-S3, over the USB serial line and nothing else: no
   display, no keyboard, no network. The point of this host is to answer
   what only hardware can — that the core builds for Xtensa, that it fits,
   and that a session over a wire feels like a session.
 *
 * Build and flash (the sketch folder is this one):
 *   arduino-cli compile -b m5stack:esp32:m5stack_cardputer host/esp32
 *   arduino-cli upload  -b m5stack:esp32:m5stack_cardputer -p /dev/cu.usbmodemXXXX host/esp32
 * Then talk to it: screen /dev/cu.usbmodemXXXX 115200
 */

extern "C" {
#include "roc.h"
}

#include <WiFi.h>
#include <M5Cardputer.h>

#include "sd.h"
#include "vt.h"

/* With no card there is still a filesystem — the tree in flash carries
   /bin — and this one line gives the VFS a root to mount it under. */
static const char BARE_INDEX[] = "/\t0\t-\t\n";

static roc M;
static uint32_t last_ms;

/* The core forbids calling back into it from a callback, so a request is
   only written down here and answered from the loop. */
static bool req_pending;
static uint32_t req_id;
static char req_path[VFS_PATH_MAX];

static void on_request(void *ctx, uint32_t id, const char *path) {
    (void)ctx;
    req_pending = true;
    req_id = id;
    snprintf(req_path, sizeof(req_path), "%s", path);
}

/* Bytes on their way to roc: at most ROC_FEED_MAX at a time, and the
   output drained between feeds, which is the contract the core states. */
static void drain(void);

static void feed(const uint8_t *p, size_t n) {
    while (n > 0) {
        size_t take = n > ROC_FEED_MAX ? ROC_FEED_MAX : n;
        roc_feed(&M, req_id, p, take);
        drain();
        p += take;
        n -= take;
    }
}

static void serve(void) {
    req_pending = false;
    if (strcmp(req_path, ROC_INDEX_PATH) == 0) {
        if (sd_mounted()) {
            sd_index(feed);
        } else {
            roc_feed(&M, req_id, (const uint8_t *)BARE_INDEX, sizeof(BARE_INDEX) - 1);
        }
        roc_feed_eof(&M, req_id);
        return;
    }
    if (!sd_read(req_path, feed)) {
        roc_feed_fail(&M, req_id);
        return;
    }
    roc_feed_eof(&M, req_id);
}

/* The card is the drive: a file the user writes is a file on it, not a
   record inside a blob the session packs on the way out. */
static const uint8_t *file_get(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    return sd_slurp(path, len);
}

static bool file_put(void *ctx, const char *path, const uint8_t *data, size_t len) {
    (void)ctx;
    sd_mkpath(path);
    return sd_write(path, data, len);
}

static bool file_del(void *ctx, const char *path) {
    (void)ctx;
    return sd_unlink(path);
}

/* Ctrl-] never reaches the shell: it asks the host what the chip has left.
   The linker only counts static DRAM, and a display driver, a radio or a
   microphone all take theirs from the heap — so this is the number that
   says whether they fit. */
static void report(void) {
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "\r\n[card: %s  free heap %u  largest block %u  free stack %u]\r\n",
                     sd_why(), (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
    Serial.write((const uint8_t *)line, (size_t)n);
}

/* Ctrl-\\ turns the radio on and says what it cost. Nothing is joined and no
   credential is involved — a passive scan is enough to make the stack
   allocate what it allocates, which is the number that matters. */
static void wifi_probe(void) {
    char line[160];
    unsigned before = (unsigned)ESP.getFreeHeap();
    WiFi.mode(WIFI_STA);
    int found = WiFi.scanNetworks();
    unsigned after = (unsigned)ESP.getFreeHeap();
    int n = snprintf(line, sizeof(line),
                     "\r\n[wifi on: heap %u -> %u, cost %u B; %d networks; largest block %u]\r\n",
                     before, after, before - after, found, (unsigned)ESP.getMaxAllocHeap());
    Serial.write((const uint8_t *)line, (size_t)n);
}

/* Everything roc says goes to both: the wire, for a session from a desk,
   and the LCD, so the machine is a machine on its own. */
static void drain(void) {
    uint8_t buf[256];
    size_t n = term_out_read(&M.t, buf, sizeof(buf));
    bool painted = false;
    while (n > 0) {
        Serial.write(buf, n);
        vt_feed(buf, n);
        painted = true;
        n = term_out_read(&M.t, buf, sizeof(buf));
    }
    if (painted) {
        vt_flush();
    }
}

void setup() {
    /* The wire first, and a word after each step. A peripheral that hangs
       is invisible if the line is opened after it — which is how a whole
       evening went by with a board that said nothing. */
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {
    }
    Serial.print("\r\nboot\r\n");
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    vt_begin(20, 8);
    Serial.print("tela ok\r\n");
    sd_begin();
    Serial.printf("card: %s\r\n", sd_why());
    roc_host host = {};
    host.request = on_request;
    /* no "@": at twenty columns the prompt would leave two of them to
       type in */
    host.host_name = "";
    if (sd_mounted()) {
        host.file_get = file_get;
        host.file_put = file_put;
        host.file_del = file_del;
    }
    /* The panel is the terminal: 240x135 px at the size that can be read
       without glasses is twelve by sixteen, so twenty columns by eight. */
    roc_init(&M, &host, 20, 8, ROC_F_NO_SPLASH);
    last_ms = millis();
    drain();

}

/* The keyboard on the lid. Only a change is read, or a held key would
   repeat as fast as the loop runs; Fn turns the arrows into what the shell
   expects, which is what a terminal would have sent over a wire. */
static size_t keyboard(uint8_t *in, size_t cap) {
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
        return 0;
    }
    Keyboard_Class::KeysState k = M5Cardputer.Keyboard.keysState();
    size_t n = 0;
    for (auto c : k.word) {
        if (n >= cap) {
            break;
        }
        uint8_t b = (uint8_t)c;
        if (k.ctrl && b >= 'a' && b <= 'z') {
            b = (uint8_t)(b - 'a' + 1); /* Ctrl-A is 1, as a terminal sends */
        } else if (k.ctrl && b >= 'A' && b <= 'Z') {
            b = (uint8_t)(b - 'A' + 1);
        }
        in[n] = b;
        n++;
    }
    if (k.enter && n < cap) {
        in[n] = '\r';
        n++;
    }
    if (k.del && n < cap) {
        in[n] = 0x7F;
        n++;
    }
    if (k.tab && n < cap) {
        in[n] = '\t';
        n++;
    }
    return n;
}

void loop() {
    uint8_t in[64];
    size_t n = keyboard(in, sizeof(in));
    while (Serial.available() > 0 && n < sizeof(in)) {
        in[n] = (uint8_t)Serial.read();
        n++;
    }
    size_t keep = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0x1D) { /* Ctrl-] */
            report();
            continue;
        }
        if (in[i] == 0x1C) { /* Ctrl-\\ */
            wifi_probe();
            continue;
        }
        if (in[i] == 0x1E) { /* Ctrl-^ : what the LCD is showing */
            static char grid[1400];
            size_t g = vt_dump(grid, sizeof(grid));
            Serial.print("\r\n--- LCD ---\r\n");
            Serial.write((const uint8_t *)grid, g);
            Serial.print("--- fim ---\r\n");
            continue;
        }
        in[keep] = in[i];
        keep++;
    }
    if (keep > 0) {
        roc_input(&M, in, keep);
    }
    if (req_pending) {
        serve();
    }
    uint32_t now = millis();
    if (now - last_ms >= 50) {
        roc_tick(&M, now - last_ms);
        last_ms = now;
    }
    drain();
}
