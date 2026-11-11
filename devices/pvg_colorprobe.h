/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Colour probe for the display path. Included by pvg_metal.m, after HvReplay's interface, now_seconds() and hv_half_mode.
   Off unless HV_COLOR_PROBE is set; then it costs a few log lines a minute, a sampled statistics pass every 15 s while the screen is
   changing, and nothing per frame.

   Why it exists: macOS on the phone looked pale. The probe showed the guest writes display-encoded values into its half-float surface and
   Henge encoded them a second time (hv_half_mode 1). Mode 0, pass-through, is the fix. The probe stays so that if colours ever look wrong
   again the log can say which stage is at fault instead of guessing. The stages it can tell apart:
     1. what the guest says about the display   (display page words, gamma table descriptor and, if it can be found, the table)
     2. what the guest writes                    (raw pixels of the displayed surface, "src", and a time series of its statistics)
     3. what Henge does with it                  (the mode in force, and a self-check of the post-shader frame against the source)
     4. what Metal hands Core Animation          (raw pixels of the drawable after the screen shader, "post")
     5. how the layer is set up                  (pixel format, colour space, EDR)
   A sixth stage lives outside this file: what the glass shows, from a screenshot of the phone.

   Every line goes to stderr, so it lands in Documents/henge.log (authoritative; the syslog relay can stall) and in os_log. One event per line,
   key=value, greppable:   colorprobe: v=1 src=engine seq=17 t=93.412 event=metrics ...
   v is the schema version, seq counts lines, t is seconds since the probe started. Events:
     probe_start   settings of this run, incl. the colour mode
     color_mode    the mode changed (start, or a mode-linear / mode-encoded file in the probe folder): encoded = pass-through (the fix),
                   linear = sRGB encode applied (the old behaviour, right only for a linear display profile)
     layer         the CAMetalLayer's colour-relevant properties
     swap          a frame handed to the display, logged for the first 3 swaps, whenever what describes the display changes (surface
                   size/format, gamma descriptor) and every 3000th swap. The two alternating display buffers are not news.
     display_page  words of the shared display page the guest reads (colour matrix, primaries, mode, state) when they change,
                   looked at every swap (a matrix can change with the gamma table unchanged: Night Shift, say); the swap counter,
                   mapping and pointer words are ignored
     gamma_check / gamma_raw / gamma_f32 / gamma_u32n / gamma_f16 / gamma_u16n
                   the guest's gamma table, read where the descriptor says, judged by the guest's checksum, and read as four element types
                   against four curves (identity, sRGB encode, gamma-2.2 encode, gamma-2.2 decode)
     gamma_task_search / gamma_task_hit
                   when the surface does not hold the table: every task's memory at the descriptor's offset is searched for it
     metrics       every HV_COLOR_PROBE_METRICS_SECS (15) while swaps happen: frames per second, mean colour, luma percentiles of the
                   displayed surface and of what the screen shows, fractions below 0 / above 1 / near white / near black
     capture       a capture was taken (why=trigger|timer)
     frame_stats / frame_hist / frame_top / frame_grid / half_stats      (tag=src|post) pixel statistics of a capture
     post_check    the captured post-shader frame against what the mode in force should have produced from the source: max difference, pixels
                   over 1, where they are (the pointer is drawn after the shader and shows up here), verdict
     post_done     the post-shader frame was written
   Files (Documents/colorprobe/ or HV_COLOR_PROBE_DIR), NNN = capture number:
     frame-NNN-src.raw   displayed surface, tightly packed, BGRA8 (or RGBA half floats, see the json)
     frame-NNN-post.raw  drawable after the screen shader, tightly packed BGRA8
     gamma-NNN.bin       the gamma table bytes, if found in the surface
     gamma-sum-SSSSSSSS.bin   each different gamma table the guest gives (SSSSSSSS its checksum), wherever it was found:
                         the whole table, to see one table or three and the order of its curve and any scaling
     frame-NNN.json      geometry, format, colour mode and gamma descriptor for the files above
   Controls, by creating an empty file in the probe folder (checked once a second, then deleted):
     trigger        take a capture (or set HV_COLOR_PROBE_SECS=N for one every N seconds)
     mode-linear / mode-encoded    switch hv_half_mode live
   HV_COLOR_PROBE_MAX caps captures (8). */

#include <dispatch/dispatch.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
    int on;
    char dir[1024], trigger[1100], mode_linear[1100], mode_encoded[1100];
    double t0, every, last_auto, metrics_every, metric_t;
    unsigned max, captures;
    uint64_t swaps, metric_swaps;
    uint32_t last_fp, last_sum, searched_sum;
    uint64_t last_len;
    int searched;
    /* the gamma descriptor of the latest swap: {port, 0, u64 mapping, u64 offset, u64 length, entries, sum} */
    uint32_t g_port, g_entries, g_sum;
    uint64_t g_off, g_len;
    uint32_t page_prev[128];
    int page_have;
    uint32_t kept_sums[16]; /* the tables already written to gamma-sum-*.bin */
    unsigned kept;
    dispatch_source_t timer;
    /* the capture waiting for its post-shader frame */
    uint8_t *pend_src;
    size_t pend_bytes;
    uint32_t pend_w, pend_h;
    int pend_half, pend_mode;
    unsigned pend_seq;
} cp;
static _Atomic unsigned cp_logseq;
static _Atomic unsigned cp_post_pending; /* a capture number waiting for the next presented frame */

static const char *cp_mode_name(int m) { return m == 1 ? "linear" : "encoded"; }

__attribute__((format(printf, 2, 3)))
static void cp_log(const char *event, const char *fmt, ...) {
    char body[1500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    fprintf(stderr, "colorprobe: v=1 src=engine seq=%u t=%.3f event=%s %s\n", atomic_fetch_add(&cp_logseq, 1) + 1, now_seconds() - cp.t0, event, body);
}

static void cp_write(const char *name, const void *data, size_t len) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", cp.dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { cp_log("write_failed", "file=%s errno=%d", name, errno); return; }
    if (fwrite(data, 1, len, f) != len) cp_log("write_failed", "file=%s short_write=1", name);
    fclose(f);
}

static void cp_fourcc(uint32_t v, char out[5]) {
    for (int i = 0; i < 4; i++) { char c = (char)(v >> (24 - 8 * i)); out[i] = c >= 32 && c < 127 ? c : '?'; }
    out[4] = 0;
}

/* ---- the gamma table ---- */

typedef struct { const char *name; unsigned elem; float (*get)(const uint8_t *, size_t); } CpFmt;
static float cp_f32(const uint8_t *b, size_t i) { float f; memcpy(&f, b + i * 4, 4); return f; }
static float cp_u32n(const uint8_t *b, size_t i) { uint32_t u; memcpy(&u, b + i * 4, 4); return (float)((double)u / 4294967295.0); }
static float cp_f16(const uint8_t *b, size_t i) { _Float16 h; memcpy(&h, b + i * 2, 2); return (float)h; }
static float cp_u16n(const uint8_t *b, size_t i) { uint16_t u; memcpy(&u, b + i * 2, 2); return u / 65535.0f; }
static float cp_srgb_encode(float x) { return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f; }

/* One channel of a planar table: n entries from p. Appends to out. */
static int cp_channel(char *out, size_t cap, char name, const CpFmt *f, const uint8_t *p, size_t n) {
    float mn = INFINITY, mx = -INFINITY, prev = -INFINITY, e_id = 0, e_srgb = 0, e_g22 = 0, e_g22i = 0;
    int mono = 1;
    unsigned bad = 0;
    for (size_t i = 0; i < n; i++) {
        float v = f->get(p, i), x = n > 1 ? (float)i / (float)(n - 1) : 0;
        if (!isfinite(v)) { bad++; continue; }
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (v < prev - 1e-6f) mono = 0;
        prev = v;
        e_id = fmaxf(e_id, fabsf(v - x));
        e_srgb = fmaxf(e_srgb, fabsf(v - cp_srgb_encode(x)));
        e_g22 = fmaxf(e_g22, fabsf(v - powf(x, 1 / 2.2f)));
        e_g22i = fmaxf(e_g22i, fabsf(v - powf(x, 2.2f)));
    }
    return snprintf(out, cap, " %c=[first=%.4f q1=%.4f mid=%.4f q3=%.4f last=%.4f min=%.4f max=%.4f mono=%d nonfinite=%u err_identity=%.4f err_srgb_enc=%.4f err_g22_enc=%.4f err_g22_dec=%.4f]",
                    name, f->get(p, 0), f->get(p, n / 4), f->get(p, n / 2), f->get(p, n * 3 / 4), f->get(p, n - 1), mn, mx, mono, bad,
                    e_id, e_srgb, e_g22, e_g22i);
}

/* The checksums the guest might have used, of the cp.g_len bytes at t. */
static void cp_sums(const uint8_t *t, uint32_t *s32, uint32_t *s8, uint32_t *sf) {
    uint32_t a = 0, b = 0;
    float f = 0;
    for (uint64_t i = 0; i + 4 <= cp.g_len; i += 4) { uint32_t w; memcpy(&w, t + i, 4); a += w; float x; memcpy(&x, t + i, 4); if (isfinite(x)) f += x; }
    for (uint64_t i = 0; i < cp.g_len; i++) b += t[i];
    *s32 = a, *s8 = b;
    memcpy(sf, &f, 4);
}

static const char *cp_sum_match(uint32_t s32, uint32_t s8, uint32_t sf) {
    return s32 == cp.g_sum ? "u32" : s8 == cp.g_sum ? "u8" : sf == cp.g_sum ? "f32" : "none";
}

/* A table the guest's checksum matched, written whole once per checksum (at most 16 of them). */
static void cp_gamma_keep(const uint8_t *t, const char *where) {
    for (unsigned i = 0; i < cp.kept && i < 16; i++)
        if (cp.kept_sums[i] == cp.g_sum) return;
    if (cp.kept >= 16) return;
    cp.kept_sums[cp.kept++] = cp.g_sum;
    char name[64];
    snprintf(name, sizeof name, "gamma-sum-%08x.bin", cp.g_sum);
    cp_write(name, t, (size_t)cp.g_len);
    cp_log("gamma_kept", "file=%s where=%s len=%llu entries_arg=%u", name, where, (unsigned long long)cp.g_len, cp.g_entries);
}

/* Read the table at t four ways and compare it with the usual curves; it is one the guest's checksum matched. */
static void cp_gamma_analyze(const uint8_t *t, const char *why, unsigned seq, const char *where) {
    cp_gamma_keep(t, where);
    char hex[200] = "";
    for (int i = 0; i < 32 && (uint64_t)i < cp.g_len; i++) snprintf(hex + strlen(hex), sizeof hex - strlen(hex), "%02x", t[i]);
    cp_log("gamma_raw", "why=%s seq=%u where=%s first32=%s as_f32=[%.5f %.5f %.5f %.5f %.5f %.5f] as_u16n=[%.5f %.5f %.5f %.5f %.5f %.5f]", why, seq, where, hex,
           cp_f32(t, 0), cp_f32(t, 1), cp_f32(t, 2), cp_f32(t, 3), cp_f32(t, 4), cp_f32(t, 5),
           cp_u16n(t, 0), cp_u16n(t, 1), cp_u16n(t, 2), cp_u16n(t, 3), cp_u16n(t, 4), cp_u16n(t, 5));
    static const CpFmt fmts[] = {{"f32", 4, cp_f32}, {"u32n", 4, cp_u32n}, {"f16", 2, cp_f16}, {"u16n", 2, cp_u16n}};
    for (unsigned k = 0; k < 4; k++) {
        const CpFmt *f = &fmts[k];
        size_t n = cp.g_len / (3u * f->elem);
        if (n < 8) continue;
        char line[1400];
        int len = snprintf(line, sizeof line, "why=%s seq=%u where=%s layout=planar_rgb entries_per_channel=%zu", why, seq, where, n);
        for (int c = 0; c < 3 && len > 0 && (size_t)len < sizeof line; c++)
            len += cp_channel(line + len, sizeof line - (size_t)len, "RGB"[c], f, t + (size_t)c * n * f->elem, n);
        char ev[24];
        snprintf(ev, sizeof ev, "gamma_%s", f->name);
        cp_log(ev, "%s", line);
    }
}

/* The surface does not hold the table: look for it in every task's memory at the descriptor's offset. */
static void cp_gamma_task_search(HvReplay *r, const char *why) {
    if (!cp.g_len || cp.g_len > 65536) return;
    uint8_t *buf = malloc((size_t)cp.g_len);
    if (!buf) return;
    unsigned readable = 0, nonzero = 0, hits = 0, shown = 0;
    for (uint32_t task = 0; task < 512; task++) {
        if (hv_pvg_lite_task_read(r->lite, task, cp.g_off, buf, (uint32_t)cp.g_len)) continue;
        readable++;
        int nz = 0;
        for (uint64_t i = 0; i < cp.g_len && !nz; i++) nz = buf[i] != 0;
        nonzero += nz;
        uint32_t s32, s8, sf;
        cp_sums(buf, &s32, &s8, &sf);
        const char *m = cp_sum_match(s32, s8, sf);
        int hit = strcmp(m, "none") != 0;
        hits += hit;
        if (hit || (nz && shown < 3)) {
            shown += !hit;
            char where[32];
            snprintf(where, sizeof where, "task%u", task);
            cp_log("gamma_task_hit", "why=%s task=%u nonzero=%d checksum_match=%s sum_guest=%08x sum_u32=%08x sum_u8=%08x sum_f32=%08x", why, task, nz, m, cp.g_sum, s32, s8, sf);
            if (hit) cp_gamma_analyze(buf, why, 0, where);
        }
    }
    cp_log("gamma_task_search", "why=%s off=%llu len=%llu tasks_readable=%u with_nonzero_bytes=%u checksum_hits=%u", why, (unsigned long long)cp.g_off, (unsigned long long)cp.g_len, readable, nonzero, hits);
    free(buf);
}

/* Where the guest's gamma table is in the surface, judged by the guest's own checksum. */
static const uint8_t *cp_gamma_ptr(HvObject *s, const char **base_name, const char **matched) {
    *matched = "none";
    *base_name = "none";
    if (!s || !s->mapped || !cp.g_len || cp.g_len > (1u << 20)) return NULL;
    const uint64_t bases[2] = {s->first, 0};
    const char *names[2] = {"first", "mapping"};
    const uint8_t *fallback = NULL;
    for (int b = 0; b < 2; b++) {
        uint64_t at = bases[b] + cp.g_off;
        if (at + cp.g_len > s->length) continue;
        const uint8_t *p = s->mapped + at;
        uint32_t s32, s8, sf;
        cp_sums(p, &s32, &s8, &sf);
        const char *m = cp_sum_match(s32, s8, sf);
        if (!fallback) { fallback = p; *base_name = names[b]; *matched = m; }
        if (strcmp(m, "none") != 0) { *matched = m; *base_name = names[b]; return p; }
    }
    return fallback;
}

static void cp_gamma_event(HvReplay *r, HvObject *s, const char *why, unsigned seq) {
    const char *base, *m;
    const uint8_t *t = cp_gamma_ptr(s, &base, &m);
    cp_log("gamma_check", "why=%s seq=%u in_surface=%d base=%s off=%llu len=%llu entries_arg=%u sum_guest=%08x checksum_match=%s surface_length=%llu", why, seq, t != NULL,
           base, (unsigned long long)cp.g_off, (unsigned long long)cp.g_len, cp.g_entries, cp.g_sum, m, s ? (unsigned long long)s->length : 0ull);
    if (t && strcmp(m, "none") != 0) cp_gamma_analyze(t, why, seq, "surface");
    else if (cp.g_len && !(cp.searched && cp.searched_sum == cp.g_sum)) { /* once per distinct checksum */
        cp.searched = 1, cp.searched_sum = cp.g_sum;
        cp_gamma_task_search(r, why);
    }
}

/* ---- the display page the guest reads ---- */

static void cp_display_page(HvReplay *r, int force) {
    const uint8_t *page = hv_pvg_lite_display_page(r->lite);
    if (!page) return;
    static const uint16_t ranges[][2] = {{0x00, 0x40}, {0x100, 0x110}, {0x200, 0x230}, {0xa00, 0xa80}, {0xe00, 0xe10}, {0xf80, 0xfc8}};
    uint32_t cur[128], offs[128];
    unsigned n = 0, changed = 0;
    for (unsigned g = 0; g < sizeof ranges / sizeof ranges[0]; g++)
        for (unsigned o = ranges[g][0]; o < ranges[g][1] && n < 128; o += 4, n++) {
            memcpy(&cur[n], page + o, 4);
            offs[n] = o;
            int noisy = o == 0xf98 || o == 0xfb0 || o == 0xe00 || o == 0xe04; /* swap counter, mapping, pointer */
            if ((!cp.page_have || cur[n] != cp.page_prev[n]) && !noisy) changed++;
        }
    if (!changed && !force) return;
    char line[1400];
    int len = snprintf(line, sizeof line, "changed_words=%u nonzero_or_changed=", changed);
    for (unsigned i = 0; i < n && len > 0 && (size_t)len < sizeof line - 24; i++)
        if (cur[i] && (force || !cp.page_have || cur[i] != cp.page_prev[i]))
            len += snprintf(line + len, sizeof line - (size_t)len, "%x=%08x,", offs[i], cur[i]);
    cp_log("display_page", "%s", line);
    memcpy(cp.page_prev, cur, n * 4);
    cp.page_have = 1;
}

/* ---- pixel statistics of a capture ---- */

static void cp_stats_bgra(const char *tag, unsigned seq, const uint8_t *p, size_t row, uint32_t w, uint32_t h) {
    uint64_t sum[3] = {0, 0, 0}, hist[16] = {0}, n = (uint64_t)w * h;
    uint8_t mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0};
    enum { SLOTS = 8192 };
    static uint32_t keys[SLOTS], counts[SLOTS];
    memset(keys, 0, sizeof keys);
    memset(counts, 0, sizeof counts);
    uint64_t overflow = 0, distinct = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *q = p + y * row;
        for (uint32_t x = 0; x < w; x++, q += 4) {
            for (int c = 0; c < 4; c++) { if (q[c] < mn[c]) mn[c] = q[c]; if (q[c] > mx[c]) mx[c] = q[c]; }
            sum[0] += q[2], sum[1] += q[1], sum[2] += q[0];
            hist[((q[2] * 54u + q[1] * 183u + q[0] * 19u) >> 8) >> 4]++;
            uint32_t key = ((uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0]) | 0x1000000u, i = (key * 2654435761u) >> 19;
            for (unsigned probe = 0;; probe++) {
                if (probe == 64) { overflow++; break; }
                uint32_t at = (i + probe) & (SLOTS - 1);
                if (!keys[at]) { keys[at] = key; counts[at] = 1; distinct++; break; }
                if (keys[at] == key) { counts[at]++; break; }
            }
        }
    }
    double mr = (double)sum[0] / (double)n, mg = (double)sum[1] / (double)n, mb = (double)sum[2] / (double)n;
    cp_log("frame_stats", "tag=%s seq=%u w=%u h=%u R[min=%u mean=%.2f max=%u] G[min=%u mean=%.2f max=%u] B[min=%u mean=%.2f max=%u] A[min=%u max=%u] distinct_colors=%llu%s",
           tag, seq, w, h, mn[2], mr, mx[2], mn[1], mg, mx[1], mn[0], mb, mx[0], mn[3], mx[3], (unsigned long long)distinct, overflow ? " (more than the table holds)" : "");
    char line[400];
    int len = snprintf(line, sizeof line, "tag=%s seq=%u bins16=[", tag, seq);
    for (int i = 0; i < 16; i++) len += snprintf(line + len, sizeof line - (size_t)len, "%s%.4f", i ? " " : "", (double)hist[i] / (double)n);
    snprintf(line + len, sizeof line - (size_t)len, "]");
    cp_log("frame_hist", "%s", line);
    len = snprintf(line, sizeof line, "tag=%s seq=%u colors_rrggbb:fraction=[", tag, seq);
    for (int k = 0; k < 10; k++) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0; i < SLOTS; i++) if (keys[i] && counts[i] > best) best = counts[i], at = i;
        if (!best) break;
        len += snprintf(line + len, sizeof line - (size_t)len, "%s%06x:%.4f", k ? " " : "", keys[at] & 0xffffff, (double)best / (double)n);
        counts[at] = 0;
    }
    snprintf(line + len, sizeof line - (size_t)len, "]");
    cp_log("frame_top", "%s", line);
    len = snprintf(line, sizeof line, "tag=%s seq=%u points_x,y=rrggbb=[", tag, seq);
    for (int gy = 0; gy < 3; gy++)
        for (int gx = 0; gx < 3; gx++) {
            uint32_t x = w * (uint32_t)(2 * gx + 1) / 6, y = h * (uint32_t)(2 * gy + 1) / 6;
            const uint8_t *q = p + y * row + (size_t)x * 4;
            len += snprintf(line + len, sizeof line - (size_t)len, "%s%u,%u=%02x%02x%02x", gx + gy ? " " : "", x, y, q[2], q[1], q[0]);
        }
    snprintf(line + len, sizeof line - (size_t)len, "]");
    cp_log("frame_grid", "%s", line);
}

static void cp_stats_half(const char *tag, unsigned seq, const uint8_t *p, size_t row, uint32_t w, uint32_t h) {
    double sum[3] = {0, 0, 0};
    float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    uint64_t above1 = 0, below0 = 0, bad = 0, n = (uint64_t)w * h;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *q = (const uint16_t *)(p + y * row);
        for (uint32_t x = 0; x < w; x++, q += 4)
            for (int c = 0; c < 3; c++) {
                _Float16 hv;
                memcpy(&hv, &q[c], 2);
                float v = (float)hv;
                if (!isfinite(v)) { bad++; continue; }
                sum[c] += v;
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
                if (v > 1.0f) above1++;
                if (v < 0.0f) below0++;
            }
    }
    cp_log("half_stats", "tag=%s seq=%u w=%u h=%u R[min=%.4f mean=%.4f max=%.4f] G[min=%.4f mean=%.4f max=%.4f] B[min=%.4f mean=%.4f max=%.4f] above_1=%llu below_0=%llu nonfinite=%llu",
           tag, seq, w, h, mn[0], sum[0] / (double)n, mx[0], mn[1], sum[1] / (double)n, mx[1], mn[2], sum[2] / (double)n, mx[2],
           (unsigned long long)above1, (unsigned long long)below0, (unsigned long long)bad);
}

/* ---- the time series: cheap sampled statistics of what is on screen ---- */

static void cp_metrics(HvReplay *r) {
    if (cp.metrics_every <= 0 || cp.swaps == cp.metric_swaps) return; /* off, or nothing was drawn since the last one */
    double now = now_seconds(), dt = now - cp.metric_t;
    if (cp.metric_t && dt < cp.metrics_every) return;
    uint64_t mapping = r->on_screen;
    HvObject *s = mapping ? r->surfaces[@(mapping)] : nil;
    if (!s || !s->mapped) return;
    uint32_t w = s->surface_w, h = s->surface_h;
    BOOL half = s->fourcc == 'RGhA';
    int mode = hv_half_mode_get();
    enum { BINS = 64 };
    uint64_t hist[BINS] = {0}, hist_shown[BINS] = {0}, n = 0, neg = 0, over = 0;
    double sum[3] = {0, 0, 0}, luma_shown = 0, luma = 0;
    const uint8_t *src = s->mapped + s->first;
    for (uint32_t y = 4; y < h; y += 8) {
        const uint8_t *row = src + (size_t)y * s->row;
        for (uint32_t x = 4; x < w; x += 8) {
            float v[3];
            if (half) {
                _Float16 hv[3];
                memcpy(hv, row + (size_t)x * 8, 6);
                v[0] = (float)hv[0], v[1] = (float)hv[1], v[2] = (float)hv[2];
            } else {
                const uint8_t *q = row + (size_t)x * 4;
                v[0] = q[2] / 255.0f, v[1] = q[1] / 255.0f, v[2] = q[0] / 255.0f;
            }
            if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) continue;
            n++;
            if (v[0] < 0 || v[1] < 0 || v[2] < 0) neg++;
            if (v[0] > 1 || v[1] > 1 || v[2] > 1) over++;
            float c[3], t[3];
            for (int k = 0; k < 3; k++) {
                c[k] = v[k] < 0 ? 0 : v[k] > 1 ? 1 : v[k];
                t[k] = half ? hv_half_shown(v[k], mode) : c[k];
                sum[k] += c[k];
            }
            float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], ls = 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2];
            luma += l, luma_shown += ls;
            hist[(int)(l * (BINS - 1) + 0.5f)]++;
            hist_shown[(int)(ls * (BINS - 1) + 0.5f)]++;
        }
    }
    if (!n) return;
    double pr[3] = {0.01, 0.50, 0.99}, p_src[3], p_shown[3];
    for (int k = 0; k < 3; k++) {
        uint64_t a = 0, b = 0;
        double target = pr[k] * (double)n;
        int ia = 0, ib = 0;
        for (int i = 0; i < BINS; i++) { a += hist[i]; if ((double)a >= target) { ia = i; break; } }
        for (int i = 0; i < BINS; i++) { b += hist_shown[i]; if ((double)b >= target) { ib = i; break; } }
        p_src[k] = ia / (double)(BINS - 1), p_shown[k] = ib / (double)(BINS - 1);
    }
    uint64_t white = hist[BINS - 1] + hist[BINS - 2], black = hist[0] + hist[1];
    char fc[5];
    cp_fourcc(s->fourcc, fc);
    cp_log("metrics", "fps=%.1f mode=%s surf=%ux%u fourcc=%s samples=%llu mean_rgb_src=[%.3f %.3f %.3f] luma_src_mean=%.3f p01_p50_p99_src=[%.3f %.3f %.3f] luma_shown_mean=%.3f p01_p50_p99_shown=[%.3f %.3f %.3f] below0_frac=%.4f above1_frac=%.4f near_white_frac=%.3f near_black_frac=%.3f",
           cp.metric_t ? (double)(cp.swaps - cp.metric_swaps) / dt : 0.0, cp_mode_name(mode), w, h, fc, (unsigned long long)n, sum[0] / (double)n, sum[1] / (double)n, sum[2] / (double)n,
           luma / (double)n, p_src[0], p_src[1], p_src[2], luma_shown / (double)n, p_shown[0], p_shown[1], p_shown[2],
           (double)neg / (double)n, (double)over / (double)n, (double)white / (double)n, (double)black / (double)n);
    cp.metric_t = now;
    cp.metric_swaps = cp.swaps;
}

/* ---- layer, swaps, captures ---- */

static void cp_log_layer(HvReplay *r, CAMetalLayer *l) { /* a layer being set up to show frames (device or monitor) */
    if (!l) return;
    CGColorSpaceRef cs = l.colorspace;
    CFStringRef name = cs ? CGColorSpaceCopyName(cs) : NULL;
    char buf[160] = "nil(no colour matching)";
    if (name) CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    cp_log("layer", "pixelFormat=%lu colorspace=%s edr=%d framebufferOnly=%d opaque=%d contentsScale=%.2f drawableSize=%.0fx%.0f maxDrawables=%lu device=%s",
           (unsigned long)l.pixelFormat, buf, (int)l.wantsExtendedDynamicRangeContent, (int)l.framebufferOnly, (int)l.isOpaque,
           l.contentsScale, l.drawableSize.width, l.drawableSize.height, (unsigned long)l.maximumDrawableCount, r->device.name.UTF8String);
    if (name) CFRelease(name);
}

/* A frame is handed to the display (the guest's swap command; a = its payload after the header). */
static void cp_note_swap(HvReplay *r, uint64_t mapping, const uint32_t *a, uint32_t n) {
    if (!cp.on) return;
    if (n >= 40) {
        cp.g_port = a[0];
        memcpy(&cp.g_off, a + 4, 8);
        memcpy(&cp.g_len, a + 6, 8);
        cp.g_entries = a[8];
        cp.g_sum = a[9];
    }
    cp.swaps++;
    HvObject *s = r->surfaces[@(mapping)];
    const uint8_t *desc = hv_pvg_lite_surface(r->lite, mapping);
    uint64_t alloc = 0, row = 0, sw = 0, sh = 0;
    uint32_t first = 0, fourcc = 0;
    if (desc) memcpy(&fourcc, desc + 4, 4), memcpy(&alloc, desc + 0x18, 8), memcpy(&sw, desc + 0x20, 8), memcpy(&sh, desc + 0x28, 8), memcpy(&row, desc + 0x30, 8), memcpy(&first, desc + 0x60, 4);
    /* The two display buffers alternate about 30 times a second; what matters is the surface's shape and format and the gamma descriptor. */
    uint32_t fp = fourcc ^ (uint32_t)(sw * 2654435761u) ^ (uint32_t)(sh * 40503u) ^ (uint32_t)row;
    int changed = fp != cp.last_fp || cp.g_sum != cp.last_sum || cp.g_len != cp.last_len;
    if (cp.swaps <= 3 || changed || cp.swaps % 3000 == 0) {
        char fc[5];
        cp_fourcc(fourcc, fc);
        cp_log("swap", "n=%llu mapping=%llu known_surface=%d surf=%llux%llu fourcc=%s row=%llu first=%u alloc=%llu bytes_after_pixels=%lld mode=%s gamma_port=%u gamma_off=%llu gamma_len=%llu gamma_entries=%u gamma_sum=%08x payload_bytes=%u changed=%d",
               (unsigned long long)cp.swaps, (unsigned long long)mapping, s != nil, (unsigned long long)sw, (unsigned long long)sh, fc, (unsigned long long)row, first,
               (unsigned long long)alloc, (long long)alloc - (long long)(row * sh), fourcc == 'RGhA' ? cp_mode_name(hv_half_mode_get()) : "plain(passthrough)", cp.g_port,
               (unsigned long long)cp.g_off, (unsigned long long)cp.g_len, cp.g_entries, cp.g_sum, n, changed);
    }
    cp_display_page(r, cp.swaps == 1); /* every swap: it says only what changed */
    if ((changed || cp.swaps == 1) && (cp.g_len || cp.swaps == 1)) cp_gamma_event(r, s, "swap", 0);
    cp.last_fp = fp;
    cp.last_sum = cp.g_sum;
    cp.last_len = cp.g_len;
}

/* Take a capture of what is on screen now: the source surface here, and the post-shader frame on the next present. */
static void cp_capture(HvReplay *r, const char *why) {
    if (cp.captures >= cp.max) { cp_log("capture_skipped", "why=%s reason=limit_reached max=%u", why, cp.max); return; }
    uint64_t mapping = r->on_screen;
    HvObject *s = mapping ? r->surfaces[@(mapping)] : nil;
    if (!s || !s->mapped) { cp_log("capture_skipped", "why=%s reason=nothing_on_screen_yet", why); return; }
    unsigned seq = ++cp.captures;
    uint32_t w = s->surface_w, h = s->surface_h;
    BOOL half = s->fourcc == 'RGhA';
    int mode = hv_half_mode_get();
    size_t bpp = half ? 8 : 4, tight = (size_t)w * bpp, bytes = tight * h;
    uint8_t *copy = malloc(bytes);
    if (!copy) { cp_log("capture_skipped", "why=%s reason=out_of_memory", why); return; }
    const uint8_t *src = s->mapped + s->first;
    for (uint32_t y = 0; y < h; y++) memcpy(copy + y * tight, src + (size_t)y * s->row, tight);
    char name[64], fc[5];
    cp_fourcc(s->fourcc, fc);
    snprintf(name, sizeof name, "frame-%03u-src.raw", seq);
    cp_write(name, copy, bytes);
    if (half) cp_stats_half("src", seq, copy, tight, w, h);
    else cp_stats_bgra("src", seq, copy, tight, w, h);
    const char *base, *m;
    const uint8_t *table = cp_gamma_ptr(s, &base, &m);
    if (table) {
        snprintf(name, sizeof name, "gamma-%03u.bin", seq);
        cp_write(name, table, (size_t)cp.g_len);
    }
    char json[1000];
    int len = snprintf(json, sizeof json,
                       "{\"v\":2,\"seq\":%u,\"why\":\"%s\",\"t\":%.3f,\"mapping\":%llu,\"width\":%u,\"height\":%u,\"fourcc\":\"%s\",\"src_format\":\"%s\",\"color_mode\":\"%s\",\"row_bytes_in_guest\":%llu,\"first\":%u,"
                       "\"gamma\":{\"port\":%u,\"offset\":%llu,\"length\":%llu,\"entries\":%u,\"sum\":\"%08x\",\"found\":%s,\"base\":\"%s\",\"checksum_match\":\"%s\"},"
                       "\"files\":{\"src\":\"frame-%03u-src.raw\",\"post\":\"frame-%03u-post.raw\",\"gamma\":\"gamma-%03u.bin\"}}\n",
                       seq, why, now_seconds() - cp.t0, (unsigned long long)mapping, w, h, fc, half ? "RGBA16F" : "BGRA8", cp_mode_name(mode), (unsigned long long)s->row, s->first,
                       cp.g_port, (unsigned long long)cp.g_off, (unsigned long long)cp.g_len, cp.g_entries, cp.g_sum, table ? "true" : "false", base, m, seq, seq, seq);
    snprintf(name, sizeof name, "frame-%03u.json", seq);
    cp_write(name, json, (size_t)len);
    cp_log("capture", "seq=%u why=%s mapping=%llu surf=%ux%u fourcc=%s mode=%s src=frame-%03u-src.raw json=frame-%03u.json post=pending", seq, why,
           (unsigned long long)mapping, w, h, fc, cp_mode_name(mode), seq, seq);
    /* keep the source until the post-shader frame arrives, to check one against the other */
    free(cp.pend_src);
    cp.pend_src = copy, cp.pend_bytes = bytes, cp.pend_w = w, cp.pend_h = h, cp.pend_half = half, cp.pend_mode = mode, cp.pend_seq = seq;
    atomic_store(&cp_post_pending, seq);
    [r screenChanged]; /* present again so the post-shader frame can be taken */
}

/* The post-shader frame against what the mode in force should have made of the source. */
static void cp_post_check(const uint8_t *post, uint32_t W, uint32_t H, unsigned seq) {
    if (!cp.pend_src || cp.pend_seq != seq || cp.pend_w != W || cp.pend_h != H) {
        cp_log("post_check", "seq=%u skipped=1 reason=%s", seq, cp.pend_src ? "size_or_sequence_mismatch" : "no_source_kept");
        return;
    }
    const size_t tight = (size_t)W * (cp.pend_half ? 8 : 4);
    uint32_t bad = 0, x0 = W, y0 = H, x1 = 0, y1 = 0;
    int maxdiff = 0;
    for (int pass = 0; pass < 2; pass++) {
        int outside_max = 0;
        for (uint32_t y = 0; y < H; y++) {
            const uint8_t *sp = cp.pend_src + y * tight, *pp = post + (size_t)y * W * 4;
            for (uint32_t x = 0; x < W; x++) {
                int want[3], got[3] = {pp[x * 4 + 2], pp[x * 4 + 1], pp[x * 4]};
                if (cp.pend_half) {
                    _Float16 hv[3];
                    memcpy(hv, sp + (size_t)x * 8, 6);
                    for (int c = 0; c < 3; c++) want[c] = (int)(hv_half_shown((float)hv[c], cp.pend_mode) * 255.0f + 0.5f);
                } else {
                    want[0] = sp[x * 4 + 2], want[1] = sp[x * 4 + 1], want[2] = sp[x * 4];
                }
                int d = 0;
                for (int c = 0; c < 3; c++) d = abs(want[c] - got[c]) > d ? abs(want[c] - got[c]) : d;
                if (pass == 0) {
                    if (d > maxdiff) maxdiff = d;
                    if (d > 1) { bad++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
                } else if (d > 1 && !(x >= x0 && x <= x1 && y >= y0 && y <= y1) && d > outside_max) outside_max = d;
            }
        }
        if (pass == 0 && !bad) { cp_log("post_check", "seq=%u mode=%s max_diff=%d pixels_over_1=0 verdict=matches", seq, cp_mode_name(cp.pend_mode), maxdiff); break; }
        if (pass == 1) {
            double area = (double)(x1 - x0 + 1) * (double)(y1 - y0 + 1), frac = area / ((double)W * H);
            const char *verdict = outside_max <= 1 && frac < 0.01 ? "matches_except_pointer_sized_box" : "MISMATCH";
            cp_log("post_check", "seq=%u mode=%s max_diff=%d pixels_over_1=%u box=%ux%u@%u,%u box_fraction=%.5f max_diff_outside_box=%d verdict=%s", seq, cp_mode_name(cp.pend_mode), maxdiff, bad,
                   x1 - x0 + 1, y1 - y0 + 1, x0, y0, frac, outside_max, verdict);
        }
    }
    free(cp.pend_src);
    cp.pend_src = NULL;
}

/* In the presenter: when a capture is waiting, copy this frame's drawable out after the screen shader has drawn it. */
static void cp_capture_post(HvReplay *r, id<MTLTexture> tex, uint32_t W, uint32_t H, id<MTLCommandBuffer> cb) {
    unsigned seq = atomic_exchange(&cp_post_pending, 0);
    if (!seq) return;
    if (tex.pixelFormat != MTLPixelFormatBGRA8Unorm || tex.framebufferOnly) {
        cp_log("post_skipped", "seq=%u pixelFormat=%lu framebufferOnly=%d", seq, (unsigned long)tex.pixelFormat, (int)tex.framebufferOnly);
        return;
    }
    size_t bytes = (size_t)W * H * 4;
    id<MTLBuffer> rb = [r->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!rb) { cp_log("post_skipped", "seq=%u reason=no_buffer", seq); return; }
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
              toBuffer:rb destinationOffset:0 destinationBytesPerRow:(NSUInteger)W * 4 destinationBytesPerImage:bytes];
    [b endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        dispatch_async(r->worker, ^{
            char name[64];
            snprintf(name, sizeof name, "frame-%03u-post.raw", seq);
            cp_write(name, rb.contents, bytes);
            cp_stats_bgra("post", seq, rb.contents, (size_t)W * 4, W, H);
            cp_post_check(rb.contents, W, H, seq);
            cp_log("post_done", "seq=%u w=%u h=%u file=%s", seq, W, H, name);
        });
    }];
}

static void cp_set_mode(HvReplay *r, int mode, const char *why) {
    int was = hv_half_mode_get();
    atomic_store(&hv_half_mode, mode);
    cp_log("color_mode", "mode=%s was=%s why=%s", cp_mode_name(mode), cp_mode_name(was), why);
    [r screenChanged]; /* draw the current frame again in the new mode */
}

static void cp_tick(HvReplay *r) {
    if (access(cp.mode_linear, F_OK) == 0) { unlink(cp.mode_linear); cp_set_mode(r, 1, "file mode-linear"); return; }
    if (access(cp.mode_encoded, F_OK) == 0) { unlink(cp.mode_encoded); cp_set_mode(r, 0, "file mode-encoded"); return; }
    if (access(cp.trigger, F_OK) == 0) { unlink(cp.trigger); cp_capture(r, "trigger"); return; }
    if (cp.every > 0 && now_seconds() - cp.last_auto >= cp.every) {
        cp.last_auto = now_seconds();
        cp_capture(r, "timer");
        return;
    }
    cp_metrics(r);
}

static void cp_start(HvReplay *r) {
    if (!getenv("HV_COLOR_PROBE")) return;
    cp.on = 1;
    cp.t0 = now_seconds();
    const char *dir = getenv("HV_COLOR_PROBE_DIR"), *home = getenv("HOME");
    if (dir) snprintf(cp.dir, sizeof cp.dir, "%s", dir);
    else snprintf(cp.dir, sizeof cp.dir, "%s/Documents/colorprobe", home ? home : "/tmp");
    mkdir(cp.dir, 0755);
    snprintf(cp.trigger, sizeof cp.trigger, "%s/trigger", cp.dir);
    snprintf(cp.mode_linear, sizeof cp.mode_linear, "%s/mode-linear", cp.dir);
    snprintf(cp.mode_encoded, sizeof cp.mode_encoded, "%s/mode-encoded", cp.dir);
    cp.every = getenv("HV_COLOR_PROBE_SECS") ? atof(getenv("HV_COLOR_PROBE_SECS")) : 0;
    cp.metrics_every = getenv("HV_COLOR_PROBE_METRICS_SECS") ? atof(getenv("HV_COLOR_PROBE_METRICS_SECS")) : 15;
    cp.max = getenv("HV_COLOR_PROBE_MAX") ? (unsigned)atoi(getenv("HV_COLOR_PROBE_MAX")) : 8;
    cp.last_auto = now_seconds();
    cp_log("probe_start", "dir=%s every_s=%.0f metrics_every_s=%.0f max_captures=%u mode=%s built=\"%s %s\"", cp.dir, cp.every, cp.metrics_every, cp.max, cp_mode_name(hv_half_mode_get()), __DATE__, __TIME__);
    cp_log("color_mode", "mode=%s was=none why=start", cp_mode_name(hv_half_mode_get()));
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, r->worker);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    HvReplay *__weak weak = r;
    dispatch_source_set_event_handler(timer, ^{ HvReplay *strong = weak; if (strong) cp_tick(strong); });
    dispatch_resume(timer);
    cp.timer = timer;
}
