/* See stall_detect.h. */
#include "stall_detect.h"

#include <string.h>

const char *const hv_stall_counter_names[HV_STALL_COUNTERS] = {
    "translations", "block_flushes", "flush_full", "flush_smc",   "syncs",         "sync_ms",
    "dev_waits",    "dev_wait_ms",   "tr_waits",   "tr_wait_ms",  "code_protects", "refills",
};

static const char *const what_names[HV_SAMPLE_KINDS] = {"guest_app", "guest_kernel", "engine", "waiting", "idle",
                                                         "guest_spin", "fault"};

const char *hv_stall_kind_name(int kind) {
    switch (kind) {
    case HV_STALL_HITCH: return "hitch";
    case HV_STALL_RESPONSE: return "response";
    case HV_STALL_HANG: return "hang";
    case HV_STALL_APP: return "app";
    case HV_STALL_APP_HANG: return "app_hang";
    case HV_STALL_STARVED: return "starved";
    default: return "none";
    }
}

void hv_stall_reset(HvStallDetector *d) { memset(d, 0, sizeof *d); }

static void push_frame(HvStallDetector *d, uint64_t t) { d->ft[d->nft++ % HV_STALL_FT] = t; }

/* Frames that came within span before end (end itself included). */
static int frames_within(const HvStallDetector *d, uint64_t end, uint64_t span) {
    int n = 0;
    for (unsigned i = 0; i < HV_STALL_FT && i < d->nft; i++) {
        uint64_t t = d->ft[(d->nft - 1 - i) % HV_STALL_FT];
        if (t > end || end - t > span) break;
        n++;
    }
    return n;
}

int hv_stall_step(HvStallDetector *d, uint64_t now_us, uint64_t frames, uint64_t input_us, uint64_t press_us,
                  HvStallEvent *ev) {
    if (!d->last_frame_us) { /* the first look: nothing to compare with yet */
        d->frames = frames;
        d->last_frame_us = now_us;
        push_frame(d, now_us);
        return 0;
    }
    if (!d->first_press_us && press_us > d->last_frame_us && press_us <= now_us) d->first_press_us = press_us;
    if (frames != d->frames) {
        HvStallEvent e = {.end_us = now_us, .ended_hang = d->hang_reported};
        e.frames_before = frames_within(d, d->last_frame_us, HV_STALL_FLOW_US);
        uint64_t gap = now_us - d->last_frame_us;
        int got = 0;
        if (d->first_press_us && now_us - d->first_press_us >= HV_STALL_RESPONSE_US &&
            now_us - d->first_press_us <= HV_STALL_MAX_US) {
            e.kind = HV_STALL_RESPONSE, e.start_us = d->first_press_us, got = 1;
        } else if (e.frames_before >= HV_STALL_FLOW_FRAMES && gap >= HV_STALL_HITCH_US && gap <= HV_STALL_MAX_US &&
                   (gap <= HV_STALL_QUIET_US || input_us > d->last_frame_us)) {
            e.kind = HV_STALL_HITCH, e.start_us = d->last_frame_us, got = 1;
        }
        d->frames = frames;
        d->last_frame_us = now_us;
        push_frame(d, now_us);
        d->first_press_us = 0;
        d->hang_reported = 0;
        if (got) *ev = e;
        return got;
    }
    if (d->first_press_us && !d->hang_reported && now_us - d->first_press_us >= HV_STALL_HANG_US) {
        d->hang_reported = 1;
        *ev = (HvStallEvent){.kind = HV_STALL_HANG,
                             .start_us = d->first_press_us,
                             .end_us = now_us,
                             .frames_before = frames_within(d, d->last_frame_us, HV_STALL_FLOW_US)};
        return 1;
    }
    return 0;
}

/* How slowly the frames were flowing: the longest wait between two frames both within span before end, leaving out
   waits of HV_STALL_HITCH_US or more (those are stalls, not the pace: the first frame after a still screen or a hitch
   pairs with the last one before it). */
static uint64_t pace_within(const HvStallDetector *d, uint64_t end, uint64_t span) {
    uint64_t slowest = 0;
    for (unsigned i = 0; i + 1 < HV_STALL_FT && i + 1 < d->nft; i++) {
        uint64_t t = d->ft[(d->nft - 1 - i) % HV_STALL_FT], before = d->ft[(d->nft - 2 - i) % HV_STALL_FT];
        if (t > end || end - before > span) break;
        if (t - before < HV_STALL_HITCH_US && t - before > slowest) slowest = t - before;
    }
    return slowest;
}

int hv_stall_watch(const HvStallDetector *d, uint64_t now_us, uint64_t frames, uint64_t input_us, uint64_t ping_wait_us) {
    if (ping_wait_us >= HV_STALL_WATCH_US) return 1;
    /* A frame since the last step: no gap, and the step ends any press's wait */
    if (!d->last_frame_us || now_us < d->last_frame_us || frames != d->frames) return 0;
    if (d->first_press_us && now_us >= d->first_press_us && now_us - d->first_press_us >= HV_STALL_WATCH_US &&
        now_us - d->first_press_us <= HV_STALL_MAX_US)
        return 1;
    uint64_t gap = now_us - d->last_frame_us;
    uint64_t start = pace_within(d, d->last_frame_us, HV_STALL_FLOW_US) + HV_STALL_WATCH_LATE_US;
    start = start < HV_STALL_WATCH_US ? HV_STALL_WATCH_US : start > HV_STALL_WATCH_MAX_US ? HV_STALL_WATCH_MAX_US : start;
    return gap >= start && gap <= HV_STALL_MAX_US && (gap <= HV_STALL_QUIET_US || input_us > d->last_frame_us) &&
           frames_within(d, d->last_frame_us, HV_STALL_FLOW_US) >= HV_STALL_FLOW_FRAMES;
}

/* A full bank and the credit each sampled tick earns as it goes last HV_STALL_BUDGET_TICKS sampled ticks in a row. */
#define BANK (HV_STALL_BUDGET_TICKS * (HV_STALL_BUDGET_RATE - 1))

void hv_stall_budget_fill(HvStallBudget *b) { b->credit = BANK; }

int hv_stall_budget_take(HvStallBudget *b, int watching) {
    b->credit++;
    int take = watching && b->credit >= HV_STALL_BUDGET_RATE;
    if (take) b->credit -= HV_STALL_BUDGET_RATE;
    if (b->credit > BANK) b->credit = BANK;
    return take;
}

/* ---- the record ---- */

#define MAX_STACKS 64
#define TOP_STACKS 3
#define SHOWN_FRAMES (1 + HV_STALL_BT)
typedef struct {
    uint8_t what, n_addr;
    uint64_t addr[1 + HV_STALL_BT];
    uint32_t n;
} Stack;

/* The addresses that tell one stack from another: none for guest code (the block's VA says nothing on its own), else
   the PC and the return addresses. */
static int stack_key(const HvStallSample *s, uint64_t *addr) {
    if (s->what == HV_SAMPLE_GUEST_APP || s->what == HV_SAMPLE_GUEST_KERNEL) return 0;
    int k = 0;
    addr[k++] = s->pc;
    for (int i = 0; i < s->nbt && i < HV_STALL_BT; i++) addr[k++] = s->bt[i];
    return k;
}

const char *const hv_stall_gfx_names[HV_STALL_GFX] = {"queued", "worker", "gpu", "barrier", "drawable"};

uint32_t hv_stall_gfx_overlap(uint64_t t_us, uint32_t age_ms, uint64_t start_us, uint64_t end_us) {
    if (!age_ms || end_us <= start_us) return 0;
    uint64_t age_us = (uint64_t)age_ms * 1000, from = t_us > age_us ? t_us - age_us : 0, to = t_us < end_us ? t_us : end_us;
    if (from < start_us) from = start_us;
    return to > from ? (uint32_t)((to - from) / 1000) : 0;
}

void hv_stall_gfx_held(const HvStallGfxSnap *s, unsigned n, uint64_t start_us, uint64_t end_us, uint32_t out[HV_STALL_GFX]) {
    memset(out, 0, HV_STALL_GFX * sizeof *out);
    for (unsigned k = 0; k < n; k++) {
        if (s[k].t_us < start_us || s[k].t_us > end_us + 50000) continue;
        for (int g = 0; g < HV_STALL_GFX; g++) {
            uint32_t held = hv_stall_gfx_overlap(s[k].t_us, s[k].ms[g], start_us, end_us);
            if (held > out[g]) out[g] = held;
        }
    }
}

static void put_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fputc('\\', f), fputc(c, f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int busiest(const uint32_t *cat, int n) {
    int best = 0;
    for (int i = 1; i < n; i++)
        if (cat[i] > cat[best]) best = i;
    return best;
}

const char *hv_stall_write(FILE *f, const HvStallRecordInfo *info, const HvStallSample *s, int n, const char *const *names,
                           int nthreads, HvStallLabel label, char *line, size_t line_len) {
    const HvStallEvent *ev = info->ev;
    static Stack stacks[MAX_STACKS];
    uint32_t cpu_cat[HV_SAMPLE_KINDS] = {0};
    size_t used = 0;
    double ms = (double)(ev->end_us - ev->start_us) / 1000.0;
    if (line_len) used = (size_t)snprintf(line, line_len, "freeze: %s %.0f ms", hv_stall_kind_name(ev->kind), ms);

    /* The verdict first, from every guest CPU together. */
    for (int i = 0; i < n; i++) {
        if (s[i].t_us < ev->start_us || s[i].t_us > ev->end_us || s[i].what >= HV_SAMPLE_KINDS || s[i].thread >= nthreads)
            continue;
        if (!strncmp(names[s[i].thread], "cpu", 3)) cpu_cat[s[i].what]++;
    }
    uint32_t cpu_total = 0;
    for (int k = 0; k < HV_SAMPLE_KINDS; k++) cpu_total += cpu_cat[k];
    /* The busiest of these, the first on a tie */
    static const char *const busy_names[] = {"guest", "guest_spin", "engine", "waiting", "fault"};
    uint32_t busy[] = {cpu_cat[HV_SAMPLE_GUEST_APP] + cpu_cat[HV_SAMPLE_GUEST_KERNEL], cpu_cat[HV_SAMPLE_GUEST_SPIN],
                       cpu_cat[HV_SAMPLE_ENGINE], cpu_cat[HV_SAMPLE_WAITING], cpu_cat[HV_SAMPLE_FAULT]};
    uint32_t busy_total = 0;
    for (int k = 0; k < 5; k++) busy_total += busy[k];
    const char *verdict;
    if (ev->kind == HV_STALL_APP || ev->kind == HV_STALL_APP_HANG) verdict = "app";
    else if (ev->kind == HV_STALL_STARVED) verdict = "starved";
    else if (!cpu_total || busy_total * 10 < cpu_total) {
        uint32_t held = 0;
        for (int k = 0; k < HV_STALL_GFX; k++)
            if (info->gfx_ms[k] > held) held = info->gfx_ms[k];
        verdict = held * 2000.0 >= (double)(ev->end_us - ev->start_us) ? "graphics" : "idle";
    }
    else verdict = busy_names[busiest(busy, 5)];
    if (line_len && used < line_len) used += (size_t)snprintf(line + used, line_len - used, ", %s", verdict);
    for (int k = 0; k < HV_STALL_GFX; k++)
        if (info->gfx_ms[k] && line_len && used < line_len)
            used += (size_t)snprintf(line + used, line_len - used, "; gfx %s %u ms", hv_stall_gfx_names[k], info->gfx_ms[k]);

    fprintf(f, "{\"v\":1,\"kind\":\"%s\",\"when\":", hv_stall_kind_name(ev->kind));
    put_string(f, info->when ? info->when : "");
    fprintf(f, ",\"uptime_s\":%.1f,\"ms\":%.0f,\"ended_hang\":%d,\"frames_before\":%d,\"verdict\":\"%s\"",
            info->uptime_s, ms, ev->ended_hang, ev->frames_before, verdict);
    fprintf(f, ",\"thermal\":%d,\"avail_mb\":%lld,\"covered_ms\":%.0f,\"image\":", info->thermal,
            (long long)info->avail_mb, (double)info->covered_us / 1000.0);
    put_string(f, info->image ? info->image : "");
    fprintf(f, ",\"counters\":{");
    for (int k = 0; k < HV_STALL_COUNTERS; k++)
        fprintf(f, "%s\"%s\":%llu", k ? "," : "", hv_stall_counter_names[k], (unsigned long long)info->counters[k]);
    fprintf(f, "},\"gfx\":{");
    for (int k = 0; k < HV_STALL_GFX; k++)
        fprintf(f, "%s\"%s_ms\":%u", k ? "," : "", hv_stall_gfx_names[k], info->gfx_ms[k]);
    fprintf(f, "},\"threads\":[");

    int first_thread = 1;
    for (int t = 0; t < nthreads; t++) {
        uint32_t cat[HV_SAMPLE_KINDS] = {0}, total = 0;
        int nstacks = 0;
        for (int i = 0; i < n; i++) {
            const HvStallSample *x = &s[i];
            if (x->thread != t || x->t_us < ev->start_us || x->t_us > ev->end_us || x->what >= HV_SAMPLE_KINDS) continue;
            cat[x->what]++, total++;
            if (x->what == HV_SAMPLE_IDLE) continue; /* where an idle CPU sleeps says nothing */
            uint64_t key[1 + HV_STALL_BT];
            int nk = stack_key(x, key);
            int j = 0;
            for (; j < nstacks; j++)
                if (stacks[j].what == x->what && stacks[j].n_addr == nk && !memcmp(stacks[j].addr, key, (size_t)nk * 8)) break;
            if (j == nstacks) {
                if (nstacks == MAX_STACKS) continue;
                stacks[j] = (Stack){.what = x->what, .n_addr = (uint8_t)nk};
                memcpy(stacks[j].addr, key, (size_t)nk * 8);
                nstacks++;
            }
            stacks[j].n++;
        }
        if (!total) continue;
        fprintf(f, "%s{\"name\":", first_thread ? "" : ",");
        first_thread = 0;
        put_string(f, names[t]);
        fprintf(f, ",\"samples\":%u", total);
        for (int k = 0; k < HV_SAMPLE_KINDS; k++) fprintf(f, ",\"%s\":%u", what_names[k], cat[k]);
        fprintf(f, ",\"stacks\":[");
        int top = busiest(cat, HV_SAMPLE_KINDS);
        const char *top_where = NULL;
        static char where_buf[160];
        for (int r = 0; r < TOP_STACKS; r++) {
            int b = -1;
            for (int j = 0; j < nstacks; j++)
                if (stacks[j].n && (b < 0 || stacks[j].n > stacks[b].n)) b = j;
            if (b < 0) break;
            fprintf(f, "%s{\"what\":\"%s\",\"n\":%u,\"at\":[", r ? "," : "", what_names[stacks[b].what], stacks[b].n);
            for (int a = 0; a < stacks[b].n_addr && a < SHOWN_FRAMES; a++) {
                char buf[160];
                label(stacks[b].addr[a], buf, sizeof buf);
                if (a) fputc(',', f);
                put_string(f, buf);
                /* For the log line: the first frame in the app itself of the thread's main stack. */
                if (r == 0 && !top_where && stacks[b].what == top && !strncmp(buf, "app+", 4)) {
                    snprintf(where_buf, sizeof where_buf, "%s", buf);
                    top_where = where_buf;
                }
            }
            fprintf(f, "]}");
            stacks[b].n = 0;
        }
        fprintf(f, "]}");
        /* The main thread blocked is the main thread idle in its run loop, unless the stall is its own. */
        int main_idle = !strcmp(names[t], "main") && top == HV_SAMPLE_WAITING && strcmp(verdict, "app");
        if (line_len && used < line_len && top != HV_SAMPLE_IDLE && !main_idle) {
            used += (size_t)snprintf(line + used, line_len - used, "; %s %s %u%%", names[t], what_names[top],
                                     100 * cat[top] / total);
            if (top_where && used < line_len) used += (size_t)snprintf(line + used, line_len - used, " at %s", top_where);
        }
    }
    fprintf(f, "]}\n");
    return verdict;
}
