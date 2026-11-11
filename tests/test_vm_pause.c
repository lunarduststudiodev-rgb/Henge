/* host/vm.c's stops, as the guest sees them. Every pause ends in exactly one resume: the time stopped comes off the
   guest's clock, macOS is told to reread the real time (the RTC's woke event) and hv_vm_stops is even again. An
   auto-save pauses and resumes in one call (save()'s tail), whether it kept the save or not, unless a hold came in
   for it (hv_vm_autosave_hold after its hold ticket was taken): then the Mac stays paused as a pause leaves it, with
   no woke event yet and the count odd, and hv_vm_resume ends that one stop. An auto-save of a paused Mac is refused
   and changes nothing.

   A real Mac: host/vm.c on the engine and devices, with a ROM that sleeps in WFI, blank disks and Quick Suspend's
   memory files (HV_RAM_SAVE_AHEAD), so an auto-save runs its whole path. Sound, camera, network and the USB probe
   belong to the host's other files and are left out (stubs below). No GPU: HV_NO_GPU and HV_PVG_NO_METAL are set
   here, before anything is made. */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach/mach_time.h>

#include "audio_mac.h"
#include "audio_usb.h"
#include "camera_apple.h"
#include "net_slirp.h"
#include "sysreg.h"
#include "usb_host.h"
#include "vm.h"

void hv_audio_attach(HvVirtioSnd *d) { (void)d; }
void hv_audio_pause(int pause) { (void)pause; }
void hv_audio_usb_attach(HvUsbAudio *a) { (void)a; }
void hv_audio_usb_pause(int pause) { (void)pause; }
void hv_audio_usb_set_listener(void (*cb)(void *opaque, double level, int mute), void *opaque) { (void)cb, (void)opaque; }
void hv_camera_attach(HvUsbVideo *v) { (void)v; }
void hv_camera_pause(int pause) { (void)pause; }
int hv_net_attach(HvMachine *m, HvVirtioNet *n) { (void)m, (void)n; return -1; }
void hv_net_poll(void) {}
void hv_usb_probe_start(HvVirtioUsb *d) { (void)d; }

static int checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("  FAIL line %d: ", __LINE__);                                                                      \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static double ms(uint64_t ticks) { return ticks / 24000.0; }
static uint64_t bias(void) { return __atomic_load_n(&hv_time_bias, __ATOMIC_SEQ_CST); }

/* Woke events since the last look (the RTC's pending bit: no driver here takes it), cleared. */
static int woke(void) {
    HvVmapple *b = hv_vm_board();
    pthread_mutex_lock(&b->avp_rtc_lock);
    int r = (b->avp_rtc[5] & HV_AVP_RTC_WOKE) != 0;
    b->avp_rtc[5] &= ~(uint64_t)HV_AVP_RTC_WOKE;
    pthread_mutex_unlock(&b->avp_rtc_lock);
    return r;
}

static pthread_mutex_t exit_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cv = PTHREAD_COND_INITIALIZER;
static int exited;
static void on_exit_cb(int reason) {
    (void)reason;
    pthread_mutex_lock(&exit_mu);
    exited = 1;
    pthread_cond_broadcast(&exit_cv);
    pthread_mutex_unlock(&exit_mu);
}

static char dir[1024];
static int make_file(const char *name, const void *data, size_t len, off_t size, char *path, size_t cap) {
    snprintf(path, cap, "%s/%s", dir, name);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = (!len || write(fd, data, len) == (ssize_t)len) && (!size || ftruncate(fd, size) == 0);
    close(fd);
    return ok ? 0 : -1;
}

static int exists(const char *path) { return access(path, F_OK) == 0; }

/* One auto-save, as Henge makes them: returns its result; *took is the host time it ran. */
static int autosave(const char *name, uint64_t hold_ticket, int clone_fails, uint64_t *took) {
    char snap[1200], d[1200], a[1200], why[256];
    snprintf(snap, sizeof snap, "%s/%s.snap", dir, name);
    snprintf(d, sizeof d, "%s/%s/%s.disk", dir, clone_fails ? "missing" : ".", name);
    snprintf(a, sizeof a, "%s/%s.aux", dir, name);
    uint64_t t = mach_absolute_time();
    int rc = hv_vm_autosave(snap, d, a, 5000, hv_vm_suspend_ticket(), hold_ticket, why, sizeof why);
    *took = mach_absolute_time() - t;
    printf("  auto-save %s: %d%s%s\n", name, rc, why[0] ? ", " : "", why);
    return rc;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_NO_GPU", "1", 1);
    setenv("HV_PVG_NO_METAL", "1", 1);
    setenv("HV_NO_SOUND", "1", 1);
    setenv("HV_NO_CAMERA", "1", 1);
    setenv("HV_RAM_NO_LEDGER", "1", 1); /* plain memory: a test binary may not leave the footprint */
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(dir, sizeof dir, "%s/henge-vm-pause.XXXXXX", tmp);
    if (!mkdtemp(dir)) return printf("cannot make a folder\n"), 1;
    char rom[1200], disk[1200], aux[1200], ram[1200];
    static const uint32_t sleep_loop[] = {0xd503207f /* wfi */, 0x17ffffff /* b .-4 */};
    snprintf(ram, sizeof ram, "%s/ram", dir);
    mkdir(ram, 0700);
    setenv("HV_RAM_SAVE_AHEAD", ram, 1);
    if (make_file("rom", sleep_loop, sizeof sleep_loop, 0, rom, sizeof rom) || make_file("disk", NULL, 0, 4 << 20, disk, sizeof disk) ||
        make_file("aux", NULL, 0, 1 << 20, aux, sizeof aux))
        return printf("cannot make the files\n"), 1;
    HvVmConfig cfg = {.rom = rom, .disk = disk, .aux = aux, .ram_mb = 64, .ncpus = 1, .no_net = 1, .no_sound = 1,
                      .jit_bytes = 64u << 20};
    char err[256] = "";
    if (hv_vm_create(&cfg, err, sizeof err) != 0) return printf("cannot make the Mac: %s\n", err), 1;
    hv_vm_start(1, on_exit_cb);
    usleep(20000);
    CHECK(hv_vm_memory_copyable(), "Quick Suspend's memory files are in use (an auto-save needs them)");
    woke();

    /* A pause: odd while it lasts, then one resume takes it off the clock and tells macOS. */
    unsigned s0 = hv_vm_stops();
    uint64_t b0 = bias();
    CHECK(s0 % 2 == 0, "running: the stop count %u is even", s0);
    int p = hv_vm_pause(2000);
    uint64_t t1 = mach_absolute_time();
    CHECK(p == 0, "paused: %d", p);
    CHECK(hv_vm_paused() && hv_vm_stops() == s0 + 1, "paused: the count %u is one up", hv_vm_stops());
    usleep(20000);
    uint64_t t2 = mach_absolute_time();
    CHECK(bias() == b0 && !woke(), "paused: the clock and macOS not told yet");
    hv_vm_resume();
    CHECK(!hv_vm_paused() && hv_vm_stops() == s0 + 2, "resumed: the count %u is two up", hv_vm_stops());
    CHECK(woke(), "resumed: macOS told to reread the time");
    CHECK(bias() - b0 >= t2 - t1, "resumed: %.2f ms off the guest's clock, at least the %.2f ms it was paused",
          ms(bias() - b0), ms(t2 - t1));
    hv_vm_resume();
    CHECK(hv_vm_stops() == s0 + 2 && !woke(), "a second resume: nothing");

    /* An auto-save: paused and resumed in the one call, kept or not. */
    uint64_t took;
    s0 = hv_vm_stops(), b0 = bias();
    int rc = autosave("kept", hv_vm_autosave_hold_ticket(), 0, &took);
    char path[1200];
    snprintf(path, sizeof path, "%s/kept.snap", dir);
    CHECK(rc == 0 && exists(path), "an auto-save kept");
    CHECK(!hv_vm_paused() && hv_vm_stops() == s0 + 2, "after an auto-save: running, the count %u two up", hv_vm_stops());
    CHECK(woke(), "after an auto-save: macOS told to reread the time");
    CHECK(bias() > b0 && bias() - b0 <= took, "after an auto-save: %.2f ms off the guest's clock, within the %.2f ms it took",
          ms(bias() - b0), ms(took));

    s0 = hv_vm_stops(), b0 = bias();
    rc = autosave("failed", hv_vm_autosave_hold_ticket(), 1, &took);
    snprintf(path, sizeof path, "%s/failed.snap", dir);
    CHECK(rc == -1 && !exists(path), "an auto-save whose disk copy fails: failed, nothing kept");
    CHECK(!hv_vm_paused() && hv_vm_stops() == s0 + 2, "after a failed auto-save: running, the count %u two up", hv_vm_stops());
    CHECK(woke(), "after a failed auto-save: macOS told to reread the time");
    CHECK(bias() > b0 && bias() - b0 <= took, "after a failed auto-save: %.2f ms off the guest's clock, within %.2f ms",
          ms(bias() - b0), ms(took));

    /* Held: a pause came in for it. It stays paused, as that pause would leave it, until the resume. */
    s0 = hv_vm_stops(), b0 = bias();
    uint64_t ticket = hv_vm_autosave_hold_ticket();
    hv_vm_autosave_hold();
    rc = autosave("held", ticket, 0, &took);
    t1 = mach_absolute_time();
    CHECK(rc == 0, "a held auto-save kept");
    CHECK(hv_vm_paused() && hv_vm_stops() == s0 + 1, "held: still paused, the count %u one up (odd)", hv_vm_stops());
    CHECK(bias() == b0 && !woke(), "held: the clock and macOS not told yet");
    usleep(20000);
    t2 = mach_absolute_time();
    hv_vm_resume();
    CHECK(!hv_vm_paused() && hv_vm_stops() == s0 + 2, "held, then resumed: the count %u two up", hv_vm_stops());
    CHECK(woke(), "held, then resumed: macOS told once");
    CHECK(bias() - b0 >= t2 - t1, "held, then resumed: %.2f ms off the clock, at least the %.2f ms held", ms(bias() - b0),
          ms(t2 - t1));

    /* Of a Mac already paused: refused, and the pause is left as it was. */
    CHECK(hv_vm_pause(2000) == 0, "paused again");
    s0 = hv_vm_stops(), b0 = bias();
    rc = autosave("paused", hv_vm_autosave_hold_ticket(), 0, &took);
    CHECK(rc == -2 && hv_vm_paused() && hv_vm_stops() == s0 && bias() == b0 && !woke(), "an auto-save of a paused Mac: refused, "
          "still paused, the count %u as it was", hv_vm_stops());
    hv_vm_resume();
    CHECK(!hv_vm_paused() && hv_vm_stops() == s0 + 1 && woke(), "and the pause ends as any does");

    hv_vm_stop();
    pthread_mutex_lock(&exit_mu);
    struct timespec ts = {10, 0};
    while (!exited && pthread_cond_timedwait_relative_np(&exit_cv, &exit_mu, &ts) == 0) {
    }
    int gone = exited;
    pthread_mutex_unlock(&exit_mu);
    CHECK(gone, "the Mac stopped");
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    system(cmd);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
