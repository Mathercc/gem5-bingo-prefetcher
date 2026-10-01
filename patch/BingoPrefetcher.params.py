class BingoPrefetcher(QueuedPrefetcher):
    type = 'BingoPrefetcher'
    cxx_class = 'gem5::prefetch::BingoPrefetcher'
    cxx_header = "mem/cache/prefetch/bingo.hh"

    degree = Param.Unsigned(4, "Max prefetches per trigger")
    history_entries = Param.Unsigned(16384, "Total history entries (paper uses 16K)")  # :contentReference[oaicite:12]{index=12}
    history_assoc   = Param.Unsigned(16, "History table associativity")
    page_buf_entries = Param.Unsigned(64, "Page buffer entries (PPH residency tracking)")
    vote_percent = Param.Unsigned(20, "Voting threshold percent for short-event multi-match") # :contentReference[oaicite:13]{index=13}
