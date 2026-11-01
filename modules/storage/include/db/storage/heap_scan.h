#pragma once

/// @file
/// The heap scan of §M1.5: walks a heap file and hands out its live records
/// one at a time, in directory order, then slot order.
///
/// ```
/// scan() -> iterator of (rid, record)
///     for each directory page:
///       for each entry:
///         fetch data page
///         for each live slot: yield
///         unpin data page before moving on
/// ```
/// Pins: **one page at a time**. §M1.5 allows at most one directory page and
/// one data page; exec's seqscan has a budget of 1 frame (§M2.3), so the
/// scan never holds both. Before each data page it pins the directory page,
/// reads the next data page id, and unpins it again. The scan keeps only
/// *where it is* (the directory page number, one guard and two counters),
/// never a list of rows or pages: a scan that read the table into a list
/// would hold more than B pages' worth of data and fail the module.
///
/// Cost of that choice: the directory page is fetched once per data page.
/// Under LRU it is always the most recently used page, so with B >= 2 it
/// stays resident and each fetch is a hit (every page is read once). Under
/// CLOCK at a tiny B, or at B = 1, it can be evicted and read again.

#include <cstdint>

#include "db/common/schema.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/page.h"
#include "db/storage/page_guard.h"
#include "db/storage/rid.h"

namespace db::storage
{
    /// Pull-style iterator: call next() until it returns false.
    ///
    /// ```
    /// HeapScan scan = heap.scan();
    /// Rid rid;
    /// Tuple tuple;
    /// while (scan.next(rid, tuple)) { ... }
    /// ```
    ///
    /// Lazy start: making a scan pins and reads nothing; the first next()
    /// fetches directory page 0. So an unused scan (or `scan --limit 0`)
    /// costs no I/O.
    ///
    /// Move-only, like PageGuard: it holds pins, and two copies would each
    /// release them. A moved-from scan is finished (next() returns false).
    ///
    /// The pool must outlive the scan.
    class HeapScan
    {
    public:
        /// Use HeapFile::scan() rather than calling this directly.
        /// `schema` is copied, so the scan does not depend on the HeapFile
        /// staying alive.
        HeapScan(BufferPool& pool, FileId fileId, Schema schema);

        /// Moves hand over the pins and the position; the source becomes
        /// finished. Written by hand (not defaulted) because a defaulted
        /// move would leave the source's position behind, and the source
        /// would then restart from page 0 or use its now-empty guards.
        HeapScan(HeapScan&& other) noexcept;
        HeapScan& operator=(HeapScan&& other) noexcept;
        HeapScan(const HeapScan&) = delete;
        HeapScan& operator=(const HeapScan&) = delete;
        /// The guards release any pins by themselves.
        ~HeapScan() = default;

        /// Fills `rid` and `tuple` with the next live record and returns
        /// true, or returns false when there are no more (and keeps
        /// returning false). The tuple owns its strings, so it stays valid
        /// after the scan moves on. If a page fetch throws, the scan
        /// releases its pins, becomes finished, and the exception passes on.
        bool next(Rid& rid, Tuple& tuple);

        /// Releases the pin now and finishes the scan; next() then
        /// returns false. Calling it again does nothing.
        void close();

    private:
        enum class State
        {
            NotStarted, // no page pinned yet (lazy start)
            Running,    // dirPageId_ names the current directory page
            Finished    // nothing pinned; next() returns false
        };

        /// Moves on to the next live record. Called by next() inside a
        /// try block, so any exception closes the scan.
        bool advance(Rid& rid, Tuple& tuple);

        BufferPool* pool_;
        FileId fileId_;
        Schema schema_;
        State state_ = State::NotStarted;

        /// The current directory page (its number only; it is not kept
        /// pinned) and the next entry on it to visit.
        PageId dirPageId_ = 0;
        std::uint16_t nextEntry_ = 0;
        /// The current data page (empty between pages) and the next slot on it to look at.
        PageGuard dataGuard_;
        std::uint16_t nextSlot_ = 0;
    };
} // namespace db::storage
