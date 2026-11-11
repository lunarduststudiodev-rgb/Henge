# Light build: one clang call per file, no parallel jobs needed.
CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
DEV = $(B)/pvg_lite.o $(B)/pvg_metal.o $(B)/gicv3.o $(B)/timer.o $(B)/pl011.o $(B)/pl031.o $(B)/cfg.o $(B)/bdif.o $(B)/pcie.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_net.o $(B)/virtio_input.o $(B)/virtio_snd.o $(B)/virtio_usb.o $(B)/virtio_balloon.o $(B)/xhci.o $(B)/vmapple.o $(B)/aes.o $(B)/pl061.o $(B)/power_source.o $(B)/metallib.o
ENGINE = $(CORE) $(HOST)
METAL_LIBS = -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers

TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
        $(B)/test_mmu $(B)/test_exc $(B)/test_devices $(B)/test_shutdown

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
$(B)/pvg_mac.o: devices/pvg_mac.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) -fobjc-arc -O2 -g -Wall -mmacosx-version-min=15.2 -Icore -Ihost -Idevices -c $< -o $@

# User-mode NAT for the NIC: Homebrew's libslirp (brew install libslirp).
SLIRP_CFLAGS = $(shell pkg-config --cflags slirp)
SLIRP_LIBS = $(shell pkg-config --libs slirp)
$(B)/net_slirp.o: host/net_slirp.c host/*.h devices/*.h core/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(B)/dns_proxy.o: host/dns_proxy.c host/dns_proxy.h core/machine.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

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
$(B)/henge-agent: tools/henge-agent.m
	@mkdir -p $(B)
	env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=12.0 -fobjc-arc -Wall -Wextra -O2 -framework AppKit $< -o $@
	codesign -f -s - $@

# Compressed in the app: tools that sign an IPA would try to sign a bare Mach-O too.
$(B)/henge-agent.gz: $(B)/henge-agent
	gzip -9 -n -c $< > $@

$(B)/share.o: host/share.c host/share.h
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/hengevm: host/main.c $(ENGINE) $(DEV) $(B)/pvg_mac.o $(B)/net_slirp.o $(B)/dns_proxy.o $(B)/share.o $(B)/audio_mac.o $(B)/vm.o $(B)/snapshot.o $(B)/usb_host.o $(B)/mac-restore/libhengerestore.a
	$(CC) $(CFLAGS) host/main.c $(ENGINE) $(DEV) $(B)/pvg_mac.o $(B)/net_slirp.o $(B)/dns_proxy.o $(B)/share.o $(B)/audio_mac.o $(B)/vm.o $(B)/snapshot.o $(B)/usb_host.o $(B)/mac-restore/libhengerestore.a -lz -lcompression -framework Foundation -framework CFNetwork -framework Security -framework SystemConfiguration -framework IOKit $(SLIRP_LIBS) -framework AudioToolbox -framework ParavirtualizedGraphics -framework Metal -framework IOSurface \
	    -framework Foundation -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -framework CoreFoundation \
	    -framework AppKit -framework QuartzCore -o $@

# mmu.c: no SIMD (hv_tlb_refill runs with the guest's SIMD registers live).
$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@
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

$(B)/test_shutdown: tests/test_shutdown.c $(ENGINE)
	$(CC) $(CFLAGS) $^ -o $@

# Native app lifecycle with real devices and networking; no restore assets.
$(B)/test_dns_lifecycle: tests/test_dns_lifecycle.c $(B)/dns_proxy.o
	$(CC) $(CFLAGS) $^ -o $@

$(B)/test_vm_lifecycle: tests/test_vm_lifecycle.c ios/Henge/Native/henge-vm.c $(ENGINE) $(DEV) $(B)/vm.o $(B)/snapshot.o $(B)/audio_mac.o $(B)/net_slirp.o $(B)/dns_proxy.o $(B)/share.o $(B)/usb_host.o
	$(CC) $(CFLAGS) -Iios/Henge/Native $^ $(SLIRP_LIBS) $(METAL_LIBS) -framework AudioToolbox -lcompression -o $@

test-vm-lifecycle: $(B)/test_vm_lifecycle
	$(B)/test_vm_lifecycle
.PHONY: test-vm-lifecycle

$(B)/test_pvg_lifecycle: tests/test_pvg_lifecycle.m $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) -fobjc-arc $^ $(METAL_LIBS) -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

# Block-level tests: test_X.c plus X_programs.S, run natively and through the JIT.
$(B)/test_devices: tests/test_devices.c tests/devices_programs.S tests/harness.S tests/harness.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) -Itests tests/test_devices.c tests/devices_programs.S tests/harness.S $(ENGINE) $(DEV) $(METAL_LIBS) -o $@
$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) -o $@

test: all
	@ulimit -s 65520; for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done # a test keeps a whole HvCpu (2 MB of TLB and more) on its stack

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)

.PHONY: all test clean

# ---- iOS: the engine as a static library for the Henge app (ios/) ----
# make ios-lib -> build/ios/libhengevm.a. Networking links the arm64 iOS builds
# of libslirp and glib from Husk's sysroot (scripts there make them); see ios/README.md.
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
# ios/deps/include/slirp first when it holds libslirp's headers (they are all the engine takes from there).
IOS_DEPS ?= $(if $(wildcard ios/deps/include/slirp/libslirp.h),$(CURDIR)/ios/deps,$(HOME)/Documents/Coding/Husk/build/ios-arm64/sysroot)
# Henge needs an A14 or M1 or newer (ios/README.md), so the engine's own C code is built for that core, not for
# the oldest arm64: atomics become single instructions (LSE) instead of load/store-exclusive loops. IOS_TUNE= turns it off.
IOS_TUNE ?= -mcpu=apple-a14
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g $(IOS_TUNE) -Icore -Ihost -Idevices -I$(IOS_DEPS)/include/slirp
IB = build/ios
IOS_SRC = $(wildcard core/*.c) host/prof_ios.c host/jit_region.c host/disk.c host/vm.c host/snapshot.c host/net_slirp.c host/dns_proxy.c host/share.c host/audio_mac.c host/hvsparse.c host/usb_host.c \
          $(filter-out devices/pvg_mac.m,$(wildcard devices/*.c))
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_SRC)) $(IB)/host/enter.o $(IB)/devices/pvg_metal.o

$(IB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/devices/pvg_metal.o: devices/pvg_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -fobjc-arc -Wno-unused-parameter -Wno-deprecated-declarations -c $< -o $@
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
$(IB)/libhengevm.a: $(IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)
# The translation layer (tl/) for the app's experimental Mac apps: its loader and disk-image reader, not the toy AppKit.
TL_SRC = tl/loader/tl_loader.c tl/loader/tl_bundle.m tl/loader/tl_tlv.S tl/dmg/udif.c tl/dmg/volume.c tl/dmg/hfsplus.c tl/dmg/apfs.c tl/dmg/appinfo.c
TL_IOS_OBJ = $(patsubst %,$(IB)/%.o,$(TL_SRC))
$(IB)/tl/%.o: tl/% tl/loader/*.h tl/dmg/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) -O2 -g -Wall -Wno-unused-parameter -fblocks $(if $(filter %.m,$<),-fobjc-arc) -c $< -o $@
$(IB)/libhengetl.a: $(TL_IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(TL_IOS_OBJ)
ios-lib: $(IB)/libhengevm.a $(IB)/libhengetl.a
.PHONY: ios-lib

# The app's glue to it (ios/Henge/Native/henge-tl.c) on the Mac: import a disk image, describe what it holds, run a program.
$(B)/test_tl_glue: tests/test_tl_glue.c ios/Henge/Native/henge-tl.c $(TL_SRC) host/jit_region.c | $(B)
	$(CC) $(CFLAGS) -fblocks -Iios/Henge/Native tests/test_tl_glue.c ios/Henge/Native/henge-tl.c $(filter-out %.m,$(TL_SRC)) host/jit_region.c \
	  -x objective-c -fobjc-arc tl/loader/tl_bundle.m -framework Foundation -lz -lbz2 -lcompression -o $@
# Sample Mac programs for the app's Mac apps screen, as a disk image (read by the same import path as anyone's).
$(B)/tl-samples.dmg: tests/tl/exit3.c tl/tests/hello.c tl/tests/fnd.m tl/tests/swifty.swift tl/tests/cxx.cpp tl/tests/fw.m tl/tests/fwapp.m | $(B)
	make -C tl build/hello build/fnd build/swifty build/cxx build/FW.app/Contents/MacOS/fwapp
	rm -rf $(B)/tl-samples $@ && mkdir -p $(B)/tl-samples
	cp tl/build/hello "$(B)/tl-samples/Hello (C)"
	cp tl/build/fnd "$(B)/tl-samples/Foundation sample"
	cp tl/build/swifty "$(B)/tl-samples/Swift sample"
	cp tl/build/cxx "$(B)/tl-samples/C++ sample"
	ditto tl/build/FW.app "$(B)/tl-samples/Framework sample.app"
	hdiutil create -quiet -srcfolder $(B)/tl-samples -volname "Henge Samples" -fs HFS+ -format UDZO $@
	xattr -c $@ # codesign refuses an app holding a file with Finder information

$(B)/tl-exit3: tests/tl/exit3.c | $(B)
	$(CC) -target arm64-apple-macos13 -O1 $< -o $@
test-tl-glue: $(B)/test_tl_glue $(B)/tl-exit3
	make -C tl all test-dmg >/dev/null
	$(B)/test_tl_glue
	$(B)/test_tl_glue exit3
.PHONY: test-tl-glue

# The restore stack, built from the same sources and with the same stand-ins as iOS.
$(B)/mac-restore/libhengerestore.a: tools/build_ios_restore.sh host/libusb_shim.c host/install.c host/curl_shim.m $(wildcard third_party/*/src/*.c) $(wildcard third_party/libirecovery/src/*.c)
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
