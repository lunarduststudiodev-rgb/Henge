# glib 2.83.0 headers for arm64 iOS

The public headers of the glib the iPad app links (`ios/deps/lib/libglib-2.0.a`, built by Husk's
`scripts/build_ios.sh` from glib 2.83.0). The Makefile compiles `third_party/libslirp` against them for the iOS
library, so libslirp sees the same `glibconfig.h` (static build, 2.83.0, iOS's type sizes) as the code it links.

Generated, not edited: `tools/glib_ios_headers.sh` downloads glib 2.83.0 (checksum pinned), configures it with
Husk's iOS cross file and glib options, and copies what `meson install` puts in `include/glib-2.0` (glib itself,
not gio, gobject, gmodule or girepository) and `lib/glib-2.0/include/glibconfig.h`. `ci/build-ipa.sh` runs
`tools/glib_ios_headers.sh --check` against Husk's sysroot after building glib, and stops if the version or
any file differs.

glib is LGPL-2.1-or-later (`COPYING`).
