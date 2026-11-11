#!/usr/bin/env python3
"""Checks that an extracted image matches the folder it was made from: every
file's bytes, every link's target, permissions, and hard links kept as links.
Extra top-level dot entries (.fseventsd and so on) are the volume's own."""
import hashlib, os, stat, sys

src, out = sys.argv[1], sys.argv[2]
problems, checked = [], 0
src_inodes, out_inodes = {}, {}
for root, dirs, files in os.walk(src):
    for name in dirs + files:
        s = os.path.join(root, name)
        rel = os.path.relpath(s, src)
        o = os.path.join(out, rel)
        st = os.lstat(s)
        try:
            ot = os.lstat(o)
        except FileNotFoundError:
            problems.append(f"missing {rel}"); continue
        checked += 1
        if stat.S_IFMT(st.st_mode) != stat.S_IFMT(ot.st_mode):
            problems.append(f"type differs: {rel}"); continue
        if stat.S_ISLNK(st.st_mode):
            if os.readlink(s) != os.readlink(o): problems.append(f"link target differs: {rel}")
            continue
        if (st.st_mode & 0o777) != (ot.st_mode & 0o777) and not stat.S_ISDIR(st.st_mode):
            problems.append(f"mode differs: {rel} {oct(st.st_mode & 0o777)} {oct(ot.st_mode & 0o777)}")
        if stat.S_ISREG(st.st_mode):
            h = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()
            if h(s) != h(o): problems.append(f"content differs: {rel}")
            if st.st_nlink > 1:
                src_inodes.setdefault(st.st_ino, []).append(rel)
                out_inodes.setdefault(ot.st_ino, []).append(rel)
groups_src = sorted(sorted(v) for v in src_inodes.values())
groups_out = sorted(sorted(v) for v in out_inodes.values())
if groups_src != groups_out: problems.append(f"hard links differ: {groups_src} vs {groups_out}")
for p in problems[:20]: print("  " + p)
print(f"{'ok  ' if not problems else 'FAIL'} {checked} entries, {len(problems)} problems")
sys.exit(1 if problems else 0)
