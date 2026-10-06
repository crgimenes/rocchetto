/* The card is the filesystem. Without it the board still runs — the tree
   in flash carries /bin — but nothing the user writes outlives a reset and
   there is nothing to read but the board's own files.
 *
 * Mounted through ESP-IDF's FATFS rather than Arduino's SD library, which
 * this build could not be made to link: the IDF way puts the card under a
 * path in the VFS, and from there it is fopen and readdir — the same calls
 * the posix host makes, which is one fewer thing that differs here.
 */

#include "sd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SPI.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

/* The Cardputer's card reader, on the pins M5Stack's own examples name —
   the library does not export them. */
#define SD_SCK 40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS 12

#define MOUNT "/sd"

namespace {

bool mounted = false;
sdmmc_card_t *card = nullptr;
/* kept so the board can say why, instead of just "no" */
esp_err_t last_bus = ESP_OK;
esp_err_t last_mount = ESP_OK;

/* Deep enough for a card someone organised, shallow enough that a bad
   image cannot walk forever. */
const int DEPTH_MAX = 4;

char line[320];

void walk(const char *dir, const char *prefix, void (*flush)(const uint8_t *, size_t), int depth) {
    if (depth > DEPTH_MAX) {
        return;
    }
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return;
    }
    struct dirent *e = readdir(d);
    while (e != nullptr) {
        const char *name = e->d_name;
        if (name[0] != '.') {
            char full[256];
            snprintf(full, sizeof(full), "%s/%s", dir, name);
            struct stat st;
            bool is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            /* the date is "-": there is no clock on this board, and a wrong
               time is worse than none */
            int n = 0;
            if (is_dir) {
                n = snprintf(line, sizeof(line), "%s%s/\t0\t-\t\n", prefix, name);
            } else {
                n = snprintf(line, sizeof(line), "%s%s\t%u\t-\t\n", prefix, name,
                             (unsigned)st.st_size);
            }
            if (n > 0) {
                flush((const uint8_t *)line, (size_t)n);
            }
            if (is_dir) {
                char sub[256];
                snprintf(sub, sizeof(sub), "%s%s/", prefix, name);
                walk(full, sub, flush, depth + 1);
            }
        }
        e = readdir(d);
    }
    closedir(d);
}

/* A path roc asks for, as a path on the card. */
void on_card(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", MOUNT, path);
}

} // namespace

bool sd_begin(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    /* SPI2, initialised here through the IDF. The display is on SPI3 with
       its own pins, and Arduino's SPI object drives registers directly
       without ever registering a host with spi_master — so SPI2 is free as
       far as the sdspi driver is concerned, and the reader gets it whole. */
    host.slot = SPI2_HOST;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI;
    bus.miso_io_num = SD_MISO;
    bus.sclk_io_num = SD_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    last_bus = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (last_bus != ESP_OK) {
        return false;
    }
    /* The default is 20 MHz. Some cards will not answer the first command
       at that speed on a bus this long, and the reader is not where the
       board spends its time anyway. */
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)SD_CS;
    slot.host_id = (spi_host_device_t)host.slot;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    /* a card that will not mount is the user's to look at, never ours to
       erase */
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;
    last_mount = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &cfg, &card);
    mounted = last_mount == ESP_OK;
    return mounted;
}

bool sd_mounted(void) {
    return mounted;
}

const char *sd_why(void) {
    static char out[96];
    snprintf(out, sizeof(out), "%s bus=%s mount=%s", mounted ? "mounted" : "ABSENT",
             esp_err_to_name(last_bus), esp_err_to_name(last_mount));
    return out;
}

void sd_index(void (*flush)(const uint8_t *, size_t)) {
    if (!mounted) {
        return;
    }
    walk(MOUNT, "/", flush, 0);
}

bool sd_read(const char *path, void (*flush)(const uint8_t *, size_t)) {
    if (!mounted) {
        return false;
    }
    char full[256];
    on_card(path, full, sizeof(full));
    FILE *f = fopen(full, "rb");
    if (f == nullptr) {
        return false;
    }
    uint8_t buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    while (n > 0) {
        flush(buf, n);
        n = fread(buf, 1, sizeof(buf), f);
    }
    fclose(f);
    return true;
}

/* One file at a time, which is what a synchronous read needs and all the
   shell ever asks for. Bigger than this and the answer is "no such file",
   honestly: the board cannot hold it. */
static uint8_t slurped[8 * 1024];

const uint8_t *sd_slurp(const char *path, size_t *len) {
    if (!mounted) {
        return nullptr;
    }
    char full[256];
    on_card(path, full, sizeof(full));
    FILE *f = fopen(full, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    size_t n = fread(slurped, 1, sizeof(slurped), f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (more) {
        return nullptr; /* it does not fit; pretending would truncate */
    }
    *len = n;
    return slurped;
}

void sd_mkpath(const char *path) {
    if (!mounted) {
        return;
    }
    char full[256];
    on_card(path, full, sizeof(full));
    for (char *p = full + strlen(MOUNT) + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        mkdir(full, 0777);
        *p = '/';
    }
}

bool sd_unlink(const char *path) {
    if (!mounted) {
        return false;
    }
    char full[256];
    on_card(path, full, sizeof(full));
    return remove(full) == 0;
}

bool sd_write(const char *path, const uint8_t *data, size_t len) {
    if (!mounted) {
        return false;
    }
    char full[256];
    on_card(path, full, sizeof(full));
    FILE *f = fopen(full, "wb");
    if (f == nullptr) {
        return false;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len;
}
