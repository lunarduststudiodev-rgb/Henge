/* Metal libraries as macOS sends them, relabelled for an older iOS (devices/metallib.h).

   A metallib (as xcrun metallib writes it, file version 2.x) starts
     +0 "MTLB", +4 u16 platform (0x8001 macOS, 0x0001 iOS), +6 u16 file version major, +8 minor,
     +10 u8 library type, +11 u8 target OS (0x81 macOS, 0x82 iOS), +12 u16 OS version major, +14 minor,
     +16 u64 file size, +24 u64 function list offset, +32 its size, +40/+48 public metadata,
     +56/+64 private metadata, +72 u64 bitcode offset, +80 its size.
   The function list is a u32 count (not in the list size), then per function a u32 size (counted from that word) and tags
   {4-byte name, u16 length, data} up to ENDT: HASH (SHA-256 of the function's bitcode), MDSZ (its
   size), OFFT (u64 public, private and bitcode offsets, relative to the sections), VERS (u16 AIR
   major, minor, language major, minor). The same shader built for macOS 15 and iOS 17 differs in
   the header, VERS (AIR 2.7 and Metal 3.2 against 2.6 and 3.1), and in the bitcode: the target triple
   (air64_v27-apple-macosx15.0.0 against air64_v26-apple-ios17.0.0), the named metadata
   !air.version = !{i32 2, i32 7, i32 0} against 2.6.0 and !air.language_version = !{!"Metal", i32 3, i32 2, i32 0}
   against 3.1.0, and so the hash. Metal's compiler checks the metadata ("Target OS is incompatible").

   Each function's bitcode is an LLVM bitstream (in the 0x0B17C0DE wrapper). It is read only as far as the module
   block's own records and its type, constant and metadata blocks, and changed only by rewriting fields in place with
   values of the same width, so nothing after them moves: the triple's characters (each printable character takes the
   same width in every encoding LLVM uses for one), and the version nodes' operands, each pointed at another metadata
   constant of the same type already holding the wanted number (the constants themselves may be shared with code). */
#include "metallib.h"

#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const HvMetallibTarget hv_metallib_ios17 = { "ios", 0x0001, 0x82, 17, 7, 2, 6, 3, 1 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

/* ---- triples ---- */

/* "air64[_vNN]-apple-<os><version>" of length n as the target's, the same length: the version padded as "17.000.00",
   which LLVM reads as 17.0.0, and the AIR version in it (if it has one) the target's. 1: out differs; 0: already the
   target's; -1: not a triple this rewrites. */
static int triple_for(const char *s, size_t n, const HvMetallibTarget *t, char *out) {
    static const char apple[] = "-apple-";
    if (n < 5 + sizeof apple - 1 + 1 || n > 63 || memcmp(s, "air64", 5) != 0) return -1;
    size_t i = 5;
    int has_v = 0;
    if (i + 4 <= n && s[i] == '_' && s[i + 1] == 'v' && s[i + 2] >= '0' && s[i + 2] <= '9' && s[i + 3] >= '0' && s[i + 3] <= '9')
        has_v = 1, i += 4;
    if (i + sizeof apple - 1 > n || memcmp(s + i, apple, sizeof apple - 1) != 0) return -1;
    i += sizeof apple - 1;
    size_t os = i;
    while (i < n && s[i] >= 'a' && s[i] <= 'z') i++;
    if (i == os) return -1;
    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) i++;
    if (i != n) return -1; /* an environment after the version (-simulator, -macabi): left alone */
    if (t->air_major > 9 || t->air_minor > 9 || t->os_major > 99) return -1;
    char head[64];
    int h = has_v ? snprintf(head, sizeof head, "air64_v%u%u-apple-%s%u.", t->air_major, t->air_minor, t->os, t->os_major)
                  : snprintf(head, sizeof head, "air64-apple-%s%u.", t->os, t->os_major);
    if (h < 0 || (size_t)h + 1 > n) return -1;
    size_t r = n - (size_t)h;
    memcpy(out, head, (size_t)h);
    memset(out + h, '0', r);
    if (r >= 3) out[h + r / 2] = '.';
    return memcmp(out, s, n) != 0;
}

/* The last triple that could not be rewritten, for the log (static: the reason strings are). */
static char triple_seen[128];

/* Every plain-text triple in [p, p + n) (the symbol table's copy) as the target's. */
static int relabel_plain_triples(uint8_t *p, size_t n, const HvMetallibTarget *t) {
    int changed = 0;
    for (size_t i = 0; i + 6 <= n; i++) {
        if (memcmp(p + i, "air64", 5) != 0 || (p[i + 5] != '_' && p[i + 5] != '-')) continue;
        size_t e = i;
        while (e < n && ((p[e] >= 'a' && p[e] <= 'z') || (p[e] >= '0' && p[e] <= '9') || p[e] == '_' || p[e] == '-' || p[e] == '.')) e++;
        char out[64];
        if (e - i < sizeof out && triple_for((const char *)p + i, e - i, t, out) == 1) {
            memcpy(p + i, out, e - i);
            changed++;
        }
        i = e - 1;
    }
    return changed;
}

/* ---- the bitstream ---- */

typedef struct { const uint8_t *p; size_t bits, pos; } Bits;

static int bget(Bits *b, unsigned n, uint64_t *v) {
    if (n > 64 || b->pos + n > b->bits) return -1;
    uint64_t r = 0;
    for (unsigned i = 0; i < n; i++, b->pos++) r |= (uint64_t)(b->p[b->pos >> 3] >> (b->pos & 7) & 1) << i;
    *v = r;
    return 0;
}

static int bvbr(Bits *b, unsigned n, uint64_t *v) {
    uint64_t r = 0, c;
    if (n < 2) return -1;
    for (unsigned s = 0; s < 64; s += n - 1) {
        if (bget(b, n, &c)) return -1;
        r |= (c & ((1ull << (n - 1)) - 1)) << s;
        if (!(c >> (n - 1))) { *v = r; return 0; }
    }
    return -1;
}

static int balign(Bits *b) {
    b->pos = (b->pos + 31) & ~(size_t)31;
    return b->pos <= b->bits ? 0 : -1;
}

static void bput(uint8_t *p, size_t pos, unsigned n, uint64_t v) {
    for (unsigned i = 0; i < n; i++, pos++) {
        if (v >> i & 1) p[pos >> 3] |= (uint8_t)(1u << (pos & 7));
        else p[pos >> 3] &= (uint8_t)~(1u << (pos & 7));
    }
}

enum { OP_LIT, OP_FIXED, OP_VBR, OP_ARRAY, OP_CHAR6, OP_BLOB };
typedef struct { uint8_t kind; uint8_t width; uint64_t val; } AbbrevOp;
#define ABBREV_OPS 32
typedef struct { AbbrevOp op[ABBREV_OPS]; unsigned n; } Abbrev;

static int char6_dec(uint64_t v) { return v < 26 ? 'a' + (int)v : v < 52 ? 'A' + (int)v - 26 : v < 62 ? '0' + (int)v - 52 : v == 62 ? '.' : '_'; }
static int char6_enc(int c) {
    return c >= 'a' && c <= 'z' ? c - 'a' : c >= 'A' && c <= 'Z' ? c - 'A' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '.' ? 62 : c == '_' ? 63 : -1;
}

/* A record's operand as read: its value, and where its bits are and how they are encoded (to write another value of the
   same width there). Literal operands have no bits. */
typedef struct { uint64_t v; size_t at; uint8_t kind, width, bits; } Field;
#define REC_FIELDS 72
typedef struct { uint64_t code; Field f[REC_FIELDS]; unsigned n, total; } Record;

static void rec_add(Record *r, uint64_t v, size_t at, uint8_t kind, uint8_t width, uint8_t bits) {
    if (r->n < REC_FIELDS) r->f[r->n++] = (Field){ v, at, kind, width, bits };
    r->total++;
}

static int read_scalar(Bits *b, const AbbrevOp *o, Field *f) {
    f->at = b->pos, f->kind = o->kind, f->width = o->width;
    int r = o->kind == OP_FIXED ? bget(b, o->width, &f->v) : o->kind == OP_VBR ? bvbr(b, o->width, &f->v)
          : o->kind == OP_CHAR6 ? bget(b, 6, &f->v) : -1;
    if (o->kind == OP_CHAR6 && !r) f->v = (uint64_t)char6_dec(f->v);
    f->bits = (uint8_t)(b->pos - f->at);
    return r;
}

/* Writes v where f was, in f's encoding and width. 0: done; -1: v does not take the same bits. */
static int put_field(uint8_t *p, const Field *f, uint64_t v) {
    if (f->kind == OP_CHAR6) {
        int e = char6_enc((int)v);
        if (v > 255 || e < 0) return -1;
        bput(p, f->at, 6, (uint64_t)e);
    } else if (f->kind == OP_FIXED) {
        if (f->width < 64 && v >> f->width) return -1;
        bput(p, f->at, f->width, v);
    } else if (f->kind == OP_VBR) {
        unsigned n = f->width, chunks = f->bits / n, need = 1;
        for (uint64_t x = v >> (n - 1); x; x >>= n - 1) need++;
        if (need != chunks) return -1; /* the same number of chunks, checked before anything is written */
        uint64_t left = v;
        for (unsigned k = 0; k < chunks; k++) {
            uint64_t part = left & ((1ull << (n - 1)) - 1);
            left >>= n - 1;
            if (k + 1 < chunks) part |= 1ull << (n - 1);
            bput(p, f->at + (size_t)k * n, n, part);
        }
    } else return -1;
    return 0;
}

static int read_abbrev_def(Bits *b, Abbrev *d) {
    uint64_t n;
    if (bvbr(b, 5, &n) || n > ABBREV_OPS) return -1;
    d->n = (unsigned)n;
    for (unsigned i = 0; i < n; i++) {
        uint64_t lit, x;
        if (bget(b, 1, &lit)) return -1;
        if (lit) { if (bvbr(b, 8, &x)) return -1; d->op[i] = (AbbrevOp){ OP_LIT, 0, x }; continue; }
        if (bget(b, 3, &x)) return -1;
        if (x == 1 || x == 2) {
            uint64_t w;
            if (bvbr(b, 5, &w) || w > 64 || (x == 2 && w < 2)) return -1;
            d->op[i] = (AbbrevOp){ x == 1 ? OP_FIXED : OP_VBR, (uint8_t)w, 0 };
        } else if (x == 3 || x == 4 || x == 5) {
            d->op[i] = (AbbrevOp){ x == 3 ? OP_ARRAY : x == 4 ? OP_CHAR6 : OP_BLOB, 0, 0 };
        } else return -1;
    }
    return 0;
}

static int read_record(Bits *b, uint64_t id, const Abbrev *abbrevs, unsigned nab, Record *r) {
    r->n = r->total = 0;
    if (id == 3) { /* UNABBREV_RECORD: code, count, operands, all VBR6 */
        uint64_t n;
        if (bvbr(b, 6, &r->code) || bvbr(b, 6, &n)) return -1;
        AbbrevOp v6 = { OP_VBR, 6, 0 };
        for (uint64_t i = 0; i < n; i++) {
            Field f;
            if (read_scalar(b, &v6, &f)) return -1;
            rec_add(r, f.v, f.at, f.kind, f.width, f.bits);
        }
        return 0;
    }
    if (id < 4 || id - 4 >= nab) return -1;
    const Abbrev *d = &abbrevs[id - 4];
    int have_code = 0;
    for (unsigned i = 0; i < d->n; i++) {
        const AbbrevOp *o = &d->op[i];
        if (o->kind == OP_ARRAY || o->kind == OP_BLOB) {
            uint64_t n;
            if (bvbr(b, 6, &n)) return -1;
            if (o->kind == OP_BLOB) { /* 32-bit aligned bytes, aligned again after */
                if (balign(b) || n > (b->bits - b->pos) / 8) return -1;
                b->pos += n * 8;
                if (balign(b)) return -1;
                if (!have_code) return -1;
                r->total++;
                continue;
            }
            if (i + 1 >= d->n) return -1;
            const AbbrevOp *e = &d->op[++i];
            for (uint64_t k = 0; k < n; k++) {
                Field f;
                if (read_scalar(b, e, &f)) return -1;
                if (!have_code) { r->code = f.v, have_code = 1; continue; }
                rec_add(r, f.v, f.at, f.kind, f.width, f.bits);
            }
        } else if (o->kind == OP_LIT) {
            if (!have_code) { r->code = o->val, have_code = 1; continue; }
            rec_add(r, o->val, 0, OP_LIT, 0, 0);
        } else {
            Field f;
            if (read_scalar(b, o, &f)) return -1;
            if (!have_code) { r->code = f.v, have_code = 1; continue; }
            rec_add(r, f.v, f.at, f.kind, f.width, f.bits);
        }
    }
    return have_code ? 0 : -1;
}

/* ---- what is read of a module ---- */

enum { BLOCKINFO = 0, MODULE = 8, CONSTANTS = 11, METADATA = 15, TYPES = 17 };
#define INFO_IDS 32
#define INFO_ABBREVS 32
#define LOCAL_ABBREVS 96

typedef struct { uint32_t ty; uint8_t is_int; int64_t v; } Value;          /* a module-level value */
typedef struct { uint8_t kind; uint32_t ty; uint64_t val; Field op[4]; uint8_t nops; } Meta;  /* kind: 0 other, 1 VALUE, 2 small NODE */

typedef struct {
    Abbrev info[INFO_IDS][INFO_ABBREVS];
    unsigned ninfo[INFO_IDS];
    uint8_t *types;   /* per type id: the integer width, 0 for not an integer */
    size_t ntypes, captypes;
    Value *values;
    size_t nvalues, capvalues;
    Meta *meta;
    size_t nmeta, capmeta;
    Record triple;
    int have_triple;
    int64_t version_node, language_node; /* metadata ids, -1 if absent */
    char name[64];
    int have_name;
} Module;

static int grow(void **p, size_t *cap, size_t need, size_t each) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * each);
    if (!q) return -1;
    *p = q, *cap = c;
    return 0;
}

static int64_t sign_vbr(uint64_t v) { return v & 1 ? -(int64_t)(v >> 1) : (int64_t)(v >> 1); }

static void on_record(Module *m, uint64_t block, const Record *r, unsigned *setbid, int depth) {
    if (block == BLOCKINFO) {
        if (r->code == 1 && r->n) *setbid = (unsigned)r->f[0].v;
    } else if (block == MODULE && depth == 1) {
        if (r->code == 2) m->triple = *r, m->have_triple = 1;
        else if (r->code == 7 || r->code == 8 || r->code == 9 || r->code == 14 || r->code == 15) { /* globals, functions, aliases, ifuncs */
            if (!grow((void **)&m->values, &m->capvalues, m->nvalues + 1, sizeof *m->values))
                m->values[m->nvalues++] = (Value){ UINT32_MAX, 0, 0 };
        }
    } else if (block == TYPES && depth == 2) {
        if (r->code == 1 || r->code == 19) return; /* NUMENTRY, STRUCT_NAME */
        if (grow((void **)&m->types, &m->captypes, m->ntypes + 1, 1)) return;
        m->types[m->ntypes++] = r->code == 7 && r->n && r->f[0].v <= 64 ? (uint8_t)r->f[0].v : 0; /* INTEGER: width */
    }
}

static uint32_t const_type; /* the CONSTANTS block's current type (SETTYPE); per walk, reset as each block starts */

static void on_constant(Module *m, const Record *r) {
    if (r->code == 1) { if (r->n) const_type = (uint32_t)r->f[0].v; return; } /* SETTYPE */
    if (grow((void **)&m->values, &m->capvalues, m->nvalues + 1, sizeof *m->values)) return;
    Value v = { const_type, 0, 0 };
    int is_int = const_type < m->ntypes && m->types[const_type];
    if (r->code == 4 && r->n && is_int) v.is_int = 1, v.v = sign_vbr(r->f[0].v); /* INTEGER */
    else if (r->code == 2 && is_int) v.is_int = 1, v.v = 0;                       /* NULL: an integer's is 0 */
    m->values[m->nvalues++] = v;
}

static void on_metadata(Module *m, const Record *r) {
    switch (r->code) {
    case 35: /* STRINGS: count, offsets size, blob */
        if (r->n && !grow((void **)&m->meta, &m->capmeta, m->nmeta + r->f[0].v, sizeof *m->meta)) {
            memset(m->meta + m->nmeta, 0, (size_t)r->f[0].v * sizeof *m->meta);
            m->nmeta += (size_t)r->f[0].v;
        }
        return;
    case 4: /* NAME: the next NAMED_NODE's */
        m->have_name = 0;
        if (r->n == r->total && r->n < sizeof m->name) {
            for (unsigned i = 0; i < r->n; i++) m->name[i] = (char)r->f[i].v;
            m->name[r->n] = 0, m->have_name = 1;
        }
        return;
    case 10: /* NAMED_NODE: its nodes' ids */
        if (m->have_name && r->n == 1) {
            if (!strcmp(m->name, "air.version")) m->version_node = (int64_t)r->f[0].v;
            if (!strcmp(m->name, "air.language_version")) m->language_node = (int64_t)r->f[0].v;
        }
        m->have_name = 0;
        return;
    case 6: case 11: case 36: case 38: case 39: /* KIND, ATTACHMENT, GLOBAL_DECL_ATTACHMENT, INDEX_OFFSET, INDEX */
        return;
    }
    if (grow((void **)&m->meta, &m->capmeta, m->nmeta + 1, sizeof *m->meta)) return;
    Meta x;
    memset(&x, 0, sizeof x);
    if (r->code == 2 && r->n == 2) x.kind = 1, x.ty = (uint32_t)r->f[0].v, x.val = r->f[1].v; /* VALUE: type, value id */
    else if ((r->code == 3 || r->code == 5) && r->n == r->total && r->n <= 4) {                /* NODE, DISTINCT_NODE */
        x.kind = 2, x.nops = (uint8_t)r->n;
        for (unsigned i = 0; i < r->n; i++) x.op[i] = r->f[i];
    }
    m->meta[m->nmeta++] = x;
}

/* Walks a block whose header has been read. The module's own type, constant and metadata blocks are read; every other
   block is skipped whole. */
static int walk(Bits *b, Module *m, uint64_t block, unsigned width, int depth) {
    Abbrev local[LOCAL_ABBREVS];
    unsigned nab = 0, setbid = 0;
    if (block < INFO_IDS && block != BLOCKINFO)
        for (unsigned i = 0; i < m->ninfo[block]; i++) local[nab++] = m->info[block][i];
    if (block == CONSTANTS) const_type = UINT32_MAX;
    Record *r = malloc(sizeof *r);
    if (!r) return -1;
    int rc = -1;
    for (;;) {
        uint64_t a;
        if (bget(b, width, &a)) break;
        if (a == 0) { rc = balign(b); break; } /* END_BLOCK */
        if (a == 1) {                           /* ENTER_SUBBLOCK */
            uint64_t id, aw, words;
            if (bvbr(b, 8, &id) || bvbr(b, 4, &aw) || balign(b) || bget(b, 32, &words)) break;
            size_t end = b->pos + words * 32;
            if (end > b->bits) break;
            int inner = id == BLOCKINFO || (depth == 1 && block == MODULE && (id == TYPES || id == CONSTANTS || id == METADATA));
            if (inner) {
                if (aw < 1 || aw > 32 || walk(b, m, id, (unsigned)aw, depth + 1) || b->pos != end) break;
            } else b->pos = end;
            continue;
        }
        if (a == 2) { /* DEFINE_ABBREV: for this block, or in BLOCKINFO for the block SETBID named */
            Abbrev d;
            if (read_abbrev_def(b, &d)) break;
            if (block == BLOCKINFO) {
                if (setbid < INFO_IDS && m->ninfo[setbid] < INFO_ABBREVS) m->info[setbid][m->ninfo[setbid]++] = d;
            } else {
                if (nab == LOCAL_ABBREVS) break;
                local[nab++] = d;
            }
            continue;
        }
        if (read_record(b, a, local, nab, r)) break;
        if (block == CONSTANTS && depth == 2) on_constant(m, r);
        else if (block == METADATA && depth == 2) on_metadata(m, r);
        else on_record(m, block, r, &setbid, depth);
    }
    free(r);
    return rc;
}

/* The integer a metadata id stands for: 0 if it is a VALUE of an integer constant, with its type. */
static int meta_int(const Module *m, uint64_t id, uint32_t *ty, int64_t *v) {
    if (id >= m->nmeta || m->meta[id].kind != 1) return -1;
    const Meta *x = &m->meta[id];
    if (x->val >= m->nvalues || !m->values[x->val].is_int) return -1;
    *ty = x->ty, *v = m->values[x->val].v;
    return 0;
}

/* Points operand f at a metadata constant of type ty holding v. 0: done; -1: none (at the same width). */
static int point_at(uint8_t *bc, const Module *m, const Field *f, uint32_t ty, int64_t v) {
    for (uint64_t k = 0; k < m->nmeta; k++) {
        uint32_t kty;
        int64_t kv;
        if (meta_int(m, k, &kty, &kv) || kty != ty || kv != v) continue;
        if (!put_field(bc, f, k + 1)) return 0; /* node operands are ids + 1 */
    }
    return -1;
}

/* Points a version node's operands at metadata holding want[] (want[i] < 0: leave operand i). Where no constant holds
   want[i] and lower_ok has bit i (a minor version), the highest one below it does: an older version than the target's,
   which it also reads (*why notes it). Changes counted, -1 not possible. */
static int retarget_node(uint8_t *bc, const Module *m, int64_t node, const int64_t *want, unsigned n, unsigned lower_ok,
                         const char *missing, const char *lowered, const char **why) {
    if (node < 0 || (uint64_t)node >= m->nmeta || m->meta[node].kind != 2 || m->meta[node].nops != n) {
        *why = "a version node is not one this reads";
        return -1;
    }
    const Meta *x = &m->meta[node];
    int changed = 0;
    for (unsigned i = 0; i < n; i++) {
        if (want[i] < 0) continue;
        uint32_t ty;
        int64_t v;
        if (!x->op[i].v || meta_int(m, x->op[i].v - 1, &ty, &v)) { *why = "a version number is not an integer constant"; return -1; }
        if (v == want[i]) continue;
        if (!point_at(bc, m, &x->op[i], ty, want[i])) { changed++; continue; }
        int done = 0;
        for (int64_t w = want[i] - 1; (lower_ok >> i & 1) && w >= 0 && !done; w--) done = !point_at(bc, m, &x->op[i], ty, w);
        if (!done) { *why = missing; return -1; }
        if (!*why) *why = lowered;
        changed++;
    }
    return changed;
}

/* The module's triple and version metadata in [code, code + len), as the target's. Changes counted, -1 if not. */
static int relabel_bitcode(uint8_t *code, size_t len, const HvMetallibTarget *t, int versions, const char **why) {
    size_t start = 0, size = len;
    if (len >= 20 && rd32(code) == 0x0B17C0DE) start = rd32(code + 8), size = rd32(code + 12);
    if (start > len || size > len - start || size < 8 || memcmp(code + start, "BC\xC0\xDE", 4) != 0) { *why = "bitcode not found"; return -1; }
    uint8_t *bc = code + start;
    Bits b = { bc, size * 8, 32 };
    Module *m = calloc(1, sizeof *m);
    if (!m) return -1;
    m->version_node = m->language_node = -1;
    int rc = -1, found = 0;
    for (;;) { /* the top level: blocks only; the module's read, the others skipped */
        uint64_t a, id, aw, words;
        if (bget(&b, 2, &a) || a != 1 || bvbr(&b, 8, &id) || bvbr(&b, 4, &aw) || balign(&b) || bget(&b, 32, &words)) break;
        size_t end = b.pos + words * 32;
        if (end > b.bits) break;
        if (id == MODULE || id == BLOCKINFO) {
            if (aw < 2 || aw > 32 || walk(&b, m, id, (unsigned)aw, 1)) break;
            if (id == MODULE) { found = 1; break; }
        } else b.pos = end;
        if (b.pos >= b.bits) break;
    }
    if (!found) { *why = "the module could not be read"; goto out; }
    rc = 0;
    if (m->have_triple) { /* its characters, each into its own field */
        Record *r = &m->triple;
        char s[REC_FIELDS + 1], o[64];
        int ok = r->n == r->total && r->n < sizeof o;
        for (unsigned i = 0; ok && i < r->n; i++) ok = r->f[i].kind != OP_LIT && r->f[i].v && r->f[i].v < 128, s[i] = (char)r->f[i].v;
        if (!ok) { *why = "the module's TRIPLE record is not plain characters"; rc = -1; goto out; }
        int c = triple_for(s, r->n, t, o);
        if (c < 0) {
            snprintf(triple_seen, sizeof triple_seen, "the module's triple is not one this rewrites: \"%.*s\"", (int)r->n, s);
            *why = triple_seen;
            rc = -1;
            goto out;
        }
        for (unsigned i = 0; c && i < r->n; i++)
            if (o[i] != s[i] && put_field(bc, &r->f[i], (unsigned char)o[i])) { *why = "the triple's characters do not fit"; rc = -1; goto out; }
        rc += c;
    }
    int64_t ver[3] = { t->air_major, t->air_minor, -1 }, lang[4] = { -1, t->lang_major, t->lang_minor, -1 };
    /* Only ever lowered: a library already old enough keeps its numbers. */
    {
        uint32_t ty;
        int64_t maj, min;
        if (m->version_node >= 0 && m->meta[m->version_node].kind == 2 && m->meta[m->version_node].nops == 3 &&
            !meta_int(m, m->meta[m->version_node].op[0].v - 1, &ty, &maj) && !meta_int(m, m->meta[m->version_node].op[1].v - 1, &ty, &min) &&
            (maj < t->air_major || (maj == t->air_major && min <= t->air_minor)))
            ver[0] = ver[1] = -1;
        if (m->language_node >= 0 && m->meta[m->language_node].kind == 2 && m->meta[m->language_node].nops == 4 &&
            !meta_int(m, m->meta[m->language_node].op[1].v - 1, &ty, &maj) && !meta_int(m, m->meta[m->language_node].op[2].v - 1, &ty, &min) &&
            (maj < t->lang_major || (maj == t->lang_major && min <= t->lang_minor)))
            lang[1] = lang[2] = -1;
    }
    if (!versions) goto out;
    if (m->version_node >= 0) {
        int c = retarget_node(bc, m, m->version_node, ver, 3, 1u << 1, "no metadata constant holds the AIR version wanted",
                              "the AIR version is claimed older than the target's (no metadata constant holds its minor)", why);
        if (c < 0) { rc = -1; goto out; }
        rc += c;
    }
    if (m->language_node >= 0) {
        int c = retarget_node(bc, m, m->language_node, lang, 4, 1u << 2, "no metadata constant holds the Metal version wanted",
                              "the Metal version is claimed older than the target's (no metadata constant holds its minor)", why);
        if (c < 0) { rc = -1; goto out; }
        rc += c;
    }
out:
    free(m->types), free(m->values), free(m->meta), free(m);
    return rc;
}

/* ---- the library ---- */

int hv_metallib_relabel_for(uint8_t *buf, size_t len, int stage, const HvMetallibTarget *t, const char **why) {
    const char *dummy;
    if (!why) why = &dummy;
    *why = NULL;
    if (len < 88 || memcmp(buf, "MTLB", 4) != 0 || rd16(buf + 6) != 2) return -1;
    if (rd64(buf + 16) > len) return -1;
    int changed = 0;
    if (rd16(buf + 4) != t->platform) wr16(buf + 4, t->platform), changed++;
    if (rd16(buf + 8) > t->file_minor) wr16(buf + 8, t->file_minor), changed++;
    int other_os = buf[11] != t->os_byte;
    if (other_os) buf[11] = t->os_byte, changed++;
    if ((other_os || rd16(buf + 12) > t->os_major) && (rd16(buf + 12) != t->os_major || rd16(buf + 14) != 0))
        wr16(buf + 12, t->os_major), wr16(buf + 14, 0), changed++;
    uint64_t fl = rd64(buf + 24), fls = rd64(buf + 32), bc = rd64(buf + 72), bcs = rd64(buf + 80);
    if (fl > len || fls > len - fl - 4 || bc > len || bcs > len - bc) return changed;
    const uint8_t *end = buf + fl + 4 + fls; /* the size counts the entries after the count */
    uint32_t count = rd32(buf + fl);
    uint8_t *f = buf + fl + 4;
    for (uint32_t k = 0; k < count && f + 4 <= end; k++) {
        uint32_t size = rd32(f);
        if (size < 4 || size > (size_t)(end - f)) break;
        uint8_t *hash = NULL, *tg = f + 4, *fend = f + size;
        uint64_t mdsz = 0, bc_off = ~0ull;
        while (tg + 6 <= fend && memcmp(tg, "ENDT", 4) != 0) {
            uint16_t n = rd16(tg + 4);
            uint8_t *d = tg + 6;
            if (d + n > fend) break;
            if (!memcmp(tg, "VERS", 4) && n >= 8) {
                if (rd16(d) == t->air_major && rd16(d + 2) > t->air_minor) wr16(d + 2, t->air_minor), changed++;
                if (rd16(d + 4) > t->lang_major || (rd16(d + 4) == t->lang_major && rd16(d + 6) > t->lang_minor))
                    wr16(d + 4, t->lang_major), wr16(d + 6, t->lang_minor), changed++;
            } else if (!memcmp(tg, "HASH", 4) && n == 32) {
                hash = d;
            } else if (!memcmp(tg, "MDSZ", 4) && n == 8) {
                mdsz = rd64(d);
            } else if (!memcmp(tg, "OFFT", 4) && n == 24) {
                bc_off = rd64(d + 16);
            }
            tg = d + n;
        }
        if (stage >= 2 && hash && bc_off != ~0ull && bc_off <= bcs && mdsz <= bcs - bc_off) {
            uint8_t *code = buf + bc + bc_off;
            const char *w = NULL;
            int c = relabel_bitcode(code, (size_t)mdsz, t, stage >= 3, &w);
            if (!*why && (c < 0 || w)) *why = w ? w : "unreadable bitcode";
            int p = relabel_plain_triples(code, (size_t)mdsz, t);
            if (c > 0 || p > 0) {
                CC_SHA256(code, (CC_LONG)mdsz, hash);
                changed += (c > 0 ? c : 0) + p;
            }
        }
        f += size;
    }
    return changed;
}

int hv_metallib_relabel(uint8_t *buf, size_t len, int stage, const char **why) {
    return hv_metallib_relabel_for(buf, len, stage, &hv_metallib_ios17, why);
}
