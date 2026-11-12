# Light build: one clang call per file, no parallel jobs needed.
CC = clang
RESTORE_A = build/mac-restore/libhengerestore.a
# ROSETTA_4K_CACHE=1: the experimental Rosetta engine's 4 KB software TLB pages (core/cpu.h). Off: the 16 KB one.
ROSETTA_4K_CACHE ?= 0
CACHE_CFLAGS = -DHENGE_ROSETTA_4K_CACHE=$(ROSETTA_4K_CACHE)
# The engine's chip: the A12's instructions (Armv8.3: LSE atomics, LDAPR), scheduled for the A14 / M1. The A12 is
# the oldest chip the app installs on because ios/Henge/Info.plist requires it (UIRequiredDeviceCapabilities
# iphone-ipad-minimum-performance-a12; iPadOS 17 alone would let A10 iPads in, which have neither). The app calls
# into this library at launch on an A12 or A13 too, before it says the chip is too old for a Mac
# (ios/Henge/Hardware.swift chipTooOld), so nothing in it may be newer than the A12 (tools/check_engine_isa.sh);
# the A14 is the oldest that runs a Mac, so it sets the tuning. The Mac build and its tests use the same, so they
# run the code the iPad runs.
ENGINE_CPU = -mcpu=apple-a12 -mtune=apple-a14
# SAN_CFLAGS: a sanitizer build's flags (-fsanitize=...), added to the rest, so a sanitizer build compiles the engine the
# app ships (its chip and optimisation; with TEST_CONFIG=-DHV_TEST_AS_SHIPPED, its logging and counting too) with only
# the sanitizer added. A wrapper script is one, and the way to run one by hand is:
#   UBSAN_OPTIONS=print_stacktrace=0 make -j4 B=build-asan TEST_CONFIG=-DHV_TEST_AS_SHIPPED \
#       SAN_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" test-cpu      (or one: build-asan/test_snapshot,
#   then run it with HV_NO_GPU=1 HV_PVG_NO_METAL=1, as test-cpu does). No stacks for UBSan's reports: symbolizing one
#   holds the CPU thread ~80 ms, and test_snapshot's clock checks fail on that.
# Not CFLAGS=...: that replaces all of the flags here, so the build is not the shipped engine (no chip, cache or test
# configuration) and FLAGS_STAMP does not see the change. mmu.o leaves the sanitizer out either way (MMU_CFLAGS); a
# checkout from before that rule (79f6f4e) fails test_snapshot's "the program is not deterministic" under ASan.
SAN_CFLAGS =
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices $(CACHE_CFLAGS) $(ENGINE_CPU) $(TEST_CONFIG) $(SAN_CFLAGS)
B = build

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/rosetta.o $(B)/machine.o $(B)/ramfile.o $(B)/saveguard.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
DEV = $(B)/pvg_lite.o $(B)/pvg_metal.o $(B)/metallib.o $(B)/gicv3.o $(B)/timer.o $(B)/pl011.o $(B)/pl031.o $(B)/cfg.o $(B)/bdif.o $(B)/pcie.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_fs.o $(B)/virtio_net.o $(B)/virtio_input.o $(B)/virtio_snd.o $(B)/virtio_usb.o $(B)/virtio_balloon.o $(B)/xhci.o $(B)/usb_storage.o $(B)/usb_audio.o $(B)/usb_video.o $(B)/vmapple.o $(B)/aes.o $(B)/pl061.o $(B)/power_source.o $(B)/frame_pace.o
ENGINE = $(CORE) $(HOST)
# Suspend (core/snapshot.h): apart from the engine, since it needs libcompression.
SNAP = $(B)/snapshot.o $(B)/codestore.o $(B)/gicv3_snap.o $(B)/vmapple_snap.o
METAL_LIBS = -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers

TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
        $(B)/test_mmu $(B)/test_exc $(B)/test_tso $(B)/test_rosetta $(B)/test_rosetta_4k $(B)/test_devices $(B)/test_virtio_fs $(B)/test_usb_storage $(B)/test_snapshot \
        $(B)/test_device_snap $(B)/test_cpu_reset_mem $(B)/test_off_giveback $(B)/test_off_giveback_race $(B)/test_pvg_snap $(B)/test_usb_snap $(B)/test_usb_touch $(B)/test_power_source $(B)/test_gpu_switch \
        $(B)/test_virtio_net $(B)/test_net_link $(B)/test_net_forward $(B)/test_mdns $(B)/test_mdns_fuzz $(B)/test_agent_ports $(B)/test_net_dns $(B)/test_usb_audio \
        $(B)/test_usb_video $(B)/test_virtio_snd_mic $(B)/test_fuzz $(B)/test_code_links $(B)/test_code_evict $(B)/test_evict_lists $(B)/test_retrans_counters $(B)/test_ghost $(B)/test_code_evict_mt $(B)/test_evict_reclaim $(B)/test_evict_concurrent_mt $(B)/test_code_writes $(B)/test_mem_giveback $(B)/test_ijc_giveback_race $(B)/test_code_store \
        $(B)/test_smc_race $(B)/test_smc_store $(B)/test_classify_host $(B)/test_shutdown $(B)/test_rosetta_snap $(B)/test_metallib $(B)/test_ramfile $(B)/test_save_guard $(B)/test_save_guard_read $(B)/test_snap_ramfile $(B)/test_autosave_ram $(B)/test_autosave $(B)/test_vm_pause $(B)/test_disk_guard \
        $(B)/test_protect_remark $(B)/test_protect_stash $(B)/test_protect_tlbi $(B)/test_switch_flush \
        $(B)/test_pvg_fuzz $(B)/test_pvg_queue $(B)/test_pvg_mapper_state $(B)/test_pvg_present $(B)/test_pvg_present_nogpu $(B)/test_pvg_present_nometal $(B)/test_pvg_present_app $(B)/test_pvg_video $(B)/test_pvg_validate \
        $(B)/test_keyboard_layout $(B)/test_keyboard_intent $(B)/test_save_ahead $(B)/test_typing_queue $(B)/test_focus_said $(B)/test_share_field $(B)/test_share_versions $(B)/test_share_link_auth $(B)/test_input_fence $(B)/test_stallmon $(B)/test_stallmon_live $(B)/test_stallmon_exit $(B)/test_stallmon_starved $(B)/test_stallmon_kinds $(B)/test_stallmon_wake $(B)/test_stallmon_quiet $(B)/test_trim \
        $(B)/test_logq $(B)/test_stats_percpu $(B)/test_stats_race $(B)/test_stop_hook $(B)/test_stop_hook_tsan $(B)/test_break $(B)/test_host_fp $(B)/test_sig_say $(B)/test_stats_print $(B)/test_wfe_yield $(B)/test_freeze_report $(B)/test_check_fp $(B)/test_pause_say $(B)/test_bench_json $(B)/test_bench_report $(B)/test_check_stats $(B)/test_pvg_standins $(B)/test_run_tests $(B)/test_make_no_gpu $(B)/test_san_flags $(B)/test_check_mmu $(B)/test_check_signal_safe $(B)/test_flags_stamp $(B)/test_wrapper_dirs $(B)/test_ios_lib_deps

all: $(TESTS) $(B)/hengevm $(B)/hengevm-ref $(B)/hvsparse

# Mac-only test tool: the same machine with the CPU on Hypervisor.framework, for comparing runs.
$(B)/hengevm-ref: host/hvf_ref.c host/hvf_ref.entitlements $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) host/hvf_ref.c $(ENGINE) $(DEV) -framework Hypervisor $(METAL_LIBS) -o $@
	codesign -s - -f --entitlements host/hvf_ref.entitlements $@

# Mac-only graphics device (ParavirtualizedGraphics.framework, as QEMU uses it).
# Built for an older deployment target: the framework's memory callbacks are
# marked obsolete in macOS 27 without a documented replacement.
$(B)/pvg_metal.o: devices/pvg_metal.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations -c $< -o $@
# The same for the tests only: with HV_PVG_ENCODE_ONLY and the hooks that make objects and GPU failures.
$(B)/pvg_metal_test.o: devices/pvg_metal.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -DHV_PVG_TEST_HOOKS -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations -c $< -o $@
DEV_TEST = $(filter-out $(B)/pvg_metal.o,$(DEV)) $(B)/pvg_metal_test.o
# The host's camera into the USB camera (also in the iOS library below).
$(B)/camera_apple.o: host/camera_apple.m host/camera_apple.h devices/usb_video.h devices/usb_storage.h | $(B)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@
$(B)/pvg_mac.o: devices/pvg_mac.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) -fobjc-arc -O2 -g -Wall -mmacosx-version-min=15.2 -Icore -Ihost -Idevices $(CACHE_CFLAGS) -c $< -o $@

# User-mode NAT for the NIC: libslirp built from third_party/libslirp, with Henge's patch for TCP window
# scaling (third_party/libslirp/HENGE.md), on Homebrew's glib (brew install glib). The iOS library below
# builds the same sources.
SLIRP_DIR = third_party/libslirp/src
SLIRP_NAMES = $(basename $(notdir $(wildcard $(SLIRP_DIR)/*.c)))
SLIRP_DEFS = -DG_LOG_DOMAIN='"Slirp"' -DBUILDING_LIBSLIRP
SLIRP_CFLAGS = -I$(SLIRP_DIR)
SLIRP_LIBS = $(B)/libslirp.a $(shell pkg-config --libs glib-2.0)
$(B)/slirp/%.o: $(SLIRP_DIR)/%.c $(SLIRP_DIR)/*.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -g -Wall $(SLIRP_DEFS) $(shell pkg-config --cflags glib-2.0) -c $< -o $@
$(B)/libslirp.a: $(patsubst %,$(B)/slirp/%.o,$(SLIRP_NAMES))
	rm -f $@
	ar rcs $@ $^
$(B)/net_slirp.o: host/net_slirp.c host/*.h devices/*.h core/*.h $(SLIRP_DIR)/libslirp.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@

RB = $(B)/restore
RESTORE_PKGS = libplist-2.0 libusbmuxd-2.0 libimobiledevice-1.0 libimobiledevice-glue-1.0 libtatsu-1.0 libzip libcurl
RESTORE_CFLAGS = -O2 -w -DHAVE_CONFIG_H -Ithird_party/restore_config -Ithird_party -Ithird_party/libusb-1.0 \
                 -Ithird_party/libirecovery/include $(shell pkg-config --cflags $(RESTORE_PKGS) 2>/dev/null)
RESTORE_LIBS = $(shell pkg-config --libs $(RESTORE_PKGS) 2>/dev/null) -lz
IRECV_SRC = third_party/libirecovery/src/libirecovery.c
MUXD_SRC = $(wildcard third_party/usbmuxd/src/*.c)
IDR_SRC = $(wildcard third_party/idevicerestore/src/*.c)
RESTORE_OBJ = $(patsubst third_party/%.c,$(RB)/%.o,$(IRECV_SRC) $(MUXD_SRC) $(IDR_SRC))

# The helper that runs inside macOS on a virtual Mac (share.c serves it to the guest).
# AGENT_SIGN: a signing identity that stays the same from build to build. macOS remembers the helper's
# Accessibility permission by its signer; signed ad hoc (-), each new build has to be allowed again.
# tools/ios/make-tools-identity.sh makes a self-signed one in a keychain file of its own (AGENT_KEYCHAIN).
AGENT_SIGN ?= -
AGENT_KEYCHAIN ?=
export AGENT_KEYCHAIN
AGENT_KC_FLAG = $(if $(AGENT_KEYCHAIN),--keychain "$(AGENT_KEYCHAIN)")
# Its Info.plist is in the binary: macOS shows its NSAppleEventsUsageDescription when it first asks to shut the Mac down.
# Built for Intel too: the benchmark's Rosetta step runs its Intel half (henge-agent --bench-work N x86_64).
$(B)/henge-agent: tools/henge-agent.m tools/agent-ports.c tools/agent-ports.h tools/focus-said.h host/share.h tools/henge-agent-Info.plist
	@mkdir -p $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx clang -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -Wall -Wextra -O2 -framework AppKit -framework ApplicationServices -framework AudioToolbox -framework Carbon -framework CoreAudio -framework IOKit -lz -Wl,-sectcreate,__TEXT,__info_plist,tools/henge-agent-Info.plist $< tools/agent-ports.c -o $@
	codesign -f -s "$(AGENT_SIGN)" $(AGENT_KC_FLAG) --identifier com.henge.agent $@

# The benchmark's video (host/bench.h; the helper plays it on a loop): 8 s of ffmpeg's moving test pattern, 1280x720 at
# 30 fps, H.264 at 2 Mbps, as a 720p stream would be. Committed; made again only by hand, as a new one is a new
# benchmark (runs made with different videos don't compare).
bench-video:
	ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=8" -c:v libx264 -profile:v high \
	    -preset slow -b:v 2M -maxrate 2500k -bufsize 4M -g 60 -pix_fmt yuv420p -movflags +faststart -an -map_metadata -1 \
	    -fflags +bitexact -flags:v +bitexact -y ios/Henge/Resources/henge-bench.mp4
.PHONY: bench-video

# Compressed in the app: tools that sign an IPA would try to sign a bare Mach-O too.
$(B)/henge-agent.gz: $(B)/henge-agent
	gzip -9 -n -c $< > $@

# The Henge Tools disk: "Install Henge Tools.app", around the helper above. Compressed for the same reason;
# the iPad app unpacks it (ios/Henge/Native/henge-unpack.c) and attaches it read-only until the helper is in.
$(B)/henge-tools.img.gz: tools/henge-tools-installer.m tools/build-henge-tools.sh tools/henge-tools.DS_Store $(B)/henge-agent ios/Henge/AppIcon.icon/icon.json
	tools/build-henge-tools.sh "$(AGENT_SIGN)" $(B)

$(B)/share.o: host/share.c host/share.h
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/hengevm: host/main.c $(ENGINE) $(DEV) $(SNAP) $(B)/pvg_mac.o $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/libslirp.a $(B)/net_dns.o $(B)/share.o $(B)/audio_mac.o $(B)/audio_usb.o $(B)/camera_apple.o $(B)/vm.o $(B)/usb_host.o $(RESTORE_A)
	$(CC) $(CFLAGS) host/main.c $(ENGINE) $(DEV) $(SNAP) -lcompression $(B)/pvg_mac.o $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/net_dns.o -lresolv $(B)/share.o $(B)/audio_mac.o $(B)/audio_usb.o $(B)/camera_apple.o $(B)/vm.o $(B)/usb_host.o $(RESTORE_A) -lz -framework Foundation -framework CFNetwork -framework Security -framework SystemConfiguration -framework IOKit $(SLIRP_LIBS) -framework AudioToolbox -framework ParavirtualizedGraphics -framework Metal -framework IOSurface \
	    -framework Foundation -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -framework CoreFoundation \
	    -framework AppKit -framework QuartzCore -framework Network -framework AVFoundation -framework CoreMedia -framework CoreVideo -o $@

# mmu.c: no SIMD (hv_tlb_refill runs with the guest's SIMD registers live), and no sanitizer: its runtime is
# outside this file and uses SIMD (tools/check_mmu_calls.py checks both; make check-mmu-calls, ios-lib and make test run it).
MMU_CFLAGS = $(filter-out -fsanitize=%,$(CFLAGS)) -mgeneral-regs-only
$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(MMU_CFLAGS) -c $< -o $@
$(B)/%.o: core/%.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) -o $@
$(B)/test_switch_flush: tests/test_switch_flush.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_switch_flush.c $(ENGINE) -o $@

$(B)/test_shutdown: tests/test_shutdown.c $(ENGINE)
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_off_giveback: tests/test_off_giveback.c $(ENGINE)
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_off_giveback_race: tests/test_off_giveback_race.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_off_giveback_race.c $(ENGINE) -lpthread -o $@
$(B)/test_ijc_giveback_race: tests/test_ijc_giveback_race.c $(ENGINE)
	$(CC) $(CFLAGS) $^ -o $@

$(B)/test_rosetta_snap: tests/test_rosetta_snap.c $(ENGINE) $(B)/snapshot.o $(B)/codestore.o
	$(CC) $(CFLAGS) $^ -lcompression -o $@

$(B)/test_rosetta_4k: tests/test_rosetta_4k.c tests/rosetta_4k_programs.S $(ENGINE) $(B)/snapshot.o $(B)/codestore.o
	$(CC) $(CFLAGS) $^ -lcompression -o $@
$(B)/test_code_links: tests/test_code_links.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_code_links.c $(ENGINE) -o $@
$(B)/test_code_evict: tests/test_code_evict.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_code_evict.c $(ENGINE) -o $@
$(B)/test_code_writes: tests/test_code_writes.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_code_writes.c $(ENGINE) -o $@
$(B)/test_evict_lists: tests/test_evict_lists.c tests/evict_lists.h $(ENGINE)
	$(CC) $(CFLAGS) tests/test_evict_lists.c $(ENGINE) -lpthread -o $@
# The tests that check the hash and the pages' lists (tests/evict_lists.h)
$(B)/test_code_evict $(B)/test_code_writes $(B)/test_code_links $(B)/test_code_store: tests/evict_lists.h
# Memory the code cache gives back on a warning or an id run-out, and keeps given back.
$(B)/test_mem_giveback: tests/test_mem_giveback.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mem_giveback.c $(ENGINE) -o $@
# The store built in here with a batch of 4100 bytes (core/codestore.c CS_CHUNK), so that every part of the test's
# small cache is read and written in many batches, as a real-size cache's are.
$(B)/test_code_store: tests/test_code_store.c core/codestore.c $(ENGINE) $(B)/snapshot.o
	$(CC) $(CFLAGS) -DCS_CHUNK=4100 $(filter-out %.h,$^) -lcompression -o $@
# Low-memory warnings: the translation tables given back at a full flush (hv_machine_request_trim).
$(B)/test_trim: tests/test_trim.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_trim.c $(ENGINE) -o $@
$(B)/test_classify_host: tests/test_classify_host.c $(B)/classify.o $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
# The counters with several CPUs at once: per-CPU, nothing lost (core/machine.h HvStats).
# ThreadSanitizer fails the run on any data race it sees (a non-zero exit: 66 by its own default, 134 where it aborts)
$(B)/test_stats_race: tests/test_stats_race.c core/machine.h core/cpu.h | $(B)
	$(CC) $(filter-out -fsanitize=%,$(CFLAGS)) -fsanitize=thread tests/test_stats_race.c -o $@
# The stop moved while CPUs run (core/machine.h hv_stop_publish, hv_stop_hook): never no hook, never the wrong one.
# Built as shipped, where a missing barrier shows as a wrong hook (ThreadSanitizer sees no race among atomics and does
# not model fences), and with ThreadSanitizer, as above, for any plain access.
$(B)/test_stop_hook: tests/test_stop_hook.c core/machine.h core/cpu.h | $(B)
	$(CC) $(CFLAGS) tests/test_stop_hook.c -o $@
$(B)/test_stop_hook_tsan: tests/test_stop_hook.c core/machine.h core/cpu.h | $(B)
	$(CC) $(filter-out -fsanitize=%,$(CFLAGS)) -fsanitize=thread tests/test_stop_hook.c -o $@
# --break (core/machine.c hv_machine_set_break): without it, a hook already at the stop stays; with it, CPU 0 halts there
# and CPU 1 runs on.
$(B)/test_break: tests/test_break.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_break.c $(ENGINE) -lpthread -o $@
$(B)/test_stats_percpu: tests/test_stats_percpu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_stats_percpu.c $(ENGINE) -lpthread -o $@
$(B)/test_pause_say: tests/test_pause_say.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_pause_say.c $(ENGINE) -o $@
$(B)/test_smc_race: tests/test_smc_race.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_smc_race.c $(ENGINE) -lpthread -o $@
$(B)/test_code_evict_mt: tests/test_code_evict_mt.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_code_evict_mt.c $(ENGINE) -lpthread -o $@
$(B)/test_retrans_counters: tests/test_retrans_counters.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_retrans_counters.c $(ENGINE) -lpthread -o $@
$(B)/test_ghost: tests/test_ghost.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_ghost.c $(ENGINE) -o $@
# Zones emptied in the background (core/dispatch.c, the reclaimer): its steps one at a time, and with CPUs running.
$(B)/test_evict_reclaim: tests/test_evict_reclaim.c tests/evict_lists.h core/codestore.c $(ENGINE) $(B)/snapshot.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -lcompression -lpthread -o $@
$(B)/test_evict_concurrent_mt: tests/test_evict_concurrent_mt.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_evict_concurrent_mt.c $(ENGINE) -lpthread -o $@

$(B)/test_stats_print: tests/test_stats_print.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_stats_print.c $(ENGINE) -lpthread -o $@
$(B)/test_wfe_yield: tests/test_wfe_yield.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_wfe_yield.c $(ENGINE) -lpthread -o $@
$(B)/test_smc_store: tests/test_smc_store.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_smc_store.c $(ENGINE) -lpthread -o $@
# The save guard: RAM as it was at one instant while guest CPUs and a device write it (core/saveguard.h).
$(B)/test_save_guard: tests/test_save_guard.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_save_guard.c $(ENGINE) -lpthread -o $@
# A guest that only reads claims nothing while a save is guarded; the same guest writing is claimed.
$(B)/test_save_guard_read: tests/test_save_guard_read.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_save_guard_read.c $(ENGINE) -lpthread -o $@
# Code-page protection races, made deterministic: each includes the file it tests (to step its threads through the
# exact interleaving), so that file's object is left out of the link.
$(B)/test_protect_remark: tests/test_protect_remark.c core/dispatch.c $(filter-out $(B)/dispatch.o,$(ENGINE))
	$(CC) $(CFLAGS) -w $< $(filter-out $(B)/dispatch.o,$(ENGINE)) -lpthread -o $@
$(B)/test_protect_tlbi: tests/test_protect_tlbi.c core/dispatch.c $(filter-out $(B)/dispatch.o,$(ENGINE))
	$(CC) $(CFLAGS) -w $< $(filter-out $(B)/dispatch.o,$(ENGINE)) -lpthread -o $@
$(B)/test_protect_stash: tests/test_protect_stash.c core/mmu.c $(filter-out $(B)/mmu.o,$(ENGINE))
	$(CC) $(CFLAGS) -w $< $(filter-out $(B)/mmu.o,$(ENGINE)) -lpthread -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

# Block-level tests: test_X.c plus X_programs.S, run natively and through the JIT.
$(B)/test_virtio_fs: tests/test_virtio_fs.c devices/virtio_fs.c devices/virtio_fs.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_virtio_fs.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

# The helper's focus and tap lines as the device hears them (share.c, helper 28).
$(B)/test_share_field: tests/test_share_field.c $(B)/share.o tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_share_field.c $(B)/share.o -o $@
$(B)/test_share_versions: tests/test_share_versions.c $(B)/share.o tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_share_versions.c $(B)/share.o -o $@
# Which system link may change the Mac's ports: its key, pinned per Mac (share.c; gates r1 S8 and S3).
$(B)/test_share_link_auth: tests/test_share_link_auth.c $(B)/share.o tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_share_link_auth.c $(B)/share.o -o $@
$(B)/input_fence.o: host/input_fence.c host/input_fence.h host/share.h
	$(CC) $(CFLAGS) -c host/input_fence.c -o $@
$(B)/test_input_fence: tests/test_input_fence.c $(B)/share.o $(B)/input_fence.o tests/scratch_dir.h
	$(CC) $(CFLAGS) -Ihost -Itests tests/test_input_fence.c $(B)/share.o $(B)/input_fence.o -o $@

# The freeze recorder's judgement: what counts as a stall, and the record it writes (host/stall_detect.c).
# henge.log's queue and writer thread (the iOS app's, plain C): order, a full queue, the crash path, exit().
$(B)/test_logq: tests/test_logq.c ios/Henge/Native/henge-logq.c ios/Henge/Native/henge-logq.h | $(B)
	$(CC) $(CFLAGS) -Iios/Henge/Native tests/test_logq.c ios/Henge/Native/henge-logq.c -o $@

# The benchmark's measures (frame gaps, a step's line), and the report that compares runs (tools/bench_report.py).
$(B)/test_bench_json: tests/test_bench_json.c host/bench_json.c host/bench.h devices/frame_pace.c devices/frame_pace.h | $(B)
	$(CC) $(CFLAGS) tests/test_bench_json.c host/bench_json.c devices/frame_pace.c -o $@
$(B)/test_bench_report: tests/test_bench_report.py tools/bench_report.py | $(B)
	printf '#!/bin/sh\nexec python3 "%s"\n' "$(CURDIR)/tests/test_bench_report.py" > $@ && chmod +x $@
$(B)/test_stallmon: tests/test_stallmon.c host/stall_detect.c host/stall_detect.h
	$(CC) $(CFLAGS) tests/test_stallmon.c host/stall_detect.c -o $@
# The whole recorder on real threads (stacks read from other threads, the main queue timed), the machine made up.
$(B)/test_stallmon_live: tests/test_stallmon_live.c host/stallmon.c host/stallmon.h host/stall_detect.c host/stall_detect.h tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_stallmon_live.c host/stallmon.c host/stall_detect.c -o $@

$(B)/test_stallmon_exit: tests/test_stallmon_exit.c host/stallmon.c host/stallmon.h host/stall_detect.c host/stall_detect.h tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_stallmon_exit.c host/stallmon.c host/stall_detect.c -o $@

$(B)/test_stallmon_kinds: tests/test_stallmon_kinds.c host/stallmon.c host/stallmon.h host/stall_detect.c host/stall_detect.h core/machine.h tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_stallmon_kinds.c host/stallmon.c host/stall_detect.c -o $@
$(B)/test_stallmon_wake: tests/test_stallmon_wake.c host/stallmon.c host/stallmon.h host/stall_detect.c host/stall_detect.h core/machine.h
	$(CC) $(CFLAGS) tests/test_stallmon_wake.c host/stall_detect.c -o $@

$(B)/test_stallmon_quiet: tests/test_stallmon_quiet.c host/stallmon.c host/stallmon.h host/stall_detect.c host/stall_detect.h core/machine.h tests/scratch_dir.h | $(B)
	$(CC) $(CFLAGS) -Itests tests/test_stallmon_quiet.c host/stall_detect.c -o $@

# The freeze report names each record with the binary of its own build only (tools/freeze_report.py); a
# script, run on the built hengevm.
$(B)/test_freeze_report: tests/test_freeze_report.py tools/freeze_report.py $(B)/hengevm
	printf '#!/bin/sh\nexec python3 "%s" "%s"\n' "$(CURDIR)/tests/test_freeze_report.py" "$(CURDIR)/$(B)/hengevm" > $@ && chmod +x $@
$(B)/test_check_fp: tests/test_check_fp.py tools/check_cpu_thread_fp.py $(B)/pvg_mac.o $(B)/ramfile.o
	printf '#!/bin/sh\nexec python3 "%s" "%s" "%s"\n' "$(CURDIR)/tests/test_check_fp.py" "$(CURDIR)/$(B)/pvg_mac.o" "$(CURDIR)/$(B)/ramfile.o" > $@ && chmod +x $@
# Engine counters are reached through their per-CPU macros (tools/check_stats_access.sh): run on the sources as
# they are when the tests run.
$(B)/test_check_stats: tools/check_stats_access.sh | $(B)
	printf '#!/bin/sh\nexec "%s" "%s"\n' "$(CURDIR)/tools/check_stats_access.sh" "$(CURDIR)" > $@ && chmod +x $@
# Every Metal hook pvg_lite.o calls has its stand-in in tests/pvg_metal_standin.h, and no test that includes it keeps
# one of its own (tools/check_pvg_standins.sh): a new hook can't break those tests' builds. Its own test plants both.
$(B)/test_pvg_standins: tests/test_pvg_standins.sh tools/check_pvg_standins.sh $(B)/pvg_lite.o
	printf '#!/bin/sh\nexec "%s" "%s" "%s"\n' "$(CURDIR)/tests/test_pvg_standins.sh" "$(CURDIR)" "$(CURDIR)/$(B)/pvg_lite.o" > $@ && chmod +x $@
# The test runner itself (tools/run_tests.sh): a failing test doesn't hide those after it.
$(B)/test_run_tests: tests/test_run_tests.sh tools/run_tests.sh | $(B)
	printf '#!/bin/sh\nexec "%s"\n' "$(CURDIR)/tests/test_run_tests.sh" > $@ && chmod +x $@
$(B)/test_make_no_gpu: tests/test_make_no_gpu.sh | $(B)
	printf '#!/bin/sh\nexec "%s" "%s"\n' "$(CURDIR)/tests/test_make_no_gpu.sh" "$(CURDIR)" > $@ && chmod +x $@
$(B)/test_san_flags: tests/test_san_flags.sh | $(B)
	printf '#!/bin/sh\nexec "%s" "%s"\n' "$(CURDIR)/tests/test_san_flags.sh" "$(CURDIR)" > $@ && chmod +x $@
$(B)/test_ios_lib_deps: tests/test_ios_lib_deps.sh | $(B)
	printf '#!/bin/sh\nexec "%s" "%s"\n' "$(CURDIR)/tests/test_ios_lib_deps.sh" "$(CURDIR)" > $@ && chmod +x $@
$(B)/test_flags_stamp: tests/test_flags_stamp.sh | $(B)
	printf '#!/bin/sh\nexec "%s" "%s"\n' "$(CURDIR)/tests/test_flags_stamp.sh" "$(CURDIR)" > $@ && chmod +x $@
$(B)/test_wrapper_dirs: tests/test_wrapper_dirs.sh | $(B)
	printf '#!/bin/sh\nexec "%s" "%s"\n' "$(CURDIR)/tests/test_wrapper_dirs.sh" "$(CURDIR)" > $@ && chmod +x $@
$(B)/test_check_mmu: tests/test_check_mmu.py tools/check_mmu_calls.py $(B)/mmu.o
	printf '#!/bin/sh\nexec python3 "%s" "%s"\n' "$(CURDIR)/tests/test_check_mmu.py" "$(CURDIR)/$(B)/mmu.o" > $@ && chmod +x $@
$(B)/test_check_signal_safe: tests/test_check_signal_safe.py tools/check_signal_safe.py tools/check_mmu_calls.py $(B)/vm.o
	printf '#!/bin/sh\nexec python3 "%s" "%s"\n' "$(CURDIR)/tests/test_check_signal_safe.py" "$(CURDIR)/$(B)/vm.o" > $@ && chmod +x $@

$(B)/test_stallmon_starved: tests/test_stallmon_starved.c host/stallmon.c host/stallmon.h host/stall_detect.c host/stall_detect.h tests/scratch_dir.h
	$(CC) $(CFLAGS) -Itests tests/test_stallmon_starved.c host/stallmon.c host/stall_detect.c -o $@

$(B)/test_ramfile: tests/test_ramfile.c $(B)/ramfile.o
	$(CC) $(CFLAGS) tests/test_ramfile.c $(B)/ramfile.o -o $@

$(B)/test_disk_guard: tests/test_disk_guard.c $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_disk_guard.c $(ENGINE) $(DEV) $(METAL_LIBS) -lpthread -o $@

$(B)/test_autosave: tests/test_autosave.c $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_autosave.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

$(B)/test_autosave_ram: tests/test_autosave_ram.c $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_autosave_ram.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -lpthread -o $@

$(B)/test_snap_ramfile: tests/test_snap_ramfile.c $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_snap_ramfile.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@

$(B)/test_snapshot: tests/test_snapshot.c tests/snapshot_programs.S tests/snapshot_guest.h $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_snapshot.c tests/snapshot_programs.S $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@

# host/vm.c's stops as the guest sees them: every pause ends in one resume (clock, macOS told, the count even), an
# auto-save's own pause and resume, and one held for a pause that came meanwhile. A real Mac on a WFI ROM; the host's
# sound, camera, network and USB probe are stubbed in the test.
$(B)/test_vm_pause: tests/test_vm_pause.c $(ENGINE) $(DEV) $(SNAP) $(B)/vm.o $(B)/share.o $(B)/net_forward.o
	$(CC) $(CFLAGS) tests/test_vm_pause.c $(ENGINE) $(DEV) $(SNAP) $(B)/vm.o $(B)/share.o $(B)/net_forward.o -lcompression $(METAL_LIBS) -framework CoreFoundation -o $@

$(B)/test_cpu_reset_mem: tests/test_cpu_reset_mem.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_cpu_reset_mem.c $(ENGINE) -o $@

$(B)/test_device_snap: tests/test_device_snap.c $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_device_snap.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@

$(B)/test_virtio_snd_mic: tests/test_virtio_snd_mic.c devices/virtio_snd.c devices/virtio_snd.h $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_virtio_snd_mic.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@

$(B)/test_pvg_snap: tests/test_pvg_snap.c $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_pvg_snap.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@

$(B)/test_usb_snap: tests/test_usb_snap.c $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_usb_snap.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@

# The GPU device's checks (the guest is untrusted: nothing it sends may stop, hang or corrupt the host)
PVG_H = devices/pvg_lite.h devices/gpu_switch.h devices/pvg_validate.h devices/pvg_command.h devices/pvg_video.h devices/pvg_colorprobe.h
$(B)/test_pvg_fuzz: tests/test_pvg_fuzz.c devices/pvg_command.h $(ENGINE) $(DEV_TEST) $(SNAP)
	$(CC) $(CFLAGS) tests/test_pvg_fuzz.c $(ENGINE) $(DEV_TEST) $(SNAP) $(METAL_LIBS) -lcompression -o $@

$(B)/test_pvg_queue: tests/test_pvg_queue.c devices/pvg_lite.c tests/pvg_metal_standin.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) $< -framework CoreFoundation -o $@

$(B)/test_sig_say: tests/test_sig_say.c host/sig_say.h | $(B)
	$(CC) $(CFLAGS) $< -o $@

$(B)/test_host_fp: tests/test_host_fp.c devices/pvg_lite.c tests/pvg_metal_standin.h host/host_fp.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) $< -framework CoreFoundation -o $@

# The mapper's state, with the Metal side stood in for: no GPU, nor Metal linked
$(B)/test_pvg_mapper_state: tests/test_pvg_mapper_state.c devices/pvg_lite.c tests/pvg_metal_standin.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) $< -framework CoreFoundation -o $@

$(B)/test_pvg_present: tests/test_pvg_present.m devices/pvg_metal.m devices/frame_pace.c devices/frame_pace.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) -DHV_PVG_TEST_HOOKS -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations $< devices/frame_pace.c $(METAL_LIBS) -o $@
# the same with no way to reach a GPU (MTLCreateSystemDefaultDevice is nil): its fakes only
$(B)/test_pvg_present_nogpu: tests/test_pvg_present.m devices/pvg_metal.m devices/frame_pace.c devices/frame_pace.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) -DHV_PVG_TEST_HOOKS -DHV_PVG_TEST_NO_GPU -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations $< devices/frame_pace.c $(METAL_LIBS) -o $@
# the same with neither Metal nor QuartzCore linked, for Macs where nothing may link Metal: the tests that check against
# Metal's own descriptor objects are left to test_pvg_present_nogpu (tests/pvg_nometal_stubs.m says which and why).
# The build fails if the binary ever links Metal, any Metal framework (MetalKit, MetalPerformanceShaders...),
# QuartzCore or CoreImage. macOS still loads Metal and QuartzCore into every process that links Foundation (or only
# CoreFoundation, as test_pvg_queue does): what this guarantees is that none of the binary's own code can call them,
# so no GPU device, and no GPU work, can be made from it.
NOMETAL_LIBS = -framework Foundation -framework IOSurface -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers
$(B)/test_pvg_present_nometal: tests/test_pvg_present.m tests/pvg_nometal.h tests/pvg_nometal_stubs.m devices/pvg_metal.m devices/frame_pace.c devices/frame_pace.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) -DHV_PVG_TEST_HOOKS -DHV_PVG_TEST_NO_GPU -DHV_PVG_TEST_NO_METAL -include tests/pvg_nometal.h -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations $< tests/pvg_nometal_stubs.m devices/frame_pace.c $(NOMETAL_LIBS) -o $@
	@if otool -L $@ | grep -E '/(Metal[A-Za-z]*|QuartzCore|CoreImage)\.framework/'; then echo "$@ links Metal, QuartzCore or CoreImage (above): it must not"; rm -f $@; exit 1; fi
# the same without the tests' hooks, as the app's build: HV_PVG_ENCODE_ONLY fails closed there (its fakes only: no GPU)
$(B)/test_pvg_present_app: tests/test_pvg_present.m devices/pvg_metal.m devices/frame_pace.c devices/frame_pace.h $(PVG_H) | $(B)
	$(CC) $(CFLAGS) -DHV_PVG_TEST_NO_GPU -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations $< devices/frame_pace.c $(METAL_LIBS) -o $@

$(B)/test_pvg_video: tests/test_pvg_video.c devices/pvg_video.h devices/pvg_validate.h | $(B)
	$(CC) $(CFLAGS) $< -o $@

$(B)/test_pvg_validate: tests/test_pvg_validate.m devices/pvg_validate.h devices/pvg_command.h | $(B)
	$(CC) $(CFLAGS) -fobjc-arc $< -framework Foundation -framework Metal -o $@

$(B)/test_usb_touch: tests/test_usb_touch.c devices/xhci.c devices/xhci.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_usb_touch.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

$(B)/test_power_source: tests/test_power_source.c $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) tests/test_power_source.c $(ENGINE) $(DEV) $(SNAP) $(METAL_LIBS) -lcompression -o $@
# HV_NO_GPU / HV_PVG_NO_METAL keep every device maker (devices/gpu_switch.h) from asking for a GPU device. The test
# has its own MTLCreateSystemDefaultDevice and PGCreateDeviceWithDescriptor, and every call from Henge's code comes to
# them: the build fails if the binary takes the frameworks'.
$(B)/test_gpu_switch: tests/test_gpu_switch.c $(B)/pvg_mac.o $(ENGINE) $(DEV) $(SNAP)
	$(CC) $(CFLAGS) $< $(B)/pvg_mac.o $(ENGINE) $(DEV) $(SNAP) -lcompression $(METAL_LIBS) -framework ParavirtualizedGraphics -framework AppKit -o $@
	@if nm -u $@ | grep -Eq '_(MTLCreateSystemDefaultDevice|PGCreateDeviceWithDescriptor)$$'; then echo "$@ takes a framework's device maker: it could reach the GPU"; rm -f $@; exit 1; fi

$(B)/test_virtio_net: tests/test_virtio_net.c devices/virtio_net.c devices/virtio_net.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_virtio_net.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

$(B)/test_net_link: tests/test_net_link.c tests/loopback_only.h $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(B)/libslirp.a $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_net_link.c $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(ENGINE) $(DEV) $(METAL_LIBS) $(SLIRP_LIBS) -lresolv -o $@

# The Mac's ports on the device (net_forward.c), through libslirp with a guest played here.
$(B)/test_net_forward: tests/test_net_forward.c tests/loopback_only.h $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(B)/libslirp.a $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_net_forward.c $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(ENGINE) $(DEV) $(METAL_LIBS) $(SLIRP_LIBS) -lresolv -o $@

# The Mac's name and services on the network (mdns.c): a guest played here, Bonjour on this Mac only (LocalOnly).
$(B)/test_mdns: tests/test_mdns.c tests/loopback_only.h $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(B)/libslirp.a $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_mdns.c $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(ENGINE) $(DEV) $(METAL_LIBS) $(SLIRP_LIBS) -lresolv -o $@

# mdns.c's reading of what the guest writes, fed random and mutated messages, with ASan and UBSan (CPU only).
$(B)/test_mdns_fuzz: tests/test_mdns_fuzz.c host/mdns.c host/mdns.h host/net_forward.c host/net_forward.h | $(B)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Ihost tests/test_mdns_fuzz.c host/mdns.c host/net_forward.c -o $@

# The helper's reading of the Mac's socket tables (tools/agent-ports.c), on this Mac's own.
$(B)/test_agent_ports: tests/test_agent_ports.c tools/agent-ports.c tools/agent-ports.h tests/scratch_dir.h | $(B)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Itools tests/test_agent_ports.c tools/agent-ports.c -o $@

# Not a test: how fast a download goes through the guest's network, with a guest played here (tests/bench_net.c).
$(B)/bench_net: tests/bench_net.c tests/loopback_only.h $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(B)/libslirp.a $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/bench_net.c $(B)/net_slirp.o $(B)/net_forward.o $(B)/mdns.o $(B)/share.o $(B)/net_dns.o $(ENGINE) $(DEV) $(METAL_LIBS) $(SLIRP_LIBS) -lresolv -o $@

$(B)/test_net_dns: tests/test_net_dns.c $(B)/net_dns.o
	$(CC) $(CFLAGS) tests/test_net_dns.c $(B)/net_dns.o -lresolv -lpthread -o $@

$(B)/test_usb_audio: tests/test_usb_audio.c devices/xhci.c devices/xhci.h devices/usb_audio.c devices/usb_audio.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_usb_audio.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

$(B)/test_usb_video: tests/test_usb_video.c devices/usb_video.c devices/usb_video.h devices/xhci.c devices/xhci.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_usb_video.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

$(B)/test_usb_storage: tests/test_usb_storage.c devices/usb_storage.c devices/xhci.c $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) tests/test_usb_storage.c $(ENGINE) $(DEV) $(METAL_LIBS) -o $@

$(B)/test_devices: tests/test_devices.c tests/devices_programs.S tests/harness.S tests/harness.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) -Itests tests/test_devices.c tests/devices_programs.S tests/harness.S $(ENGINE) $(DEV) $(METAL_LIBS) -o $@
$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) -o $@

# pvg_metallib.h: macOS 15's GPU libraries relabelled for iPadOS 17's Metal.
$(B)/test_metallib: tests/test_metallib.m devices/pvg_metallib.h | $(B)
	$(CC) $(CFLAGS) -fobjc-arc tests/test_metallib.m -framework Foundation -framework Metal -o $@

# The tests that drive the host's real GPU (Metal). make test never runs them: Metal work from a test froze the
# MacBook twice. make test-gpu does, on a quiet Mac or a GitHub runner (.github/workflows/dev-build.yml).
# Not test_pvg_queue, which stands in for the Metal side itself and links no Metal (otool -L), nor
# test_pvg_present_nogpu and test_pvg_present_app, built so no GPU can be reached (MTLCreateSystemDefaultDevice is nil).
GPU_TESTS = test_pvg_snap test_metallib test_pvg_fuzz test_pvg_present test_pvg_validate
CPU_TESTS = $(filter-out $(addprefix $(B)/,$(GPU_TESTS)),$(TESTS))
# The other tests reach no GPU either: with either switch no device maker asks for a GPU device (devices/gpu_switch.h,
# test_gpu_switch), so a GPU device in one is made without its Metal side. HV_NO_GPU is also what the GPU tests read
# to skip their GPU part (test_pvg_present, test_pvg_validate). The tests that stand in for the Metal side
# (tests/pvg_metal_standin.h) clear both, as theirs reaches no GPU.
NO_GPU = HV_NO_GPU=1 HV_PVG_NO_METAL=1

# The tests as the app runs: every machine with log on (henge-vm.c sets .log = 1) and the statistics counted
# (HV_STATS=1), which take paths the plain tests never do. Its own build directory, so nothing is mixed.
test-shipped:
	$(MAKE) B=build/shipped TEST_CONFIG=-DHV_TEST_AS_SHIPPED test-cpu
# Every test runs, whatever the ones before did, and the end lists each one's result (tools/run_tests.sh).
test-cpu: $(CPU_TESTS)
	@$(NO_GPU) tools/run_tests.sh $^
test-gpu: $(addprefix $(B)/,$(GPU_TESTS))
	@tools/run_tests.sh $^
.PHONY: test-shipped test-cpu test-gpu
# hv_zero_kept checking that every page mincore calls untouched reads zero (core/machine.c), on the tests that zero
# the most CPU and machine state. The check reads those pages, so it brings them in: the tests that count resident
# pages of memory hv_zero_kept zeroes (test_mem_giveback, test_trim, test_off_giveback, test_cpu_reset_mem) can't run
# with it and are left out. test_ijc_giveback_race counts too, but only the ijc tables and stashes, which a give-back
# swaps for zero pages without hv_zero_kept: one that zeroed them through it would fail here only.
ZERO_KEPT_TESTS = test_code_writes test_code_evict test_off_giveback_race test_ijc_giveback_race test_tso test_jit
test-zero-kept:
	$(MAKE) B=build/zero-kept TEST_CONFIG=-DHV_ZERO_KEPT_CHECK $(addprefix build/zero-kept/,$(ZERO_KEPT_TESTS))
	@$(NO_GPU) tools/run_tests.sh $(addprefix build/zero-kept/,$(ZERO_KEPT_TESTS))
.PHONY: test-zero-kept

test: all
	@$(NO_GPU) tools/run_tests.sh $(CPU_TESTS) # every test, whatever the ones before did; 77: skipped (no FEAT_AFP)
	@echo "== $(B)/test_rosetta reference"; $(NO_GPU) $(B)/test_rosetta reference; rc=$$?; [ $$rc -eq 0 ] || [ $$rc -eq 77 ]

$(B):
	mkdir -p $(B)
# Every test is built into $(B): made first, so a test built on its own in a fresh checkout links (not every rule
# above names it)
$(TESTS): | $(B)

clean:
	rm -rf $(B)

.PHONY: all test clean

# ---- iOS: the engine as a static library for the Henge app (ios/) ----
# make ios-lib -> build/ios/libhengevm.a, with libslirp (third_party/libslirp) built into it. The app links
# the arm64 iOS build of glib from Husk's sysroot (scripts there make it); see ios/README.md.
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 $(ENGINE_CPU) -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
# libslirp compiles against the headers of the glib the app links, glib 2.83.0 configured for arm64 iOS as
# Husk builds it (third_party/glib-ios/README.md; ci/build-ipa.sh checks they match Husk's build).
IOS_GLIB = third_party/glib-ios
IOS_GLIB_CFLAGS = -I$(IOS_GLIB)/include/glib-2.0 -I$(IOS_GLIB)/lib/glib-2.0/include
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices $(CACHE_CFLAGS)
IB = build/ios
IOS_SRC = $(wildcard core/*.c) host/prof_ios.c host/stallmon.c host/stall_detect.c host/bench.c host/bench_json.c host/jit_region.c host/disk.c host/vm.c host/net_slirp.c host/net_forward.c host/mdns.c host/net_dns.c host/share.c host/input_fence.c host/audio_mac.c host/audio_usb.c host/hvsparse.c host/usb_host.c \
          $(filter-out devices/pvg_mac.m,$(wildcard devices/*.c))
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_SRC)) $(IB)/host/enter.o $(IB)/devices/pvg_metal.o $(IB)/host/camera_apple.o \
          $(patsubst %,$(IB)/slirp/%.o,$(SLIRP_NAMES))

# Switching ROSETTA_4K_CACHE or ENGINE_CPU rebuilds everything built with it: a stale object must not mix
# 4 KB and 16 KB software pages, or keep another chip's atomics. A missing stamp is phony for this run, so a
# switch within one timestamp tick rebuilds too. (Based on PR #51 by liukaixiang817.)
empty :=
space := $(empty) $(empty)
comma := ,
# Every flag a build directory's objects may differ in, so a change of any rebuilds them, never mixes them
FLAGS_STAMP = .flags-4k$(ROSETTA_4K_CACHE)$(subst $(space),,$(subst $(comma),-,$(subst =,-,$(ENGINE_CPU) $(TEST_CONFIG) $(SAN_CFLAGS))))
CACHE_STAMPS = $(B)/$(FLAGS_STAMP) $(IB)/$(FLAGS_STAMP)
.PHONY: $(filter-out $(wildcard $(CACHE_STAMPS)),$(CACHE_STAMPS))
$(CACHE_STAMPS):
	@mkdir -p $(dir $@)
	@rm -f $(dir $@).rosetta-4k-* $(dir $@).flags-*
	@touch $@
$(filter-out $(B)/enter.o,$(sort $(patsubst %,$(B)/%.o,aes audio_mac audio_usb bdif camera_apple cfg classify codestore disk dispatch exception install \
    frame_pace input_fence jit_region libusb_shim machine mdns memop mmu net_dns net_forward net_slirp operands pcie power_source pvg_lite pvg_mac pvg_metal pvg_metal_test ramfile rosetta saveguard share snapshot \
    sysreg timer translate usb_audio usb_host usb_storage usb_video virtio virtio_balloon virtio_blk virtio_fs virtio_input virtio_net \
    virtio_snd virtio_usb vm vmapple vmapple_snap xhci gicv3 gicv3_snap pl011 pl031 pl061 hvsparse))): $(B)/$(FLAGS_STAMP)
$(IOS_OBJ): $(IB)/$(FLAGS_STAMP)

# libslirp; its headers only for net_slirp.c (they have names like util.h and socket.h).
$(IB)/slirp/%.o: $(SLIRP_DIR)/%.c $(SLIRP_DIR)/*.h $(IOS_GLIB)/lib/glib-2.0/include/glibconfig.h
	@mkdir -p $(dir $@)
	$(IOS_CC) -std=gnu11 -O2 -g -Wall $(SLIRP_DEFS) $(IOS_GLIB_CFLAGS) -c $< -o $@
$(IB)/host/net_slirp.o: IOS_CFLAGS += $(SLIRP_CFLAGS)
$(IB)/host/net_slirp.o: $(SLIRP_DIR)/libslirp.h

$(IB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(filter-out -fsanitize=%,$(IOS_CFLAGS)) -mgeneral-regs-only -c $< -o $@
$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/devices/pvg_metal.o: devices/pvg_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations -c $< -o $@
$(IB)/host/camera_apple.o: host/camera_apple.m host/camera_apple.h devices/usb_video.h devices/usb_storage.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -fobjc-arc -c $< -o $@
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
# What the checks below read besides the objects (check_engine_isa.sh reads the plist; check_signal_safe.py imports
# check_mmu_calls.py): a change to any of them checks the library again.
IOS_LIB_CHECKED_BY = tools/check_engine_isa.sh ios/Henge/Info.plist tools/check_mmu_calls.py tools/check_cpu_thread_fp.py \
                     tools/check_signal_safe.py
$(IB)/libhengevm.a: $(IOS_OBJ) $(IOS_LIB_CHECKED_BY)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)
	tools/check_engine_isa.sh $@ || { rm -f $@; exit 1; }
	tools/check_mmu_calls.py $(IB)/core/mmu.o || { rm -f $@; exit 1; }
	tools/check_cpu_thread_fp.py $(filter %.o,$(IOS_OBJ)) || { rm -f $@; exit 1; }
	tools/check_signal_safe.py $(IB)/host/vm.o || { rm -f $@; exit 1; }
ios-lib: $(IB)/libhengevm.a
.PHONY: ios-lib
# The Mac engine's objects, the same check (tools/check_engine_isa.sh).
check-isa: $(ENGINE) $(DEV) $(SNAP)
	tools/check_engine_isa.sh $^
.PHONY: check-isa
check-mmu-calls: $(B)/mmu.o
	tools/check_mmu_calls.py $<
.PHONY: check-mmu-calls
# The crash handlers (host/vm.c on_ill and on_stop) reach nothing that can take a lock.
check-signal-safe: $(B)/vm.o
	tools/check_signal_safe.py $<
.PHONY: check-signal-safe
# Floating point on a CPU thread (host/host_fp.h): every engine function that computes in it, listed with its thread.
check-fp: ios-lib
.PHONY: check-fp

# The restore stack, built from the same sources and with the same stand-ins as iOS.
# One for every build directory: tools/build_ios_restore.sh mac writes it to build/mac-restore and takes none of
# the engine's flags, so build/shipped's hengevm links the same one.
$(RESTORE_A): tools/build_ios_restore.sh host/libusb_shim.c host/install.c host/curl_shim.m $(wildcard third_party/*/src/*.c) $(wildcard third_party/libirecovery/src/*.c) \
		$(wildcard third_party/*/src/*.h) $(wildcard third_party/*/include/*.h)
	nice -n 10 tools/build_ios_restore.sh mac

# The installer: libirecovery, usbmuxd and idevicerestore (third_party, vendored
# sources) linked against host/libusb_shim.c, so they restore the virtual Mac
# through its USB port the way Virtualization.framework's installer does.
# Mac build: the libraries they need come from Homebrew (libimobiledevice,
# libtatsu, libplist, libzip).
$(RB)/libirecovery/%.o: third_party/libirecovery/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RESTORE_CFLAGS) -DPACKAGE_VERSION='"1.3.1-henge"' -c $< -o $@
$(RB)/usbmuxd/%.o: third_party/usbmuxd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RESTORE_CFLAGS) -DPACKAGE_NAME='"usbmuxd"' -DPACKAGE_VERSION='"1.1.1-henge"' -Dmain=usbmuxd_main -Dlog_level=usbmuxd_log_level -c $< -o $@
$(RB)/idevicerestore/%.o: third_party/idevicerestore/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RESTORE_CFLAGS) -Ithird_party/idevicerestore/src -DIDEVICERESTORE_NOMAIN -DPACKAGE_NAME='"idevicerestore"' -DPACKAGE_VERSION='"1.0.0-henge"' -c $< -o $@
$(B)/libusb_shim.o: host/libusb_shim.c host/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) -Ithird_party -c $< -o $@
$(B)/install.o: host/install.c host/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) $(RESTORE_CFLAGS) -c $< -o $@
restore-objs: $(RESTORE_OBJ)
.PHONY: restore-objs

# Session-owned display routing, independent of JIT, Metal and a connected monitor.
$(B)/test_display_targets: ios/Henge/DisplayTargetSelection.swift tests/test_display_targets.swift | $(B)
	xcrun --sdk macosx swiftc $^ -o $@

# The router's hand-off to the engine, with the engine call recorded instead of made.
$(B)/test_display_router: ios/Henge/DisplayRouter.swift ios/Henge/DisplayTargetSelection.swift tests/test_display_router.swift tests/display_engine_stub.h | $(B)
	xcrun --sdk macosx swiftc -import-objc-header tests/display_engine_stub.h $(filter %.swift,$^) -o $@

# Which external screen shows the Mac before iOS 27, for any order of session and plug events.
$(B)/test_external_screen_plan: ios/Henge/ExternalScreenPlan.swift tests/test_external_screen_plan.swift | $(B)
	xcrun --sdk macosx swiftc $^ -o $@

# The glue: fake screens and a real router, to check what each screen is told and what the engine draws.
$(B)/test_legacy_external_display: ios/Henge/LegacyExternalDisplay.swift ios/Henge/ExternalScreenPlan.swift ios/Henge/MacPresentation.swift ios/Henge/DisplayRouter.swift ios/Henge/DisplayTargetSelection.swift tests/test_legacy_external_display.swift tests/display_engine_stub.h | $(B)
	xcrun --sdk macosx swiftc -import-objc-header tests/display_engine_stub.h $(filter %.swift,$^) -o $@

# Info.plist must declare the external display scene role with ExternalDisplaySceneDelegate, or iOS 17-26 never offer the display.
$(B)/test_scene_manifest: tests/test_scene_manifest.swift | $(B)
	xcrun --sdk macosx swiftc -parse-as-library $^ -o $@

test-display-routing: $(B)/test_display_targets $(B)/test_display_router $(B)/test_external_screen_plan $(B)/test_legacy_external_display $(B)/test_scene_manifest
	$(B)/test_display_targets
	$(B)/test_display_router
	$(B)/test_external_screen_plan
	$(B)/test_legacy_external_display
	$(B)/test_scene_manifest ios/Henge/Info.plist ios/Henge/ExternalDisplaySceneDelegate.swift
.PHONY: test-display-routing

# What the JIT setup tells the user and when it retries by itself, for each kind of network path and helper error.
$(B)/test_jit_advice: ios/Henge/JIT/JITNetworkAdvice.swift tests/test_jit_advice.swift | $(B)
	xcrun --sdk macosx swiftc $^ -o $@

test-jit-advice: $(B)/test_jit_advice
	$(B)/test_jit_advice
.PHONY: test-jit-advice

# The iPad keyboard's rules (when it shows and hides), a pure type checked on the Mac.
$(B)/test_keyboard_intent: ios/Henge/KeyboardIntent.swift ios/Henge/PasswordPrompt.swift tests/test_keyboard_intent.swift | $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx swiftc -O ios/Henge/KeyboardIntent.swift ios/Henge/PasswordPrompt.swift tests/test_keyboard_intent.swift -o $@

test-keyboard-intent: $(B)/test_keyboard_intent
	$(B)/test_keyboard_intent

$(B)/test_save_ahead: ios/Henge/SaveAhead.swift tests/test_save_ahead.swift | $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx swiftc -O ios/Henge/SaveAhead.swift tests/test_save_ahead.swift -o $@
.PHONY: test-keyboard-intent

# Where the iPad keyboard's frames put the row of Mac keys, and when the Mac makes room (docked, floating, a hardware
# keyboard's bar, Stage Manager, iPhone), checked on the Mac.
$(B)/test_keyboard_layout: ios/Henge/KeyboardLayout.swift tests/test_keyboard_layout.swift | $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx swiftc -O ios/Henge/KeyboardLayout.swift tests/test_keyboard_layout.swift -o $@

test-keyboard-layout: $(B)/test_keyboard_layout
	$(B)/test_keyboard_layout
.PHONY: test-keyboard-layout

# Everything Henge types at the Mac's cursor, one piece at a time: order, dropping, Return only after a whole password.
$(B)/test_typing_queue: ios/Henge/TypingQueue.swift tests/test_typing_queue.swift | $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx swiftc -O -parse-as-library ios/Henge/TypingQueue.swift tests/test_typing_queue.swift -o $@

test-typing-queue: $(B)/test_typing_queue
	$(B)/test_typing_queue
.PHONY: test-typing-queue

# What the helper says has the Mac's focus: a password field, or not, held by which process (helper 34).
$(B)/test_focus_said: tests/test_focus_said.c tools/focus-said.h | $(B)
	$(CC) $(CFLAGS) tests/test_focus_said.c -o $@

test-focus-said: $(B)/test_focus_said
	$(B)/test_focus_said
.PHONY: test-focus-said


# Dictation's rules (English only, how phrases join, what the Mac's keyboard can type), checked on the Mac.
$(B)/test_dictation_rules: ios/Henge/DictationRules.swift tests/test_dictation_rules.swift | $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx swiftc -O ios/Henge/DictationRules.swift tests/test_dictation_rules.swift -o $@

test-dictation: $(B)/test_dictation_rules
	$(B)/test_dictation_rules
.PHONY: test-dictation
