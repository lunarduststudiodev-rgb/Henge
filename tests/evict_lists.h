// Test helper: the block hash, the pages' block lists and their lists of exits from other pages hold together
// (core/machine.h: hv_page_list_*, hv_xlist_*, hv_hash_*). Call it with no CPU running. Returns the faults found,
// printing the first few with where.
#ifndef HENGE_TEST_EVICT_LISTS_H
#define HENGE_TEST_EVICT_LISTS_H

#include <stdio.h>
#include <stdlib.h>

#include "machine.h"

static int evict_lists_block_used(const HvMachine *m, uint32_t i) {
    uint32_t z = i / m->zone_blocks;
    return z < (uint32_t)m->nzones && i - z * m->zone_blocks < hv_zone_nblocks(m, (int)z);
}

static int evict_lists_slot_used(const HvMachine *m, uint32_t s) {
    uint32_t z = s / m->zone_slots;
    return z < (uint32_t)m->nzones && s - z * m->zone_slots < hv_zone_nslots(m, (int)z);
}

static int check_lists(HvMachine *m, const char *where) {
    int bad = 0;
#define LBAD(...)                                                                                                      \
    do {                                                                                                               \
        if (bad++ < 5) {                                                                                               \
            fprintf(stderr, "check_lists (%s): ", where);                                                              \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)
    uint8_t *seen = calloc(m->max_blocks, 1), *xseen = calloc(m->max_slots, 1);
    if (!seen || !xseen) {
        free(seen), free(xseen);
        LBAD("out of memory");
        return bad;
    }
    uint64_t xtotal = 0;
    for (int ri = 0; ri < m->nram; ri++) {
        HvRam *r = &m->ram[ri];
        uint64_t pages = (r->size + (1ull << HV_RAM_PAGE_BITS) - 1) >> HV_RAM_PAGE_BITS;
        for (uint32_t pg = 0; pg < pages; pg++) {
            uint32_t prev = 0;
            for (uint32_t i = r->block_heads[pg]; i;) { /* the page's blocks, forward, each pointing back */
                if (i > m->max_blocks) {
                    LBAD("ram %d page %u: block index %u out of the table", ri, pg, i);
                    break;
                }
                HvBlock *b = &m->blocks[i - 1];
                if (b->page_prev != prev) LBAD("ram %d page %u: block %u points back to %u, not %u", ri, pg, i, b->page_prev, prev);
                if (b->dead) LBAD("ram %d page %u: block %u is retired but listed", ri, pg, i);
                if (b->ram != r || b->pg != pg) LBAD("ram %d page %u: block %u is of another page (%u)", ri, pg, i, b->pg);
                if (!evict_lists_block_used(m, i - 1)) LBAD("ram %d page %u: block %u is in no zone's used records", ri, pg, i);
                if (seen[i - 1]++) {
                    LBAD("ram %d page %u: block %u listed twice (or a loop)", ri, pg, i);
                    break;
                }
                prev = i;
                i = b->page_next;
            }
            prev = 0;
            for (uint32_t l = r->chain_heads[pg]; l;) { /* the page's exits from other pages, likewise */
                if (l > m->max_slots) {
                    LBAD("ram %d page %u: slot index %u out of the table", ri, pg, l);
                    break;
                }
                uint32_t s = l - 1;
                if (m->slot_xprev[s] != prev) LBAD("ram %d page %u: slot %u points back to %u, not %u", ri, pg, l, m->slot_xprev[s], prev);
                if (m->slot_xpg[s] != hv_page_key(m, r, pg)) LBAD("ram %d page %u: slot %u has key %#x", ri, pg, l, m->slot_xpg[s]);
                if (!evict_lists_slot_used(m, s)) LBAD("ram %d page %u: slot %u is in no zone's used slots", ri, pg, l);
                if (xseen[s]++) {
                    LBAD("ram %d page %u: slot %u listed twice (or a loop)", ri, pg, l);
                    break;
                }
                xtotal++;
                prev = l;
                l = m->slot_xnext[s];
            }
        }
    }
    if (xtotal != m->nxchained) LBAD("%llu exits listed, nxchained %u", (unsigned long long)xtotal, m->nxchained);
    for (int z = 0; z < m->nzones; z++) {
        uint32_t b0 = (uint32_t)z * m->zone_blocks, nb = hv_zone_nblocks(m, z);
        for (uint32_t i = b0; i < b0 + nb; i++) {
            HvBlock *b = &m->blocks[i];
            if (!b->dead && !seen[i]) LBAD("block %u (zone %d) is live but on no page's list", i + 1, z);
            if (b->dead && (b->page_prev || b->page_next)) LBAD("block %u (zone %d) is retired but keeps list links", i + 1, z);
            if (!b->dead && hv_block_find(m, hv_block_hash(b->va, b->pa, b->flags), b->va, b->pa, b->flags, b->gen) != b)
                LBAD("block %u (zone %d) is live but no lookup finds it", i + 1, z);
        }
        uint32_t s0 = (uint32_t)z * m->zone_slots, ns = hv_zone_nslots(m, z);
        for (uint32_t s = s0; s < s0 + ns; s++) {
            if (m->slot_xpg[s] && !xseen[s]) LBAD("slot %u (zone %d) has key %#x but is on no list", s + 1, z, m->slot_xpg[s]);
            if (!m->slot_xpg[s] && (m->slot_xprev[s] || m->slot_xnext[s]))
                LBAD("slot %u (zone %d) is on no list but keeps links %u/%u", s + 1, z, m->slot_xprev[s], m->slot_xnext[s]);
        }
    }
    /* Whatever the hash reaches is live, in a zone's used records, and in its own bucket */
    for (uint32_t h = 0; h < HV_BLOCK_HASH; h++) {
        uint32_t n = 0;
        for (uint32_t i = m->hash[h]; i; i = m->blocks[i - 1].hash_next) {
            if (i > m->max_blocks || ++n > m->max_blocks) {
                LBAD("bucket %u: index %u out of the table, or a loop", h, i);
                break;
            }
            HvBlock *b = &m->blocks[i - 1];
            if (b->dead) LBAD("bucket %u: retired block %u is reachable", h, i);
            if (!evict_lists_block_used(m, i - 1)) LBAD("bucket %u: block %u is in no zone's used records", h, i);
            if (hv_block_hash(b->va, b->pa, b->flags) != h) LBAD("bucket %u: block %u belongs in another", h, i);
        }
    }
#undef LBAD
    free(seen);
    free(xseen);
    return bad;
}

#endif
