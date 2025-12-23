#ifndef __MEM_CACHE_PREFETCH_BINGO_HH__
#define __MEM_CACHE_PREFETCH_BINGO_HH__

#include <array>
#include <bitset>
#include <cstdint>
#include <unordered_map>
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

    // These are invoked via probe listeners registered in Base::regProbeListeners()
    void notifyFill(const CacheAccessProbeArg &arg) override;
    void notifyEvict(const CacheDataUpdateProbeArg &info) override;

  private:
    // Geometry (paper uses 4KB page, 64B line => 64 blocks/page)
    static constexpr int PageBits = 12;      // 4KB
    static constexpr int BlocksPerPage = 64; // 4096/64 (assumes 64B lines)
    using Footprint = std::bitset<BlocksPerPage>;

    // History table (set-assoc)
    struct HistEntry
    {
        bool valid = false;

        uint64_t long_tag = 0; // hash(PC, page_tag)
        Addr pc = 0;           // for short match
        uint16_t offset = 0;   // for short match (0..63)

        Footprint fp{};
        uint32_t lru = 0;
    };

    struct HistSet
    {
        std::vector<HistEntry> ways;
        explicit HistSet(unsigned assoc) : ways(assoc) {}
    };

    // Page buffer (optional: record current page footprints; also acts as a small cache)
    struct PageBufEntry
    {
        bool valid = false;
        Addr page_tag = 0;

        Addr trigger_pc = 0;
        uint16_t trigger_off = 0;

        Footprint fp{};
        uint64_t lru = 0;
    };

    // Per-page residency tracking (for eviction-driven commit)
    struct PageState
    {
        bool hasTrigger = false;
        Addr triggerPC = 0;
        uint16_t triggerOff = 0;
        Footprint fp{};
    };

    // Stats
    struct BingoStats : public statistics::Group
    {
        BingoStats(statistics::Group *parent);

        statistics::Scalar lookups;

        statistics::Scalar hist_long_hits;
        statistics::Scalar hist_short_hits;
        statistics::Scalar hist_misses;

        statistics::Scalar pagebuf_hits;
        statistics::Scalar pagebuf_inserts;
        statistics::Scalar pagebuf_evictions;  // pagebuf capacity evictions
        statistics::Scalar pagebuf_commits;    // committed due to residency end (true eviction)

        statistics::Scalar issued;
        statistics::Scalar dropped_dup;
        statistics::Scalar dropped_same_page;
    };

    // Helpers
    Addr pageTag(Addr a) const { return a >> PageBits; }

    // offset in cache lines within the 4KB page
    uint16_t pageOffset(Addr a) const
    {
        const Addr in_page = a & ((Addr(1) << PageBits) - 1);
        return (uint16_t)((in_page / blkSize) & (BlocksPerPage - 1));
    }

    bool samePageAddr(Addr a, Addr b) const
    {
        return pageTag(a) == pageTag(b);
    }

    uint64_t mix64(uint64_t x) const;
    uint64_t hashLong(Addr pc, Addr page_tag) const;
    uint64_t hashShort(Addr pc, uint16_t off) const;

    // Page buffer ops
    PageBufEntry* findPageBuf(Addr page_tag);
    PageBufEntry* allocPageBuf(Addr page_tag, Addr pc, uint16_t off);
    void commitPageBufToHistory(const PageBufEntry &pbe); // (pagebuf eviction-based)

    // History ops
    HistSet& histSetFor(uint64_t short_hash);
    void histTouchLRU(HistSet &set, int hit_way);
    int histFindLong(const HistSet &set, uint64_t long_tag) const;
    std::vector<int> histFindShortMatches(const HistSet &set, Addr pc,
                                          uint16_t off) const;
    int histChooseVictim(const HistSet &set) const;

    // Residency commit (eviction-driven)
    void commitResidencyFootprint(Addr page_tag, const PageState &ps);

    // Address selection for probes
    Addr probeAddr(const PacketPtr &pkt) const;

  private:
    // knobs
    const unsigned degree;
    const bool enforceSamePage;
    const unsigned historyEntries;
    const unsigned historyAssoc;
    const unsigned pageBufEntries;
    const unsigned votePercent; // 20 means 20%

    BingoStats stats;

    // structures
    std::vector<HistSet> history;       // num_sets = historyEntries / historyAssoc
    std::vector<PageBufEntry> pagebuf;  // small fully-assoc
    mutable uint64_t tick = 0;          // local timestamp for LRU

    // page_tag -> number of cache lines currently resident in L2
    std::unordered_map<Addr, int> pageLines;

    // page_tag -> footprint accumulated while resident
    std::unordered_map<Addr, PageState> residency;
};

} // namespace prefetch
} // namespace gem5

#endif 
