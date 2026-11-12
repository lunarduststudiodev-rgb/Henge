#!/usr/bin/env python3
"""Which engine functions compute in floating point, checked on the built objects (Makefile: ios-lib, check-fp).

While a CPU runs, the guest's FPCR and FPSR are live in the host registers (host/enter.S): host C on a CPU
thread that computes in floating point rounds and flushes the guest's way and leaves its flags in the
guest's FPSR, unless it runs between hv_host_fp_begin and hv_host_fp_end (host/host_fp.h). Which thread a
function runs on is not in the object, so every function with floating-point arithmetic (or a call to libm)
is listed below with where it runs. A function not listed fails the build: look at which threads reach it,
bracket it if a CPU thread does, and add it here with the reason.

Usage: check_cpu_thread_fp.py <objects...>
"""
import os
import re
import subprocess
import sys

# Arithmetic, conversions and compares, scalar or vector (objdump's Apple syntax puts the arrangement after a dot:
# fmla.4s, scvtf.2d); moves, abs, neg and selects copy bits and touch neither FPCR nor FPSR.
FP_OP = re.compile(r"^(f(?!mov$|abs$|neg$|csel$|mov\.|abs\.|neg\.)[a-z0-9]+|scvtf|ucvtf|bfcvt\w*)(\.\w+)?$")
LIBM = re.compile(r"^_(pow|exp2?|expm1|log(2|10|1p)?|sqrt|cbrt|sin|cos|tan|asin|acos|atan2?|sinh|cosh|tanh|floor|ceil|"
                  r"round|l?lround|trunc|fmod|remainder|hypot|ldexp|frexp|modf|nearbyint|l?l?rint)[fl]?$|^_(strtod|strtof|strtold|atof)$")

BRACKETED = "a CPU thread, between hv_host_fp_begin and _end"
SAVE = "save and restore threads (a Suspend, a snapshot, the RAM file's writer)"
METAL = "Metal's worker and presenter threads; the CPU threads reach pvg_metal only through pvg_lite's bracketed doorbells and poll"
UI = "the app's threads (UI, control), through host/vm.c's hv_vm_* calls"
OWN = "its own thread"
LINK = "the share link's thread (host/share.c: the helper's bench lines), or the app's (hv_bench_give_up)"

ALLOWED = {
    "dispatch.o": {
        "_run_loop": "print_stats, inlined: between hv_fp_leave and hv_fp_enter (HV_STATS)",
        "_print_stats": "CPU 0's thread (run_loop, HV_STATS), between its own hv_fp_leave and hv_fp_enter: when not inlined",
    },
    "pvg_lite.o": {
        "_display_set_page": BRACKETED + " (gfx_write, hv_pvg_lite_poll)",
        "_display_command": BRACKETED + " (gfx_write, hv_pvg_lite_poll: channel_command), and Metal's worker (hv_pvg_lite_command)",
        "_hv_pvg_lite_start_mode": UI + " (hv_vm_start_mode)",
    },
    "vm.o": {
        "_board_work": BRACKETED + " (board_poll)",
        "_park_send": BRACKETED + " (board_poll: park_tick)",
        "_hv_vm_boot_display": UI, "_hv_vm_touch": UI, "_hv_vm_touch_button": UI, "_hv_vm_touchscreen_frame": UI,
        "_pause_l": UI, "_restore": UI, "_resume_watch": OWN + " (the resume watcher)", "_save": UI,
    },
    "power_source.o": {"_hv_power_source_set": UI},
    "usb_audio.o": {
        "_hv_usb_audio_scalar_to_db": UI + " (hv_vm_speaker_host_volume)",
        # Also called from host/audio_usb.c volume(), on the CPU that set the volume, unbracketed, with the
        # app's listener after it: a known gap, left to the audio work (outside the engine's scope).
        "_hv_usb_audio_db_to_scalar": UI + " (hv_vm_speaker_volume); ALSO a CPU thread via audio_usb.c volume(): known, unbracketed",
        "_hv_usb_audio_pull": "the audio unit's render thread",
    },
    "virtio_blk.o": {"_worker": OWN + " (the disk worker)"},
    "vmapple.o": {"_prefault_thread": OWN + " (the RAM file's prefault)"},
    "codestore.o": {"_hv_code_store_save": SAVE, "_ms_since": SAVE + " (hv_code_store_save's timing)"},
    "snapshot.o": {"_hv_snap_get_ram": SAVE, "_put_ram": SAVE},
    "vmapple_snap.o": {"_cap_room": SAVE, "_hv_vmapple_snap_capture": SAVE, "_hv_vmapple_snap_save": SAVE, "_pvg_stream_room": SAVE},
    "ramfile.o": {f: SAVE for f in ("_hv_ramfile_adopt", "_hv_ramfile_catch_up", "_hv_ramfile_commit", "_hv_ramfile_finish",
                                     "_hv_ramfile_set_policy", "_hv_ramfile_stats", "_hv_ramfile_sync", "_hv_ramfile_take",
                                     "_hv_ramfile_verify", "_hv_ramfile_writer_pass")},
    "pvg_metal.o": {"*": METAL},
    "audio_mac.o": {"_mic_apply": "the audio threads", "_start": UI + " (the audio unit's start)"},
    "input_fence.o": {"_ended_l": UI + " (input from the app)"},
    "net_slirp.o": {"_hv_net_report": UI, "_track": "the network thread"},
    "prof_ios.o": {"_sampler": OWN + " (the profiler)"},
    "share.o": {f: UI + " (the share link)" for f in ("_hv_share_focus_frame", "_hv_share_tap", "_hv_share_touch_hit",
                                                     "_hv_share_touch_scroll", "_serve_henge",
                                                     "_hv_share_volume_level", "_hv_share_set_battery")},
    "stall_detect.o": {"_hv_stall_write": OWN + " (the stall monitor)"},
    "stallmon.o": {"_monitor": OWN + " (the stall monitor)", "_record": OWN + " (the stall monitor)",
                   "_hv_stall_cpu_time": LINK + " (bench.c snap)"},
    "bench.o": {"_finish": LINK, "_heard": LINK, "_snap": LINK},
    "bench_json.o": {f: LINK + " (a step's line)" for f in ("_hv_bench_step_json", "_metric", "_pace")},
    "frame_pace.o": {"_hv_frame_flow_summary": LINK + " (bench_json.c)", "_hv_pace_summary": LINK + " (bench_json.c)"},
}


def scan(path):
    """{function: [what in it is floating point]}"""
    names = {}
    for line in subprocess.run(["nm", "-n", path], check=True, capture_output=True, text=True).stdout.splitlines():
        f = line.split(maxsplit=2)
        if len(f) == 3 and f[1] in "tT" and not f[2].startswith("ltmp"):
            names.setdefault(int(f[0], 16), f[2])
    out = subprocess.run(["xcrun", "objdump", "-d", "-r", "--no-show-raw-insn", path],
                         check=True, capture_output=True, text=True).stdout
    found, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
        if m:
            cur = m.group(2)
            if cur.startswith("ltmp"):  # the function at a section's start: its real name
                cur = names.get(int(m.group(1), 16), cur)
            continue
        if cur is None:
            continue
        m = re.match(r"^\s+[0-9a-f]+:\s+ARM64_RELOC_BRANCH26\s+(\S+)", line)
        if m:
            if LIBM.match(m.group(1)):
                found.setdefault(cur, set()).add(m.group(1))
            continue
        m = re.match(r"^\s+[0-9a-f]+:\s+(\S+)", line)
        if m and FP_OP.match(m.group(1)):
            found.setdefault(cur, set()).add(m.group(1))
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    bad, seen = [], 0
    for path in sys.argv[1:]:
        obj = os.path.basename(path)
        allowed = ALLOWED.get(obj, {})
        for fn, what in sorted(scan(path).items()):
            seen += 1
            if "*" not in allowed and fn not in allowed:
                bad.append(f"  {obj} {fn}: {' '.join(sorted(what))[:80]}")
    if bad:
        print("check_cpu_thread_fp: floating point in functions not listed in tools/check_cpu_thread_fp.py;\n"
              "  on a CPU thread they need hv_host_fp_begin/_end (host/host_fp.h):", file=sys.stderr)
        print("\n".join(bad), file=sys.stderr)
        return 1
    print(f"check_cpu_thread_fp: {seen} functions with floating point, each listed with its thread")
    return 0


if __name__ == "__main__":
    sys.exit(main())
