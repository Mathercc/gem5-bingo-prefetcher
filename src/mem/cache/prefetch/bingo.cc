#include "mem/cache/prefetch/bingo.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"

namespace gem5
{
namespace prefetch
{

BingoPrefetcher::BingoStats::BingoStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(triggers, statistics::units::Count::get(),
               "trigger accesses (first access to a region)"),
      ADD_STAT(long_hits, statistics::units::Count::get(),
               "triggers predicted from a PC+Address match"),
      ADD_STAT(short_hits, statistics::units::Count::get(),
               "triggers predicted by PC+Offset voting"),
      ADD_STAT(hist_misses, statistics::units::Count::get(),
               "triggers with no history match"),
      ADD_STAT(commits_evict, statistics::units::Count::get(),
               "footprints stored at the end of region residency"),
      ADD_STAT(commits_capacity, statistics::units::Count::get(),
               "footprints stored because the accumulation table was full"),
      ADD_STAT(filter_drops, statistics::units::Count::get(),
               "single-access regions dropped from the filter table"),
      ADD_STAT(predicted, statistics::units::Count::get(),
               "blocks predicted (before cache filtering)"),
      ADD_STAT(issued, statistics::units::Count::get(),
               "prefetch candidates handed to the queue")
{
}

BingoPrefetcher::BingoPrefetcher(const Params &p)
    : Queued(p),
      regionShift(floorLog2(p.region_size)),
      blocksPerRegion(p.region_size / p.block_size),
      historySets(p.history_entries / p.history_assoc),
      votePercent(p.vote_percent),
      maxPrefetches(p.max_prefetches),
      useLongEvent(p.use_long_event),
      useShortEvent(p.use_short_event),
      filter(p.ft_entries),
      accum(p.at_entries),
      history(p.history_entries / p.history_assoc,
              std::vector<HistEntry>(p.history_assoc)),
      stats(this)
{
    fatal_if(!isPowerOf2(p.region_size) || p.region_size < p.block_size,
             "Bingo: region_size must be a power of two >= block size");
    fatal_if(blocksPerRegion > MaxBlocks,
             "Bingo: at most %d blocks per region", MaxBlocks);
    fatal_if(historySets == 0 || p.history_entries % p.history_assoc,
             "Bingo: history_entries must be a multiple of history_assoc");
    fatal_if(!p.use_long_event && !p.use_short_event,
             "Bingo: at least one event must be enabled");
    fatal_if(p.ft_entries == 0 || p.at_entries == 0,
             "Bingo: filter and accumulation tables need entries");
}

unsigned
BingoPrefetcher::setIndex(Addr pc, unsigned offset) const
{
    uint64_t x = (uint64_t(pc) << 6) ^ offset;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x % historySets;
}

std::vector<BingoPrefetcher::HistEntry> &
BingoPrefetcher::setFor(Addr pc, unsigned offset)
{
    return history[setIndex(pc, offset)];
}

template <typename Entry>
Entry *
BingoPrefetcher::find(std::vector<Entry> &table, Addr region)
{
    for (auto &e : table) {
        if (e.valid && e.region == region)
            return &e;
    }
    return nullptr;
}

template <typename Entry>
Entry &
BingoPrefetcher::victim(std::vector<Entry> &table)
{
    Entry *v = &table[0];
    for (auto &e : table) {
        if (!e.valid)
            return e;
        if (e.lru < v->lru)
            v = &e;
    }
    return *v;
}

void
BingoPrefetcher::commit(const AccumEntry &ae)
{
    auto &set = setFor(ae.pc, ae.offset);

    HistEntry *slot = nullptr;
    for (auto &e : set) {
        if (e.valid && e.pc == ae.pc && e.region == ae.region &&
            e.offset == ae.offset) {
            slot = &e;
            break;
        }
    }
    if (!slot)
        slot = &victim(set);

    slot->valid = true;
    slot->pc = ae.pc;
    slot->region = ae.region;
    slot->offset = ae.offset;
    slot->fp = ae.fp;
    slot->lru = ++tick;
}

bool
BingoPrefetcher::predict(Addr pc, Addr region, unsigned offset,
                         Footprint &pred)
{
    auto &set = setFor(pc, offset);

    // Long event: PC+Address.
    for (auto &e : set) {
        if (!useLongEvent)
            break;
        if (e.valid && e.pc == pc && e.region == region &&
            e.offset == offset) {
            e.lru = ++tick;
            pred = e.fp;
            stats.long_hits++;
            return true;
        }
    }

    // Short event: PC+Offset, all matches in the same set vote.
    unsigned votes[MaxBlocks] = {};
    unsigned matches = 0;
    for (auto &e : set) {
        if (useShortEvent && e.valid && e.pc == pc && e.offset == offset) {
            e.lru = ++tick;
            matches++;
            for (unsigned b = 0; b < blocksPerRegion; b++) {
                if (e.fp.test(b))
                    votes[b]++;
            }
        }
    }
    if (matches == 0) {
        stats.hist_misses++;
        return false;
    }

    stats.short_hits++;
    pred.reset();
    for (unsigned b = 0; b < blocksPerRegion; b++) {
        // votes/matches >= votePercent/100, without rounding
        if (votes[b] * 100 >= votePercent * matches)
            pred.set(b);
    }
    return true;
}

void
BingoPrefetcher::notifyEvict(const CacheDataUpdateProbeArg &info)
{
    // The first eviction of any block in a region ends its generation.
    const Addr region = regionOf(info.addr);
    DPRINTF(HWPrefetch, "Bingo: evict %#x\n", info.addr);

    if (AccumEntry *ae = find(accum, region)) {
        commit(*ae);
        stats.commits_evict++;
        ae->valid = false;
    }
    if (FilterEntry *fe = find(filter, region))
        fe->valid = false;
}

void
BingoPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses,
                                   const CacheAccessor &cache)
{
    if (!pfi.hasPC())
        return;

    const Addr pc = pfi.getPC();
    const Addr region = regionOf(pfi.getAddr());
    const unsigned offset = offsetOf(pfi.getAddr());
    DPRINTF(HWPrefetch, "Bingo: access pc=%#x addr=%#x\n", pc,
            pfi.getAddr());

    // Region already being recorded: just extend its footprint.
    if (AccumEntry *ae = find(accum, region)) {
        ae->fp.set(offset);
        ae->lru = ++tick;
        return;
    }

    // Second distinct block of a filtered region: start accumulating.
    if (FilterEntry *fe = find(filter, region)) {
        if (fe->offset != offset) {
            AccumEntry &slot = victim(accum);
            if (slot.valid) {
                commit(slot);
                stats.commits_capacity++;
            }
            slot.valid = true;
            slot.region = region;
            slot.pc = fe->pc;
            slot.offset = fe->offset;
            slot.fp.reset();
            slot.fp.set(fe->offset);
            slot.fp.set(offset);
            slot.lru = ++tick;
            fe->valid = false;
        }
        return;
    }

    // Trigger access.
    stats.triggers++;
    FilterEntry &fslot = victim(filter);
    if (fslot.valid)
        stats.filter_drops++;
    fslot.valid = true;
    fslot.region = region;
    fslot.pc = pc;
    fslot.offset = offset;
    fslot.lru = ++tick;

    Footprint pred;
    if (!predict(pc, region, offset, pred)) {
        DPRINTF(HWPrefetch, "Bingo: trigger %#x none\n", pfi.getAddr());
        return;
    }
    pred.reset(offset);
    DPRINTF(HWPrefetch, "Bingo: trigger %#x pred=%#llx\n", pfi.getAddr(),
            (unsigned long long)pred.to_ullong());

    // Issue the whole footprint at once, nearest blocks after the trigger
    // first so the ones needed soonest are queued first.
    unsigned emitted = 0;
    for (unsigned i = 1; i < blocksPerRegion; i++) {
        const unsigned b = (offset + i) & (blocksPerRegion - 1);
        if (!pred.test(b))
            continue;
        stats.predicted++;
        const Addr addr = blockOf(region, b);
        if (cache.inCache(addr, pfi.isSecure()))
            continue;
        addresses.emplace_back(addr, 0);
        stats.issued++;
        if (maxPrefetches && ++emitted >= maxPrefetches)
            break;
    }
}

} // namespace prefetch
} // namespace gem5
