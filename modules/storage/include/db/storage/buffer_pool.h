#pragma once

/// @file
/// The buffer pool (§M1.6): B frames of 4096 bytes in memory, shared by
/// every file. It keeps track of which page is in which frame, how many
/// users each page has (pin count), which pages were changed (dirty), and
/// counts hits, misses, evictions, reads and writes exactly.
///
/// Every page access in the whole system goes through this pool, and
/// nothing else keeps copies of pages. So "at most B pages in memory" is
/// true simply because only B frames exist.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "db/storage/file_registry.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/replacement_policy.h"

namespace db::storage
{
    /// Declared here, defined in the private buffer/replacer.h. The pool
    /// only stores a pointer to it, so other modules never see the LRU and
    /// CLOCK classes.
    class Replacer;

    /// What newPage gives back: the new page's number and its bytes.
    struct NewPage
    {
        PageId pageId = kInvalidPageId;
        /// Usable only while the page stays pinned (same rule as fetchPage).
        std::span<std::byte> bytes;
    };

    /// One pool for all files. A page is named by (FileId, PageId).
    ///
    /// The spans that fetchPage and newPage return point straight into a
    /// frame. Use them only while the page is pinned. After the last unpin,
    /// the pool may put a different page into that frame at any moment.
    ///
    /// Errors (all std::logic_error, naming the file and page, D1):
    /// - no unpinned frame is available;
    /// - unpin of a page that is not in memory, or whose pin count is 0;
    /// - an unknown FileId, or a page number past the end of the file;
    /// - discardFile while one of the file's pages is pinned.
    /// A call that fails changes nothing: no frame, no counter, no file size.
    ///
    /// The pool cannot be copied or moved, because other objects hold
    /// references and spans into it. Create it where it lives, or keep it in
    /// a unique_ptr (the storage engine does, chunk 12).
    /// The destructor does not save changed pages: must call flushAll first.
    class BufferPool
    {
    public:
        /// Creates `frames` empty frames, once (at least 1, else
        /// std::logic_error), and a replacer for `policy`.
        /// `files` must stay alive longer than the pool.
        BufferPool(std::size_t frames, ReplacementPolicy policy, FileRegistry& files);
        ~BufferPool();

        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&&) = delete;
        BufferPool& operator=(BufferPool&&) = delete;

        /// Gives you the page's 4096 bytes and pins it (pin count + 1).
        /// - Already in memory: a HIT, no disk access.
        /// - Not in memory: a MISS. The pool takes an empty frame (lowest
        ///   number first), or else asks the replacer for a victim (an
        ///   EVICTION). A changed victim is saved to disk first (one WRITE).
        ///   Then the page is read from disk (one READ).
        /// `pageId` must be smaller than the file's page count.
        std::span<std::byte> fetchPage(FileId fileId, PageId pageId);

        /// Gives back one pin. Pass `isDirty = true` if you changed the
        /// bytes. Dirty is "sticky": once a page is marked changed, a later
        /// unpin with false does not undo it, until the page is saved.
        /// No disk access, no counter changes.
        void unpinPage(FileId fileId, PageId pageId, bool isDirty);

        /// Adds a new, all-zero page at the end of the file and puts it in
        /// a frame, pinned once and marked dirty. Nothing is read from disk,
        /// so it counts no hit, miss or eviction, even if it threw out
        /// another page (chunk 05 Q2). Saving a changed victim still counts
        /// one WRITE. If no frame is free it throws before the file grows.
        NewPage newPage(FileId fileId);

        /// Saves page (fileId, pageId) to disk if it is in memory and dirty
        /// (one WRITE), and marks it clean. Otherwise does nothing.
        /// Pin counts do not change.
        void flushPage(FileId fileId, PageId pageId);

        /// flushPage for every frame, in frame order, pinned or not.
        /// Nothing is evicted. Calling it again right away writes nothing.
        void flushAll();

        /// Throws away every page of `fileId` that is in memory, without
        /// saving it, even if it was changed (used for temporary files,
        /// chunk 10). Those frames become empty, so reusing one later is not
        /// an eviction. If any of the file's pages is pinned, it throws
        /// std::logic_error and changes nothing.
        void discardFile(FileId fileId);

        /// True if page (fileId, pageId) is in a frame right now.
        /// Counts nothing.
        bool isResident(FileId fileId, PageId pageId) const;

        /// How many pages file `fileId` has right now (newPage adds one).
        /// No disk access, counts nothing. The heap file uses it to check a
        /// rid and to count its data pages. Unknown FileId -> std::logic_error.
        PageId numPages(FileId fileId) const;

        /// Takes `count` frames out of the pool for memory that is not a page,
        /// e.g. tuples that exec keeps in a C++ vector (exec D2: such memory
        /// counts against B, §M2.1). While reserved, a frame holds no page,
        /// and no page can be loaded into it. So pages in frames + reserved
        /// frames is never more than B.
        /// If a frame still holds an unpinned page, that page is thrown out
        /// to make room (saved first if dirty: one WRITE). This is not a
        /// miss, so it counts no eviction.
        /// A reserved frame counts in pinnedFrameCount() and the peak, so the
        /// peak is the most frames held at once, pages and reserved together.
        /// Fewer than `count` frames free of pins -> std::logic_error, and
        /// nothing changes.
        void reserveFrames(std::size_t count);

        /// Gives `count` reserved frames back to the pool; they become empty
        /// frames. More than are reserved -> std::logic_error, nothing changes.
        void releaseFrames(std::size_t count);

        /// A copy of the counters: a snapshot that later activity does not change.
        IoStats stats() const;
        /// The number of frames, B.
        std::size_t frameCount() const;
        /// How many frames are in use right now: pages with a pin count
        /// above 0, plus reserved frames.
        std::size_t pinnedFrameCount() const;
        /// The highest pinnedFrameCount() seen since the pool was made or
        /// since the last resetPeak(). Chunk 09 uses it to check that a scan
        /// never holds more than 2 pages; exec uses it to show the most
        /// frames a plan held at once.
        std::size_t peakPinnedCount() const;
        /// Starts a new peak measurement from the current pinnedFrameCount().
        void resetPeak();

    private:
        /// One frame: room for one page, plus what the pool knows about it.
        struct Frame
        {
            std::array<std::byte, kPageSize> bytes{};
            FileId fileId = 0;
            PageId pageId = kInvalidPageId;
            /// true = the bytes were changed and the disk copy is out of date.
            bool dirty = false;
            /// false = the frame holds no page (never used, or discarded).
            bool inUse = false;
            /// true = taken by reserveFrames: holds no page and must not
            /// get one until releaseFrames gives it back.
            bool reserved = false;
        };

        /// The frame takeFrame found for a new page, and whether another
        /// page had to be thrown out to get it. Only that second case is an
        /// eviction.
        struct TakenFrame
        {
            std::size_t index = 0;
            bool replacedPage = false;
        };

        /// (file, page): how the page table names a page.
        using PageKey = std::pair<FileId, PageId>;

        /// The frame holding page (fileId, pageId), or nothing if it is not in memory.
        std::optional<std::size_t> findFrame(FileId fileId, PageId pageId) const;
        /// Finds a frame for a new page: the lowest empty frame, or else the
        /// replacer's victim (saved first if dirty, and removed from the
        /// page table). If every frame is pinned it throws std::logic_error
        /// naming (fileId, pageId), the page that wanted a frame.
        TakenFrame takeFrame(FileId fileId, PageId pageId);
        /// Adds one pin. When a frame goes from 0 to 1 pin, it also updates
        /// the pinned-frame count and the peak.
        void pin(std::size_t frame);
        /// Saves a frame's page to its file (one WRITE) and marks it clean.
        void writeBack(std::size_t frame);

        FileRegistry& files_;
        /// The B frames. Sized once in the constructor and never resized:
        /// resizing could move the bytes in memory and break every span the
        /// pool has handed out.
        std::vector<Frame> frames_;
        /// pinCounts_[i] is frame i's pin count. Kept in its own vector so
        /// the replacer can be handed a read-only view of all of them at once.
        std::vector<std::uint32_t> pinCounts_;
        /// Page table: (file, page) -> frame number, for pages in memory only.
        std::map<PageKey, std::size_t> pageTable_;
        std::unique_ptr<Replacer> replacer_;
        IoStats stats_;
        /// Frames with pin count > 0 right now (reserved frames included:
        /// each has pin count 1, so the replacer never picks it).
        std::size_t pinnedFrames_ = 0;
        /// Frames taken by reserveFrames right now.
        std::size_t reservedFrames_ = 0;
        /// The highest pinnedFrames_ since construction or resetPeak().
        std::size_t peakPinned_ = 0;
    };
} // namespace db::storage
