#include "panda_mp3.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "panda_audio.h"
#include "panda_board.h"
#include "panda_board_pins.h"

static const char *TAG = "mp3";

#define MP3_DIR_NAME "Music"
#define MP3_IN_BUF 4096
#define MP3_TASK_STACK (6 * 1024)
#define MP3_TX_STACK (4 * 1024)
/* LVGL runs at priority 4: a decode burst that loses the CPU for longer than
 * the 65 ms I2S DMA depth was audible as a click. */
#define MP3_TASK_PRIO 8
#define MP3_TX_PRIO 9
/* 64 KB PSRAM, ~0.7 s at 48 kHz. Frame-by-frame writes had no slack at all,
 * so every SD or scheduler stall longer than the DMA depth broke the sound. */
#define MP3_RING_SAMPLES (32 * 1024)
#define MP3_PREBUF_SAMPLES (16 * 1024) /* ~340 ms before the first DAC write */
#define MP3_CHUNK_SAMPLES 1024         /* ~23 ms at 44.1 kHz */
#define MP3_FADE_SAMPLES 441           /* 10 ms at 44.1 kHz */
#define MP3_DRAIN_MS 90                /* > 12 x 240 frames of DMA @44.1 kHz */

typedef enum {
    MP3_CMD_NONE = 0,
    MP3_CMD_TOGGLE,
    MP3_CMD_NEXT,
    MP3_CMD_PREV,
} mp3_cmd_t;

static SemaphoreHandle_t s_ctrl;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;    /* decoder */
static TaskHandle_t s_tx_task; /* I2S writer */

/* Decoder -> writer PCM queue in PSRAM. Keeping the two roles in separate
 * tasks is what makes the prebuffer useful: a slow SD read or a long frame
 * decode no longer stops the DAC feed. */
static int16_t *s_ring;
static int16_t *s_stage; /* writer's copy-out chunk */
static size_t s_ring_head;
static size_t s_ring_tail;
static size_t s_ring_count;
static SemaphoreHandle_t s_ring_lock;
/* The config page upload handler and the menu music page can scan at the
 * same time; the tables are globals, so serialize the rebuild. */
static SemaphoreHandle_t s_scan_lock;
static volatile bool s_primed;  /* prebuffer reached, writer may start */
static volatile bool s_eof;     /* decoder reached the end of this file */
static volatile bool s_tx_idle; /* writer empty and DMA drained */
static volatile bool s_abort;   /* fade out now: track change or stop */
static volatile bool s_fade_in;
static volatile uint32_t s_underruns;

static char s_dir[96];
/* Track name tables live in PSRAM: 2 x 64 x 96 bytes of .bss would otherwise
 * shrink the internal heap that TLS and FATFS need. */
static char (*s_files)[PANDA_MP3_NAME_MAX];
static char (*s_titles)[PANDA_MP3_NAME_MAX];
static int s_count;

static bool tables_alloc(void)
{
    if (s_files != NULL) {
        return true;
    }
    s_files = heap_caps_malloc(PANDA_MP3_MAX_FILES * PANDA_MP3_NAME_MAX,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_titles = heap_caps_malloc(PANDA_MP3_MAX_FILES * PANDA_MP3_NAME_MAX,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_files == NULL || s_titles == NULL) {
        heap_caps_free(s_files);
        heap_caps_free(s_titles);
        s_files = NULL;
        s_titles = NULL;
        return false;
    }
    return true;
}

static volatile int s_index = -1;
static volatile int s_cur_index = -1;
static volatile mp3_cmd_t s_cmd = MP3_CMD_NONE;
static volatile bool s_stop;
static volatile bool s_paused;
static volatile panda_mp3_state_t s_state;
static char s_cur_title[PANDA_MP3_NAME_MAX];

static bool has_mp3_ext(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
}

static void title_from_name(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
}

static size_t utf8_put(char *out, size_t pos, size_t cap, uint32_t cp)
{
    if (cp < 0x80) {
        if (pos + 1 < cap) out[pos++] = (char)cp;
    } else if (cp < 0x800) {
        if (pos + 2 < cap) {
            out[pos++] = (char)(0xC0 | (cp >> 6));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (pos + 3 < cap) {
            out[pos++] = (char)(0xE0 | (cp >> 12));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return pos;
}

/* Minimal ID3v2 TIT2 reader: enough for latin-1 and UTF-8 tags, which covers
 * the files people actually drop on an SD card. Anything else falls back to
 * the file name. */
static void read_id3_title(FILE *f, char *out, size_t cap)
{
    unsigned char hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "ID3", 3) != 0) {
        return;
    }
    int tag_size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                   ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
    if (tag_size <= 0 || tag_size > 65536) {
        return;
    }
    unsigned char *tag = heap_caps_malloc(tag_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tag == NULL) {
        return;
    }
    int got = (int)fread(tag, 1, tag_size, f);
    for (int i = 0; i + 10 <= got; ) {
        if (memcmp(tag + i, "TIT2", 4) != 0) {
            /* Frame id is 4 chars, then a 4-byte size (v2.4 syncsafe is rare
             * enough that treating both as plain big-endian only mis-skips
             * exotic tags, which then fall back to the file name). */
            int fsize = (tag[i + 4] << 24) | (tag[i + 5] << 16) |
                        (tag[i + 6] << 8) | tag[i + 7];
            if (fsize <= 0) {
                break;
            }
            i += 10 + fsize;
            continue;
        }
        int fsize = (tag[i + 4] << 24) | (tag[i + 5] << 16) |
                    (tag[i + 6] << 8) | tag[i + 7];
        if (fsize <= 1 || i + 10 + fsize > got) {
            break;
        }
        const unsigned char *body = tag + i + 10;
        unsigned char enc = body[0];
        size_t pos = 0;
        if (enc == 0) {
            for (int k = 1; k < fsize && body[k] != 0 && pos + 2 < cap; k++) {
                pos = utf8_put(out, pos, cap, body[k]);
            }
        } else if (enc == 3) {
            for (int k = 1; k < fsize && body[k] != 0 && pos + 1 < cap; k++) {
                out[pos++] = (char)body[k];
            }
        } else {
            pos = 0; /* UTF-16 tags: keep the file name instead */
        }
        out[pos] = '\0';
        break;
    }
    heap_caps_free(tag);
}

static void track_title(const char *path, const char *name, char *out, size_t cap)
{
    title_from_name(name, out, cap);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    char id3[PANDA_MP3_NAME_MAX] = {0};
    read_id3_title(f, id3, sizeof(id3));
    fclose(f);
    if (id3[0] != '\0') {
        snprintf(out, cap, "%s", id3);
    }
}

static int scan_locked(void)
{
    if (s_task != NULL) {
        return s_count;
    }
    const char *mount = panda_board_sdcard_mount_point();
    if (!panda_board_sdcard_mounted() || mount == NULL || !tables_alloc()) {
        s_count = 0;
        return 0;
    }

    s_count = 0;
    snprintf(s_dir, sizeof(s_dir), "%s/%s", mount, MP3_DIR_NAME);
    DIR *dir = opendir(s_dir);
    if (dir == NULL) {
        snprintf(s_dir, sizeof(s_dir), "%s/music", mount);
        dir = opendir(s_dir);
    }
    if (dir == NULL) {
        ESP_LOGW(TAG, "no %s/%s on the SD card", mount, MP3_DIR_NAME);
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < PANDA_MP3_MAX_FILES) {
        if (ent->d_name[0] == '.' || !has_mp3_ext(ent->d_name)) {
            continue;
        }
        snprintf(s_files[s_count], PANDA_MP3_NAME_MAX, "%s", ent->d_name);
        s_count++;
    }
    closedir(dir);

    for (int i = 1; i < s_count; i++) {
        char tmp[PANDA_MP3_NAME_MAX];
        memcpy(tmp, s_files[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcasecmp(s_files[j], tmp) > 0) {
            memcpy(s_files[j + 1], s_files[j], sizeof(tmp));
            j--;
        }
        memcpy(s_files[j + 1], tmp, sizeof(tmp));
    }
    /* Read the ID3 tag only once, after sorting, so title[i] matches file[i].
     * Each read is an SD open plus up to 64 KB and dominates the scan time. */
    for (int i = 0; i < s_count; i++) {
        char path[192];
        snprintf(path, sizeof(path), "%.95s/%.95s", s_dir, s_files[i]);
        track_title(path, s_files[i], s_titles[i], PANDA_MP3_NAME_MAX);
    }

    ESP_LOGI(TAG, "found %d track(s) in %s", s_count, s_dir);
    return s_count;
}

int panda_mp3_scan(void)
{
    if (s_scan_lock == NULL) {
        return scan_locked();
    }
    /* The config page upload handler and the menu worker can scan at the
     * same time; s_files/s_titles are globals, so rebuild them serially. */
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    int count = scan_locked();
    xSemaphoreGive(s_scan_lock);
    return count;
}

int panda_mp3_count(void)
{
    return s_count;
}

const char *panda_mp3_title(int index)
{
    if (index < 0 || index >= s_count) {
        return "";
    }
    return s_titles[index];
}

const char *panda_mp3_dir(void)
{
    return s_dir;
}

static void skip_id3(FILE *f)
{
    unsigned char hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "ID3", 3) != 0) {
        fseek(f, 0, SEEK_SET);
        return;
    }
    int size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
               ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
    bool footer = (hdr[5] & 0x10) != 0;
    fseek(f, size + (footer ? 10 : 0), SEEK_SET);
}

static int read_more(FILE *f, uint8_t *buf, uint8_t **rp, int *bytes_left,
                     int cap, bool *eof)
{
    int keep = *bytes_left;
    if (keep > 0 && *rp != buf) {
        memmove(buf, *rp, (size_t)keep);
    }
    if (keep > cap) {
        keep = cap;
    }
    int n = (int)fread(buf + keep, 1, (size_t)(cap - keep), f);
    if (n <= 0) {
        *eof = true;
        n = 0;
    }
    *bytes_left = keep + n;
    *rp = buf;
    return n;
}

static void downmix_to_mono(short *pcm, int samples)
{
    int pairs = samples / 2;
    for (int i = 0; i < pairs; i++) {
        int32_t sum = (int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1];
        pcm[i] = (short)(sum / 2);
    }
}

static size_t ring_count(void)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t n = s_ring_count;
    xSemaphoreGive(s_ring_lock);
    return n;
}

static void ring_clear(void)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_ring_head = s_ring_tail = s_ring_count = 0;
    xSemaphoreGive(s_ring_lock);
}

/* Caller holds s_ring_lock. */
static size_t ring_push_locked(const int16_t *pcm, size_t n)
{
    size_t free_n = MP3_RING_SAMPLES - s_ring_count;
    if (n > free_n) {
        n = free_n;
    }
    if (n == 0) {
        return 0;
    }
    size_t first = MP3_RING_SAMPLES - s_ring_head;
    if (first > n) {
        first = n;
    }
    memcpy(&s_ring[s_ring_head], pcm, first * sizeof(int16_t));
    if (n > first) {
        memcpy(&s_ring[0], pcm + first, (n - first) * sizeof(int16_t));
    }
    s_ring_head = (s_ring_head + n) % MP3_RING_SAMPLES;
    s_ring_count += n;
    return n;
}

static size_t ring_pop(int16_t *pcm, size_t max_n)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t n = s_ring_count < max_n ? s_ring_count : max_n;
    if (n > 0) {
        size_t first = MP3_RING_SAMPLES - s_ring_tail;
        if (first > n) {
            first = n;
        }
        memcpy(pcm, &s_ring[s_ring_tail], first * sizeof(int16_t));
        if (n > first) {
            memcpy(pcm + first, &s_ring[0], (n - first) * sizeof(int16_t));
        }
        s_ring_tail = (s_ring_tail + n) % MP3_RING_SAMPLES;
        s_ring_count -= n;
    }
    xSemaphoreGive(s_ring_lock);
    return n;
}

/* Waits for room in short polls so next/prev and stop still take effect while
 * the ring is full (for example while paused). */
static bool ring_push_blocking(const int16_t *pcm, size_t n)
{
    size_t off = 0;
    while (off < n) {
        if (s_stop || s_abort || s_cmd != MP3_CMD_NONE) {
            return false;
        }
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        size_t w = ring_push_locked(pcm + off, n - off);
        xSemaphoreGive(s_ring_lock);
        off += w;
        if (off < n) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return true;
}

static void fade_in_pcm(int16_t *pcm, size_t n)
{
    size_t f = n < MP3_FADE_SAMPLES ? n : MP3_FADE_SAMPLES;
    for (size_t i = 0; i < f; i++) {
        pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)i / (int32_t)f);
    }
}

/* Ramp the whole chunk to zero: pause and track changes cut the stream here. */
static void fade_out_all(int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)(n - 1 - i) / (int32_t)n);
    }
}

static void fade_out_tail(int16_t *pcm, size_t n)
{
    size_t f = n < MP3_FADE_SAMPLES ? n : MP3_FADE_SAMPLES;
    for (size_t i = 0; i < f; i++) {
        size_t k = n - f + i;
        pcm[k] = (int16_t)((int32_t)pcm[k] * (int32_t)(f - 1 - i) / (int32_t)f);
    }
}

static bool wait_tx_idle(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while (!s_tx_idle) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

/* Decodes one frame. Returns 1 = frame ready, 0 = command pending, -1 = end. */
static int decode_frame(FILE *f, HMP3Decoder dec, uint8_t *in, uint8_t **rp,
                        int *bytes_left, bool *eof, int16_t *pcm, MP3FrameInfo *info)
{
    for (;;) {
        if (s_stop || s_abort || s_cmd != MP3_CMD_NONE) {
            return 0;
        }
        if (*bytes_left < MAINBUF_SIZE && !*eof) {
            read_more(f, in, rp, bytes_left, MP3_IN_BUF, eof);
        }
        int off = MP3FindSyncWord(*rp, *bytes_left);
        if (off < 0) {
            if (*eof) {
                return -1;
            }
            /* Keep the last byte: a frame header may straddle reads. */
            if (*bytes_left > 0) {
                in[0] = (*rp)[*bytes_left - 1];
            }
            *bytes_left = *bytes_left > 0 ? 1 : 0;
            *rp = in;
            if (read_more(f, in, rp, bytes_left, MP3_IN_BUF, eof) == 0 && *eof) {
                return -1;
            }
            continue;
        }
        *rp += off;
        *bytes_left -= off;

        int err = MP3Decode(dec, rp, bytes_left, pcm, 0);
        if (err == 0) {
            MP3GetLastFrameInfo(dec, info);
            return 1;
        }
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (*eof) {
                return -1;
            }
            read_more(f, in, rp, bytes_left, MP3_IN_BUF, eof);
        } else if (*bytes_left > 0) {
            (*rp)++;
            (*bytes_left)--;
        } else if (*eof) {
            return -1;
        }
    }
}

static void mp3_tx_task(void *arg)
{
    (void)arg;
    int16_t *stage = s_stage;
    bool pause_written = false;
    bool abort_written = false;

    while (!s_stop) {
        if (s_abort) {
            if (!abort_written) {
                size_t n = ring_pop(stage, MP3_CHUNK_SAMPLES);
                size_t written = 0;
                if (n > 0) {
                    fade_out_all(stage, n);
                    panda_audio_play(stage, n, &written);
                }
                abort_written = true;
            }
            ring_clear();
            vTaskDelay(pdMS_TO_TICKS(MP3_DRAIN_MS));
            abort_written = false;
            /* Clear abort before publishing idle: the decoder resumes on
             * s_tx_idle and must not mistake this for a new stop request. */
            s_abort = false;
            s_primed = false;
            s_tx_idle = true;
            continue;
        }
        if (s_paused) {
            if (!pause_written) {
                size_t n = ring_pop(stage, MP3_CHUNK_SAMPLES);
                size_t written = 0;
                if (n > 0) {
                    fade_out_all(stage, n);
                    panda_audio_play(stage, n, &written);
                }
                pause_written = true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (pause_written) {
            pause_written = false;
            s_fade_in = true;
        }
        if (!s_primed) {
            if (s_eof && ring_count() == 0 && !s_tx_idle) {
                vTaskDelay(pdMS_TO_TICKS(MP3_DRAIN_MS));
                s_tx_idle = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }
        size_t n = ring_pop(stage, MP3_CHUNK_SAMPLES);
        if (n == 0) {
            if (s_eof) {
                if (!s_tx_idle) {
                    vTaskDelay(pdMS_TO_TICKS(MP3_DRAIN_MS));
                    s_tx_idle = true;
                }
                /* Never spin here: this task outranks the decoder, which is
                 * the one that clears s_eof for the next track. */
                vTaskDelay(pdMS_TO_TICKS(5));
            } else {
                s_underruns++;
                if (s_underruns <= 3) {
                    ESP_LOGW(TAG, "buffer underrun (%u)", (unsigned)s_underruns);
                }
                s_fade_in = true;
                vTaskDelay(pdMS_TO_TICKS(3));
            }
            continue;
        }
        bool last = s_eof && ring_count() == 0;
        if (s_fade_in) {
            fade_in_pcm(stage, n);
            s_fade_in = false;
        }
        if (last) {
            fade_out_tail(stage, n);
        }
        size_t written = 0;
        if (panda_audio_play(stage, n, &written) != ESP_OK) {
            s_fade_in = true;
        }
        if (last) {
            vTaskDelay(pdMS_TO_TICKS(MP3_DRAIN_MS));
            s_tx_idle = true;
        }
    }
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

static void mp3_task(void *arg)
{
    (void)arg;
    int index = s_index;
    int fails = 0;

    HMP3Decoder dec = MP3InitDecoder();
    uint8_t *in = heap_caps_malloc(MP3_IN_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* A frame holds up to MAX_NGRAN granules; helix writes up to
     * MAX_NCHAN * MAX_NGRAN * MAX_NSAMP samples and would otherwise run
     * past the end of this buffer and corrupt the heap. */
    int16_t *pcm = heap_caps_malloc(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dec == NULL || in == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "decoder alloc failed (dec=%p in=%p pcm=%p)", dec, in, pcm);
    } else {
        while (!s_stop && s_count > 0) {
            if (index < 0 || index >= s_count) {
                index = 0;
            }
            char path[192];
            snprintf(path, sizeof(path), "%.95s/%.95s", s_dir, s_files[index]);
            FILE *f = fopen(path, "rb");
            if (f == NULL) {
                ESP_LOGE(TAG, "cannot open %s", path);
                if (++fails >= s_count) {
                    break;
                }
                index = (index + 1) % s_count;
                continue;
            }
            fails = 0;

            track_title(path, s_files[index], s_cur_title, sizeof(s_cur_title));
            s_cur_index = index;
            s_paused = false;
            s_state = PANDA_MP3_PLAYING;
            ring_clear();
            s_eof = false;
            s_primed = false;
            s_fade_in = true;
            s_tx_idle = false;

            skip_id3(f);
            bool eof = false;
            int bytes_left = 0;
            uint8_t *rp = in;
            MP3FrameInfo info = {0};
            read_more(f, in, &rp, &bytes_left, MP3_IN_BUF, &eof);
            bool rate_done = false;
            bool switched = false;

            for (;;) {
                if (s_stop || s_abort) {
                    break;
                }
                mp3_cmd_t cmd = s_cmd;
                if (cmd != MP3_CMD_NONE) {
                    s_cmd = MP3_CMD_NONE;
                    if (cmd == MP3_CMD_TOGGLE) {
                        s_paused = !s_paused;
                        s_state = s_paused ? PANDA_MP3_PAUSED : PANDA_MP3_PLAYING;
                        ESP_LOGI(TAG, "%s", s_paused ? "paused" : "resumed");
                        continue;
                    }
                    index = (cmd == MP3_CMD_NEXT ? index + 1 : index - 1);
                    index = (index + s_count) % s_count;
                    s_index = index;
                    switched = true;
                    break;
                }

                int got = decode_frame(f, dec, in, &rp, &bytes_left, &eof, pcm, &info);
                if (got == 0) {
                    continue;
                }
                if (got < 0) {
                    s_eof = true;
                    ESP_LOGI(TAG, "end of track %d/%d at %ld bytes (ring=%u)", index + 1,
                             s_count, (long)ftell(f), (unsigned)ring_count());
                    /* Tracks shorter than the prebuffer must still play. */
                    if (ring_count() > 0) {
                        s_primed = true;
                    }
                    break;
                }

                if (!rate_done) {
                    uint32_t want = (uint32_t)info.samprate;
                    if (want != panda_audio_get_rate()) {
                        /* Reconfiguring the codec mid-stream injects a gap and
                         * a click. With the writer still idle this costs one
                         * short silence at the very start of the track. */
                        panda_audio_mute(true);
                        esp_err_t rerr = panda_audio_set_rate(want);
                        if (rerr != ESP_OK) {
                            ESP_LOGW(TAG, "rate %u failed: %s", (unsigned)want,
                                     esp_err_to_name(rerr));
                        }
                        vTaskDelay(pdMS_TO_TICKS(30));
                        panda_audio_mute(false);
                    }
                    ESP_LOGI(TAG, "play %d/%d: %s (%d Hz, %d ch, %d kbps)", index + 1,
                             s_count, s_cur_title, info.samprate, info.nChans,
                             info.bitrate / 1000);
                    rate_done = true;
                }

                int samples = info.outputSamps;
                if (info.nChans == 2) {
                    downmix_to_mono(pcm, samples);
                    samples /= 2;
                }
                if (!ring_push_blocking(pcm, (size_t)samples)) {
                    continue;
                }
                if (!s_primed && ring_count() >= MP3_PREBUF_SAMPLES) {
                    s_primed = true;
                }
            }

            bool stop_now = s_stop || s_abort;
            fclose(f);
            MP3FreeDecoder(dec);
            dec = NULL;
            if (stop_now) {
                break;
            }
            if (switched) {
                s_abort = true;
                if (!wait_tx_idle(1500)) {
                    ESP_LOGW(TAG, "writer idle timeout on track change");
                }
            } else if (!wait_tx_idle(2000)) {
                ESP_LOGW(TAG, "writer drain timeout at end of track");
            }
            dec = MP3InitDecoder();
            if (dec == NULL) {
                ESP_LOGE(TAG, "decoder reinit failed");
                break;
            }
            if (!switched) {
                index = (index + 1) % s_count;
                s_index = index;
            }
        }
    }

    if (s_tx_task != NULL) {
        s_abort = true;
        wait_tx_idle(800);
        TickType_t start = xTaskGetTickCount();
        while (s_tx_task != NULL &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(1500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (panda_audio_get_rate() != PANDA_AUDIO_RATE) {
        panda_audio_mute(true);
        panda_audio_set_rate(PANDA_AUDIO_RATE);
        vTaskDelay(pdMS_TO_TICKS(20));
        panda_audio_mute(false);
    }
    if (dec != NULL) {
        MP3FreeDecoder(dec);
    }
    heap_caps_free(in);
    heap_caps_free(pcm);

    s_state = PANDA_MP3_IDLE;
    s_cur_index = -1;
    s_cur_title[0] = '\0';
    s_paused = false;
    s_task = NULL;
    ESP_LOGI(TAG, "stopped, audio rate %u Hz, underruns=%u",
             (unsigned)panda_audio_get_rate(), (unsigned)s_underruns);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t panda_mp3_init(void)
{
    if (s_ctrl != NULL) {
        return ESP_OK;
    }
    s_ctrl = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_ring_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(MP3_RING_SAMPLES * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_stage = heap_caps_malloc(MP3_CHUNK_SAMPLES * sizeof(int16_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ctrl == NULL || s_done == NULL || s_ring_lock == NULL ||
        s_scan_lock == NULL || s_ring == NULL || s_stage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_tx_idle = true;
    const char *mount = panda_board_sdcard_mount_point();
    if (mount != NULL) {
        snprintf(s_dir, sizeof(s_dir), "%.79s/%s", mount, MP3_DIR_NAME);
    }
    if (!tables_alloc()) {
        return ESP_ERR_NO_MEM;
    }
    s_state = PANDA_MP3_IDLE;
    return ESP_OK;
}

esp_err_t panda_mp3_stop(void)
{
    if (s_ctrl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctrl, portMAX_DELAY);
    if (s_task == NULL) {
        /* A stop that timed out earlier can leave the worker's completion
         * token behind; drop it so the next stop cannot consume it and
         * report success while the worker is still alive. */
        xSemaphoreTake(s_done, 0);
        xSemaphoreGive(s_ctrl);
        s_state = PANDA_MP3_IDLE;
        return ESP_OK;
    }
    /* Let the writer fade out and drain the DMA before tearing the decoder
     * down, otherwise stopping at a waveform peak is an audible click. */
    if (s_tx_task != NULL) {
        s_abort = true;
        wait_tx_idle(800);
    }
    s_stop = true;
    /* A blocked I2S write can hold the worker for a few hundred ms. */
    BaseType_t done = xSemaphoreTake(s_done, pdMS_TO_TICKS(3000));
    if (done != pdTRUE) {
        ESP_LOGW(TAG, "worker did not exit in time");
        xSemaphoreGive(s_ctrl);
        return ESP_ERR_TIMEOUT;
    }
    s_task = NULL;
    s_state = PANDA_MP3_IDLE;
    xSemaphoreGive(s_ctrl);
    return ESP_OK;
}

esp_err_t panda_mp3_play(int index)
{
    if (s_ctrl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!panda_audio_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        esp_err_t serr = panda_mp3_stop();
        if (serr != ESP_OK || s_task != NULL) {
            /* Starting a second worker would share s_stage and the ring with
             * the old one, corrupting both. Fail instead. */
            ESP_LOGW(TAG, "previous worker did not stop (%s)", esp_err_to_name(serr));
            return serr != ESP_OK ? serr : ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreTake(s_done, 0);
    if (s_count <= 0 && panda_mp3_scan() <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (index < 0 || index >= s_count) {
        index = s_index >= 0 && s_index < s_count ? s_index : 0;
    }

    s_index = index;
    s_stop = false;
    s_abort = false;
    s_cmd = MP3_CMD_NONE;
    s_paused = false;
    s_eof = false;
    s_primed = false;
    s_tx_idle = true;
    s_fade_in = true;
    ring_clear();
    s_state = PANDA_MP3_PLAYING;
    BaseType_t ok = xTaskCreatePinnedToCore(mp3_tx_task, "mp3_tx", MP3_TX_STACK, NULL,
                                            MP3_TX_PRIO, &s_tx_task, 1);
    if (ok != pdPASS) {
        s_tx_task = NULL;
        s_state = PANDA_MP3_IDLE;
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreatePinnedToCore(mp3_task, "mp3", MP3_TASK_STACK, NULL,
                                 MP3_TASK_PRIO, &s_task, 1);
    if (ok != pdPASS) {
        s_stop = true;
        s_task = NULL;
        s_state = PANDA_MP3_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t panda_mp3_toggle(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cmd = MP3_CMD_TOGGLE;
    return ESP_OK;
}

esp_err_t panda_mp3_next(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cmd = MP3_CMD_NEXT;
    return ESP_OK;
}

esp_err_t panda_mp3_prev(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cmd = MP3_CMD_PREV;
    return ESP_OK;
}

panda_mp3_state_t panda_mp3_state(void)
{
    return s_state;
}

bool panda_mp3_active(void)
{
    return s_task != NULL;
}

int panda_mp3_current(void)
{
    return s_cur_index;
}

const char *panda_mp3_current_title(void)
{
    return s_cur_title;
}

uint32_t panda_mp3_underruns(void)
{
    return s_underruns;
}

size_t panda_mp3_buffered(void)
{
    return (s_ring == NULL) ? 0 : ring_count();
}

uint32_t panda_mp3_flags(void)
{
    return (s_primed ? 1u : 0u) | (s_eof ? 2u : 0u) |
           (s_tx_idle ? 4u : 0u) | (s_abort ? 8u : 0u);
}
