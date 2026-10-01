class BingoPrefetcher(QueuedPrefetcher):
    type = "BingoPrefetcher"
    cxx_class = "gem5::prefetch::BingoPrefetcher"
    cxx_header = "mem/cache/prefetch/bingo.hh"

    region_size = Param.MemorySize("2KiB", "Spatial region size")
    ft_entries = Param.Unsigned(64, "Filter table entries")
    at_entries = Param.Unsigned(128, "Accumulation table entries")
    history_entries = Param.Unsigned(
        16384, "History table entries (16K in the paper)"
    )
    history_assoc = Param.Unsigned(16, "History table associativity")
    vote_percent = Param.Unsigned(
        20, "A block is predicted if at least this percent of PC+Offset "
        "matches contain it"
    )
    max_prefetches = Param.Unsigned(
        0, "Cap on prefetches per trigger (0 = whole footprint)"
    )
    use_long_event = Param.Bool(
        True, "Look up PC+Address (disable only for ablation studies)"
    )
    use_short_event = Param.Bool(
        True, "Fall back to PC+Offset voting (disable only for ablation "
        "studies)"
    )
