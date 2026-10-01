/**
 * Bingo spatial data prefetcher.
 *
 * M. Bakhshalipour, M. Shakerinava, P. Lotfi-Kamran, H. Sarbazi-Azad,
 * "Bingo Spatial Data Prefetcher", HPCA 2019.
 *
 * A region's footprint is recorded from its trigger access (the first
 * access to the region) until the end of its residency (the first eviction
 * or invalidation of one of its blocks). The footprint is stored in a single
 * history table tagged with the trigger's PC+Address and indexed by a hash of
 * its PC+Offset. On a trigger access the table is searched for PC+Address;
 * on a miss, every entry in the same set whose PC+Offset matches votes, and a
 * block is prefetched if it appears in at least vote_percent of them. All
 * predicted blocks are prefetched at once.
 */

#ifndef __MEM_CACHE_PREFETCH_BINGO_HH__
#define __MEM_CACHE_PREFETCH_BINGO_HH__

#include <bitset>
#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/BingoPrefetcher.hh"

namespace gem5
{
namespace prefetch
{

class BingoPrefetcher : public Queued
{
  public:
    using Params = BingoPrefetcherParams;
    BingoPrefetcher(const Params &p);

  protected:
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    void notifyEvict(const CacheDataUpdateProbeArg &info) override;

  private:
    static constexpr unsigned MaxBlocks = 64; // up to 4KiB regions of 64B
    using Footprint = std::bitset<MaxBlocks>;

    // Region seen once: waits for a second, different block (SMS filter).
    struct FilterEntry
    {
        bool valid = false;
        Addr region = 0;
        Addr pc = 0;
        unsigned offset = 0;
        uint64_t lru = 0;
    };

    // Region being recorded.
    struct AccumEntry
    {
        bool valid = false;
        Addr region = 0;
        Addr pc = 0;
        unsigned offset = 0;
        Footprint fp;
        uint64_t lru = 0;
    };

    // History entry: tagged with PC+Address, set chosen by PC+Offset.
    struct HistEntry
    {
        bool valid = false;
        Addr pc = 0;
        Addr region = 0;
        unsigned offset = 0;
        Footprint fp;
        uint64_t lru = 0;
    };

    struct BingoStats : public statistics::Group
    {
        BingoStats(statistics::Group *parent);

        statistics::Scalar triggers;
        statistics::Scalar long_hits;
        statistics::Scalar short_hits;
        statistics::Scalar hist_misses;
        statistics::Scalar commits_evict;
        statistics::Scalar commits_capacity;
        statistics::Scalar filter_drops;
        statistics::Scalar predicted;
        statistics::Scalar issued;
    };

    Addr regionOf(Addr a) const { return a >> regionShift; }
    unsigned offsetOf(Addr a) const
    {
        return (a >> lBlkSize) & (blocksPerRegion - 1);
    }
    Addr blockOf(Addr region, unsigned off) const
    {
        return (region << regionShift) + (Addr(off) << lBlkSize);
    }

    unsigned setIndex(Addr pc, unsigned offset) const;
    std::vector<HistEntry> &setFor(Addr pc, unsigned offset);

    template <typename Entry>
    static Entry *find(std::vector<Entry> &table, Addr region);
    template <typename Entry>
    static Entry &victim(std::vector<Entry> &table);

    void commit(const AccumEntry &ae);
    bool predict(Addr pc, Addr region, unsigned offset, Footprint &pred);

    const unsigned regionShift;
    const unsigned blocksPerRegion;
    const unsigned historySets;
    const unsigned votePercent;
    const unsigned maxPrefetches;
    const bool useLongEvent;
    const bool useShortEvent;

    std::vector<FilterEntry> filter;
    std::vector<AccumEntry> accum;
    std::vector<std::vector<HistEntry>> history;
    uint64_t tick = 0;

    BingoStats stats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_BINGO_HH__
