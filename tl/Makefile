# Henge's translation layer for Mac apps: the loader and its tests (Mac).
# make -C tl test: each test program runs natively and through the loader, and the two outputs must match.
B = build
CC = xcrun clang
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -fblocks
GUEST = -target arm64-apple-macos13
LOADER = loader/tl_loader.c loader/tl_tlv.S loader/tl_bundle.m

TESTS = $(B)/hello $(B)/hello-opcodes $(B)/fnd $(B)/fnd-opcodes $(B)/swifty $(B)/cxx $(B)/FW.app/Contents/MacOS/fwapp

DMG = dmg/udif.c dmg/volume.c dmg/hfsplus.c dmg/apfs.c dmg/appinfo.c

all: $(B)/tlrun $(B)/tlrun-mini $(B)/tldmg $(TESTS)

$(B):
	mkdir -p $(B)

$(B)/tlrun: tools/tlrun.c $(LOADER) loader/tl_loader.h | $(B)
	$(CC) $(CFLAGS) -fobjc-arc tools/tlrun.c $(LOADER) -framework Foundation -o $@
# With the toy AppKit (shim/appkit_mini.m) standing in for the real one.
$(B)/tlrun-mini: tools/tlrun.c $(LOADER) loader/tl_loader.h shim/appkit_mini.m shim/appkit_mini.h | $(B)
	$(CC) $(CFLAGS) -fobjc-arc -DTL_APPKIT_MINI tools/tlrun.c $(LOADER) shim/appkit_mini.m -framework Foundation -framework CoreText -framework CoreGraphics -framework ImageIO -o $@

$(B)/tldmg: tools/tldmg.c $(DMG) dmg/tl_dmg.h dmg/tl_dmg_private.h | $(B)
	$(CC) $(CFLAGS) tools/tldmg.c $(DMG) -framework CoreFoundation -lz -lbz2 -lcompression -o $@

$(B)/hello: tests/hello.c | $(B)
	$(CC) $(GUEST) -O1 $< -o $@
$(B)/hello-opcodes: tests/hello.c | $(B)
	$(CC) -target arm64-apple-macos11 -O1 -Wl,-no_fixup_chains $< -o $@
$(B)/fnd: tests/fnd.m | $(B)
	$(CC) $(GUEST) -O1 -fobjc-arc -framework Foundation $< -o $@
$(B)/fnd-opcodes: tests/fnd.m | $(B)
	$(CC) -target arm64-apple-macos11 -O1 -fobjc-arc -framework Foundation -Wl,-no_fixup_chains $< -o $@
$(B)/swifty: tests/swifty.swift | $(B)
	xcrun swiftc -target arm64-apple-macos13 -O -module-name swifty $< -o $@
$(B)/cxx: tests/cxx.cpp | $(B)
	$(CC) $(GUEST) -O1 -std=c++17 $< -lc++ -o $@

# An app bundle whose executable links a library inside it through @rpath.
$(B)/FW.app/Contents/MacOS/fwapp: tests/fwapp.m tests/fw.m | $(B)
	mkdir -p $(B)/FW.app/Contents/MacOS $(B)/FW.app/Contents/Frameworks
	$(CC) $(GUEST) -O1 -fobjc-arc -dynamiclib -install_name @rpath/libtlfw.dylib -framework Foundation tests/fw.m -o $(B)/FW.app/Contents/Frameworks/libtlfw.dylib
	$(CC) $(GUEST) -O1 -fobjc-arc -framework Foundation -rpath @executable_path/../Frameworks tests/fwapp.m $(B)/FW.app/Contents/Frameworks/libtlfw.dylib -o $@
	printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>fwapp</string><key>CFBundleIdentifier</key><string>dev.henge.tl.fwapp</string><key>CFBundleName</key><string>FW</string><key>LSMinimumSystemVersion</key><string>13.0</string></dict></plist>\n' > $(B)/FW.app/Contents/Info.plist

# Window apps, for sizing what iOS lacks (they cannot be proven on the Mac, where AppKit is real).
SWIFTUI_APP = $(B)/HelloSwiftUI.app/Contents/MacOS/HelloSwiftUI
APPKIT_APP = $(B)/HelloAppKit.app/Contents/MacOS/HelloAppKit
apps: $(SWIFTUI_APP) $(APPKIT_APP)
$(SWIFTUI_APP): tests/apps/HelloSwiftUI.swift | $(B)
	mkdir -p $(B)/HelloSwiftUI.app/Contents/MacOS
	xcrun swiftc -target arm64-apple-macos14 -O -parse-as-library $< -o $@
	printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>HelloSwiftUI</string><key>CFBundleIdentifier</key><string>dev.henge.tl.swiftui</string><key>CFBundleName</key><string>HelloSwiftUI</string><key>LSMinimumSystemVersion</key><string>14.0</string></dict></plist>\n' > $(B)/HelloSwiftUI.app/Contents/Info.plist
$(APPKIT_APP): tests/apps/HelloAppKit.m | $(B)
	mkdir -p $(B)/HelloAppKit.app/Contents/MacOS
	$(CC) $(GUEST) -O1 -fobjc-arc -framework Cocoa $< -o $@
	printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>HelloAppKit</string><key>CFBundleIdentifier</key><string>dev.henge.tl.appkit</string><key>CFBundleName</key><string>HelloAppKit</string><key>LSMinimumSystemVersion</key><string>13.0</string></dict></plist>\n' > $(B)/HelloAppKit.app/Contents/Info.plist

# Disk images made here with hdiutil (HFS+ and APFS, four codecs), read back by tldmg and compared with their source folder.
test-dmg: all
	@src=$(B)/dmgsrc; mkdir -p $$src/docs "$$src/docs/name with spaces ü"; \
	  cp -R $(B)/FW.app $$src/ 2>/dev/null; echo "read me" > $$src/docs/readme.txt; ln -f $$src/docs/readme.txt $$src/docs/hardlink.txt; \
	  ln -sfh ../docs/readme.txt $$src/docs/relative-link; ln -sfh /Applications $$src/Applications; echo hi > "$$src/docs/name with spaces ü/f.txt"; \
	  python3 -c "open('$$src/docs/big.txt','w').write(''.join('line %d of a long compressible file\n' % i for i in range(150000)))"; \
	  fail=0; for fs in HFS+ APFS; do for fmt in UDZO ULFO UDBZ ULMO; do img=$(B)/t-$$fs-$$fmt.dmg; out=$(B)/x-$$fs-$$fmt-$$$$; \
	    hdiutil create -quiet -ov -srcfolder $$src -volname "TL $$fs $$fmt" -fs $$fs -format $$fmt $$img || { echo "FAIL hdiutil $$fs $$fmt"; fail=1; continue; }; \
	    if $(B)/tldmg extract $$img / $$out && python3 -I tests/compare_tree.py $$src $$out >$(B)/cmp.txt; then echo "ok   $$fs $$fmt: $$(tail -1 $(B)/cmp.txt)"; \
	    else echo "FAIL $$fs $$fmt"; cat $(B)/cmp.txt; fail=1; fi; done; done; \
	  $(B)/tldmg apps $(B)/t-APFS-ULFO.dmg $(B)/apps-$$$$ && $(B)/tlrun $(B)/apps-$$$$/FW.app/Contents/MacOS/fwapp | grep -q '^PASS' && echo "ok   app copied out of a dmg runs through the loader" || fail=1; exit $$fail

# Window apps through the loader on this Mac. AppKit and SwiftUI are the Mac's own here, so this
# proves the loader on real window apps, not that they would run on iOS.
test-apps: all apps
	@for a in HelloAppKit HelloSwiftUI; do out=$$(TL_SELFTEST=1 timeout 30 $(B)/tlrun $(B)/$$a.app/Contents/MacOS/$$a 2>&1); \
	  if echo "$$out" | grep -q "^PASS $$a"; then echo "ok   $$a: $$(echo "$$out" | grep -v PASS | tail -1)"; else echo "FAIL $$a"; echo "$$out" | sed 's/^/  /'; fi; done
	@out=$$(TL_SELFTEST=1 TL_SHIM_PNG=$(B)/appkit-mini.png $(B)/tlrun-mini $(APPKIT_APP) 2>&1); \
	  if echo "$$out" | grep -q '^PASS HelloAppKit' && echo "$$out" | grep -q 'was never loaded'; then echo "ok   HelloAppKit on the toy AppKit, no real AppKit: $$(echo "$$out" | grep '^window')"; \
	  else echo "FAIL HelloAppKit on the toy AppKit"; echo "$$out" | sed 's/^/  /'; fi

$(B)/map_names: tests/map_names.c loader/tl_loader.c loader/tl_tlv.S loader/tl_loader.h | $(B)
	$(CC) $(CFLAGS) tests/map_names.c loader/tl_loader.c loader/tl_tlv.S -o $@

test: all $(B)/map_names
	@$(B)/map_names | sed 's/^PASS/ok  /'
	@fail=0; for t in $(TESTS); do \
	  native=$$($$t one two 2>/dev/null); loaded=$$(TLRUN_REPORT=1 $(B)/tlrun $$t one two 2>$(B)/stderr.txt); rc=$$?; \
	  if [ $$rc -eq 0 ] && [ "$$native" = "$$loaded" ] && echo "$$loaded" | grep -q '^PASS'; then echo "ok   $$t: $$(grep 'not in dyld' $(B)/stderr.txt | sed 's/.*bytes, //')"; \
	  else echo "FAIL $$t (exit $$rc)"; echo "  native: $$native"; echo "  loaded: $$loaded"; sed 's/^/  /' $(B)/stderr.txt; fail=1; fi; done; exit $$fail

# The loader, the dmg reader and the toy AppKit compiled for iOS 17, into a static library (not run: that needs the phone).
IOS_CC = xcrun --sdk iphoneos clang -target arm64-apple-ios17.0 -O2 -Wall -Wextra -Wno-unused-parameter -fblocks
ios-lib: | $(B)
	mkdir -p $(B)/ios
	for f in $(LOADER) $(DMG) shim/appkit_mini.m; do $(IOS_CC) -fobjc-arc -c $$f -o $(B)/ios/$$(basename $$f).o || exit 1; done
	xcrun --sdk iphoneos libtool -static -o $(B)/ios/libhengetl.a $(B)/ios/*.o
	@echo "ok   $(B)/ios/libhengetl.a"

clean:
	rm -rf $(B)

.PHONY: all test clean apps test-apps test-dmg ios-lib
