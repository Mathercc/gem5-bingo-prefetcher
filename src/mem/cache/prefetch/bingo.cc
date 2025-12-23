#include "mem/cache/prefetch/bingo.hh"

#include <algorithm>
#include <cmath>

#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{
namespace prefetch
{

// Stats
BingoPrefetcher::BingoStats::BingoStats(statistics::Group *parent)
    : statistics::Group(parent),
      lookups(this, "bingo_lookups", "calculatePrefetch calls"),

      hist_long_hits(this, "bingo_hist_long_hits",
                     "history long-event hits (PC+page)"),
      hist_short_hits(this, "bingo_hist_short_hits",
                      "history short-event hits (PC+offset)"),
      hist_misses(this, "bingo_hist_misses", "history misses"),

      pagebuf_hits(this, "bingo_pagebuf_hits", "page buffer hits"),
      pagebuf_inserts(this, "bingo_pagebuf_inserts", "page buffer inserts"),
      pagebuf_evictions(this, "bingo_pagebuf_evictions",
                        "page buffer evictions (capacity; may commit-to-history)"),
      pagebuf_commits(this, "bingo_pagebuf_commits",
                      "page footprints committed on true residency end (eviction-driven)"),

      issued(this, "bingo_issued", "prefetches issued"),
      dropped_dup(this, "bingo_dropped_dup", "dropped duplicates in same trigger"),
      dropped_same_page(this, "bingo_dropped_same_page",
                        "dropped due to same-page policy")
{
}

// ctor
BingoPrefetcher::BingoPrefetcher(const Params &p)
    : Queued(p),
      degree(p.degree),
      enforceSamePage(true),
      historyEntries(p.history_entries),
      historyAssoc(p.history_assoc),
      pageBufEntries(p.page_buf_entries),
      votePercent(p.vote_percent),
      stats(this),
      history(historyEntries / historyAssoc, HistSet(historyAssoc)),
      pagebuf(pageBufEntries)
{
    regProbeListeners();
}

// probeAddr helper 
Addr
BingoPrefetcher::probeAddr(const PacketPtr &pkt) const
{
    if (!pkt || !pkt->req) {
        return 0;
    }
    if (useVirtualAddresses) {
        return pkt->getAddr();
    }
    return pkt->req->getPaddr();
}

uint64_t
BingoPrefetcher::mix64(uint64_t x) const
{
    // splitmix64
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t
BingoPrefetcher::hashLong(Addr pc, Addr page_tag) const
{
    return mix64((uint64_t)pc ^ (mix64((uint64_t)page_tag) + 0x1234ULL));
}

uint64_t
BingoPrefetcher::hashShort(Addr pc, uint16_t off) const
{
    return mix64((uint64_t)pc ^ ((uint64_t)off << 1) ^ 0xBEEF1234ULL);
}

// page buffer ops
BingoPrefetcher::PageBufEntry*
BingoPrefetcher::findPageBuf(Addr page_tag)
{
    for (auto &e : pagebuf) {
        if (e.valid && e.page_tag == page_tag)
            return &e;
    }
    return nullptr;
}

void
BingoPrefetcher::commitPageBufToHistory(const PageBufEntry &pbe)
{
    const uint64_t long_tag = hashLong(pbe.trigger_pc, pbe.page_tag);
    const uint64_t short_h  = hashShort(pbe.trigger_pc, pbe.trigger_off);

    HistSet &set = histSetFor(short_h);

    int w = histFindLong(set, long_tag);
    if (w < 0) {
        w = histChooseVictim(set);
        set.ways[w] = HistEntry{};
        set.ways[w].valid = true;
    }

    set.ways[w].long_tag = long_tag;
    set.ways[w].pc = pbe.trigger_pc;
    set.ways[w].offset = pbe.trigger_off;
    set.ways[w].fp = pbe.fp;
    set.ways[w].lru = (uint32_t)(++tick);
}

BingoPrefetcher::PageBufEntry*
BingoPrefetcher::allocPageBuf(Addr page_tag, Addr pc, uint16_t off)
{
    // Try empty slot
    for (auto &e : pagebuf) {
        if (!e.valid) {
            e = PageBufEntry{};
            e.valid = true;
            e.page_tag = page_tag;
            e.trigger_pc = pc;
            e.trigger_off = off;
            e.lru = ++tick;
            stats.pagebuf_inserts++;
            return &e;
        }
    }

    // Evict LRU (capacity eviction)
    int victim = 0;
    for (int i = 1; i < (int)pagebuf.size(); i++) {
        if (pagebuf[i].lru < pagebuf[victim].lru)
            victim = i;
    }

    // committing pagebuf on capacity pressure (not true eviction-driven)
    commitPageBufToHistory(pagebuf[victim]);
    stats.pagebuf_evictions++;

    pagebuf[victim] = PageBufEntry{};
    pagebuf[victim].valid = true;
    pagebuf[victim].page_tag = page_tag;
    pagebuf[victim].trigger_pc = pc;
    pagebuf[victim].trigger_off = off;
    pagebuf[victim].lru = ++tick;

    stats.pagebuf_inserts++;
    return &pagebuf[victim];
}

// history ops 
BingoPrefetcher::HistSet&
BingoPrefetcher::histSetFor(uint64_t short_hash)
{
    const unsigned num_sets = history.size();
    return history[(unsigned)(short_hash % num_sets)];
}

void
BingoPrefetcher::histTouchLRU(HistSet &set, int hit_way)
{
    set.ways[hit_way].lru = (uint32_t)(++tick);
}

int
BingoPrefetcher::histFindLong(const HistSet &set, uint64_t long_tag) const
{
    for (int w = 0; w < (int)set.ways.size(); w++) {
        if (set.ways[w].valid && set.ways[w].long_tag == long_tag)
            return w;
    }
    return -1;
}

std::vector<int>
BingoPrefetcher::histFindShortMatches(const HistSet &set, Addr pc,
                                      uint16_t off) const
{
    std::vector<int> hits;
    for (int w = 0; w < (int)set.ways.size(); w++) {
        const auto &e = set.ways[w];
        if (e.valid && e.pc == pc && e.offset == off)
            hits.push_back(w);
    }
    return hits;
}

int
BingoPrefetcher::histChooseVictim(const HistSet &set) const
{
    for (int w = 0; w < (int)set.ways.size(); w++) {
        if (!set.ways[w].valid) return w;
    }
    int victim = 0;
    for (int w = 1; w < (int)set.ways.size(); w++) {
        if (set.ways[w].lru < set.ways[victim].lru)
            victim = w;
    }
    return victim;
}

// eviction-driven residency commit 
void
BingoPrefetcher::commitResidencyFootprint(Addr page_tag, const PageState &ps)
{
    if (!ps.hasTrigger) {
        return;
    }

    const uint64_t long_tag = hashLong(ps.triggerPC, page_tag);
    const uint64_t short_h  = hashShort(ps.triggerPC, ps.triggerOff);

    HistSet &set = histSetFor(short_h);

    int w = histFindLong(set, long_tag);
    if (w < 0) {
        w = histChooseVictim(set);
        set.ways[w] = HistEntry{};
        set.ways[w].valid = true;
    }

    set.ways[w].long_tag = long_tag;
    set.ways[w].pc = ps.triggerPC;
    set.ways[w].offset = ps.triggerOff;
    set.ways[w].fp = ps.fp;
    set.ways[w].lru = (uint32_t)(++tick);

    stats.pagebuf_commits++;
}

// notifyFill / notifyEvict
void
BingoPrefetcher::notifyFill(const CacheAccessProbeArg &arg)
{
    if (!arg.pkt || !arg.pkt->req)
        return;

    const Addr a = probeAddr(arg.pkt);
    const Addr blk = blockAddress(a);
    const Addr ptag = pageTag(blk);
    const uint16_t off = pageOffset(blk);
    const Addr pc = arg.pkt->req->hasPC() ? arg.pkt->req->getPC() : 0;

    // Residency accounting
    pageLines[ptag]++;

    PageState &ps = residency[ptag];
    if (!ps.hasTrigger) {
        ps.hasTrigger = true;
        ps.triggerPC = pc;
        ps.triggerOff = off;
    }
    ps.fp.set(off);
}

void
BingoPrefetcher::notifyEvict(const CacheDataUpdateProbeArg &info)
{
    const Addr a = info.addr;
    const Addr blk = blockAddress(a);
    const Addr ptag = pageTag(blk);

    auto it = pageLines.find(ptag);
    if (it == pageLines.end()) {
        return;
    }

    if (it->second > 0) {
        it->second--;
    }

    if (it->second == 0) {
        // True residency end for this page: commit footprint to history
        auto ps_it = residency.find(ptag);
        if (ps_it != residency.end()) {
            commitResidencyFootprint(ptag, ps_it->second);
            residency.erase(ps_it);
        }
        pageLines.erase(it);
    }
}

// main
void
BingoPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const CacheAccessor &cache)
{
    stats.lookups++;
    ++tick;

    // Trigger address & PC
    const Addr addr = pfi.getAddr();
    const Addr blk  = blockAddress(addr);
    const Addr pc   = pfi.hasPC() ? pfi.getPC() : 0;

    const Addr ptag = pageTag(blk);
    const uint16_t off = pageOffset(blk);

    // 1) Update page buffer footprint
    PageBufEntry *pbe = findPageBuf(ptag);
    if (pbe) {
        stats.pagebuf_hits++;
        pbe->lru = tick;
    } else {
        pbe = allocPageBuf(ptag, pc, off);
    }
    pbe->fp.set(off);

    // 2) Lookup history table and generate a predicted footprint
    const uint64_t long_tag = hashLong(pc, ptag);
    const uint64_t short_h  = hashShort(pc, off);

    HistSet &set = histSetFor(short_h);

    Footprint pred{};
    int long_hit = histFindLong(set, long_tag);
    if (long_hit >= 0) {
        stats.hist_long_hits++;
        pred = set.ways[long_hit].fp;
        histTouchLRU(set, long_hit);
    } else {
        auto short_hits = histFindShortMatches(set, pc, off);
        if (short_hits.empty()) {
            stats.hist_misses++;
            return;
        }
        stats.hist_short_hits++;

        const int m = (int)short_hits.size();
        const int thr = (int)std::ceil((votePercent / 100.0) * m);

        std::array<int, BlocksPerPage> votes{};
        votes.fill(0);

        for (int w : short_hits) {
            histTouchLRU(set, w);
            const auto &fp = set.ways[w].fp;
            for (int b = 0; b < BlocksPerPage; b++) {
                if (fp.test(b)) votes[b]++;
            }
        }

        for (int b = 0; b < BlocksPerPage; b++) {
            if (votes[b] >= thr) pred.set(b);
        }
    }

    // 3) Emit prefetches from predicted footprint
    pred.reset(off);

    unsigned emitted = 0;
    for (int b = 0; b < BlocksPerPage && emitted < degree; b++) {
        if (!pred.test(b)) continue;

        const Addr cand = (ptag << PageBits) + (Addr(b) * Addr(blkSize));

        if (enforceSamePage && !samePageAddr(blk, cand)) {
            stats.dropped_same_page++;
            continue;
        }

        if (cache.inCache(cand, pfi.isSecure())) {
            continue;
        }

        bool dup = false;
        for (const auto &ap : addresses) {
            if (ap.first == cand) { dup = true; break; }
        }
        if (dup) {
            stats.dropped_dup++;
            continue;
        }

        const int prio = 0;
        addresses.emplace_back(cand, prio);
        emitted++;
        stats.issued++;
    }
}

} // namespace prefetch
} // namespace gem5
