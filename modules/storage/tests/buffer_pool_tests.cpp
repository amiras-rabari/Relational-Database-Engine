// FileRegistry and BufferPool: the SPEC M1.6 trace through the real pool for
// both policies, the required "evicting a pinned page fails" test, pin/unpin
// errors, dirty write-back, newPage counting (chunk 05 Q2), flush, two files
// with the same page id, discardFile and pin accounting.
//
// Test names start with "registry:" or "pool:" so a group can be run alone,
// e.g. storage_tests -tc="registry:*".

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/disk_manager.h"
#include "db/storage/file_registry.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/replacement_policy.h"

namespace
{

    namespace fs = std::filesystem;
    using db::storage::BufferPool;
    using db::storage::DiskManager;
    using db::storage::FileId;
    using db::storage::FileRegistry;
    using db::storage::IoStats;
    using db::storage::kPageSize;
    using db::storage::PageId;
    using db::storage::ReplacementPolicy;
    using Page = std::array<std::byte, kPageSize>;

    // A fresh folder under the system temp directory, removed when the test
    // starts and again when it ends. Each test case uses its own name.
    class TempDir
    {
    public:
        explicit TempDir(const std::string& name)
            : path_(fs::temp_directory_path() / "comp3005_storage_tests" / name)
        {
            fs::remove_all(path_);
            fs::create_directories(path_);
        }
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        const fs::path& path() const { return path_; }

    private:
        fs::path path_;
    };

    // A temp folder, a registry and one file "t.db" with `pages` zero pages.
    // Members are created in this order (dir before files before file).
    struct Fixture
    {
        TempDir dir;
        FileRegistry files;
        FileId file;

        explicit Fixture(const std::string& name, PageId pages = 8)
            : dir(name), file(files.create(dir.path() / "t.db"))
        {
            for (PageId i = 0; i < pages; ++i)
                files.disk(file).allocatePage();
        }

        // Page `pageId` as it is on disk now (counts a read on the disk
        // manager, not on the pool).
        Page onDisk(FileId id, PageId pageId)
        {
            Page page{};
            files.disk(id).readPage(pageId, page);
            return page;
        }
    };

    bool allZero(std::span<const std::byte> bytes)
    {
        return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
    }

    // Each page fetched and unpinned clean at once (the SPEC M1.6 setup).
    void runTrace(BufferPool& pool, FileId file, const std::vector<PageId>& trace)
    {
        for (PageId page : trace)
        {
            pool.fetchPage(file, page);
            pool.unpinPage(file, page, false);
        }
    }

    const std::vector<PageId> kSpecTrace{1, 2, 3, 1, 4, 2};

} // namespace

// ---------------------------------------------------------------- registry

TEST_CASE("registry: create and open hand out ids 0, 1, 2 and the same path keeps its id")
{
    TempDir dir("registry_ids");
    FileRegistry files;
    const FileId a = files.create(dir.path() / "a.db");
    const FileId b = files.create(dir.path() / "b.db");
    CHECK(a == 0);
    CHECK(b == 1);
    CHECK(files.contains(a));
    CHECK(files.disk(b).path() == dir.path() / "b.db");

    // Already open: the same id, not a second disk manager.
    CHECK(files.open(dir.path() / "a.db") == a);
    CHECK(files.open(dir.path() / "." / "a.db") == a);

    // An existing file that is not open yet gets the next id.
    DiskManager::create(dir.path() / "c.db");
    CHECK(files.open(dir.path() / "c.db") == 2);
}

TEST_CASE("registry: errors")
{
    TempDir dir("registry_errors");
    FileRegistry files;
    const FileId a = files.create(dir.path() / "a.db");

    CHECK_THROWS_AS(files.create(dir.path() / "a.db"), db::Error); // exists
    CHECK_THROWS_AS(files.open(dir.path() / "missing.db"), db::Error);
    CHECK_FALSE(files.contains(a + 1));
    CHECK_THROWS_AS(files.disk(a + 1), std::logic_error);
    CHECK_THROWS_AS(files.remove(a + 1), std::logic_error);
}

TEST_CASE("registry: remove closes and deletes the file, and the id is not reused")
{
    TempDir dir("registry_remove");
    FileRegistry files;
    const FileId a = files.create(dir.path() / "a.db");
    files.remove(a);
    CHECK_FALSE(files.contains(a));
    CHECK_FALSE(fs::exists(dir.path() / "a.db"));
    CHECK_THROWS_AS(files.disk(a), std::logic_error);
    CHECK(files.create(dir.path() / "a.db") != a);
}

// -------------------------------------------------------------------- pool

TEST_CASE("pool: a new pool is empty")
{
    Fixture fx("pool_new");
    CHECK_THROWS_AS(BufferPool(0, ReplacementPolicy::Lru, fx.files), std::logic_error);

    BufferPool pool(3, ReplacementPolicy::Lru, fx.files);
    CHECK(pool.frameCount() == 3);
    CHECK(pool.stats() == IoStats{});
    CHECK(pool.pinnedFrameCount() == 0);
    CHECK(pool.peakPinnedCount() == 0);
    CHECK_FALSE(pool.isResident(fx.file, 0));
}

TEST_CASE("pool: fetchPage returns the page's bytes from disk")
{
    Fixture fx("pool_bytes");
    Page page{};
    for (std::size_t i = 0; i < page.size(); ++i)
        page[i] = static_cast<std::byte>((i * 7 + 3) % 251);
    fx.files.disk(fx.file).writePage(3, page);

    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    const auto bytes = pool.fetchPage(fx.file, 3);
    REQUIRE(bytes.size() == kPageSize);
    CHECK(std::equal(bytes.begin(), bytes.end(), page.begin()));
    CHECK(pool.isResident(fx.file, 3));
}

TEST_CASE("pool: LRU reproduces the SPEC M1.6 trace")
{
    Fixture fx("pool_trace_lru");
    BufferPool pool(3, ReplacementPolicy::Lru, fx.files);
    runTrace(pool, fx.file, kSpecTrace);
    // hits, misses, evictions, reads, writes. Reads = misses: all clean.
    CHECK(pool.stats() == IoStats{1, 5, 2, 5, 0});
    // Final frames [1, 4, 2]: 3 was evicted last.
    CHECK(pool.isResident(fx.file, 1));
    CHECK(pool.isResident(fx.file, 4));
    CHECK(pool.isResident(fx.file, 2));
    CHECK_FALSE(pool.isResident(fx.file, 3));
}

TEST_CASE("pool: CLOCK reproduces the SPEC M1.6 trace")
{
    Fixture fx("pool_trace_clock");
    BufferPool pool(3, ReplacementPolicy::Clock, fx.files);
    runTrace(pool, fx.file, kSpecTrace);
    CHECK(pool.stats() == IoStats{2, 4, 1, 4, 0});
    // Final frames [4, 2, 3]: 1 was the only victim.
    CHECK(pool.isResident(fx.file, 4));
    CHECK(pool.isResident(fx.file, 2));
    CHECK(pool.isResident(fx.file, 3));
    CHECK_FALSE(pool.isResident(fx.file, 1));
}

TEST_CASE("pool: the same trace in two fresh pools gives identical counters")
{
    Fixture fx("pool_determinism");
    for (ReplacementPolicy policy : {ReplacementPolicy::Lru, ReplacementPolicy::Clock})
    {
        BufferPool first(3, policy, fx.files);
        BufferPool second(3, policy, fx.files);
        runTrace(first, fx.file, kSpecTrace);
        runTrace(second, fx.file, kSpecTrace);
        CHECK(first.stats() == second.stats());
    }
}

TEST_CASE("pool: evicting a pinned page fails and changes nothing (SPEC M1.11 required test)")
{
    for (ReplacementPolicy policy : {ReplacementPolicy::Lru, ReplacementPolicy::Clock})
    {
        Fixture fx("pool_pinned_eviction");
        BufferPool pool(2, policy, fx.files);
        pool.fetchPage(fx.file, 0);
        pool.fetchPage(fx.file, 1); // both stay pinned
        const IoStats before = pool.stats();
        REQUIRE(before == IoStats{0, 2, 0, 2, 0});

        CHECK_THROWS_AS(pool.fetchPage(fx.file, 2), std::logic_error);
        CHECK(pool.stats() == before);
        CHECK(pool.isResident(fx.file, 0));
        CHECK(pool.isResident(fx.file, 1));
        CHECK_FALSE(pool.isResident(fx.file, 2));
        CHECK(pool.pinnedFrameCount() == 2);

        // Once a page is unpinned, the fetch works and evicts it.
        pool.unpinPage(fx.file, 0, false);
        pool.fetchPage(fx.file, 2);
        CHECK(pool.stats() == IoStats{0, 3, 1, 3, 0});
        CHECK_FALSE(pool.isResident(fx.file, 0));
    }
}

TEST_CASE("pool: bad fetches throw and change nothing")
{
    Fixture fx("pool_bad_fetch"); // pages 0..7
    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    CHECK_THROWS_AS(pool.fetchPage(fx.file, 8), std::logic_error);      // past the end
    CHECK_THROWS_AS(pool.fetchPage(fx.file + 1, 0), std::logic_error);  // unknown file
    CHECK(pool.stats() == IoStats{});
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("pool: unpin errors (SPEC M1.8)")
{
    Fixture fx("pool_unpin_errors");
    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    CHECK_THROWS_AS(pool.unpinPage(fx.file, 0, false), std::logic_error); // not resident
    pool.fetchPage(fx.file, 0);
    pool.unpinPage(fx.file, 0, false);
    CHECK_THROWS_AS(pool.unpinPage(fx.file, 0, false), std::logic_error); // pin count 0
}

TEST_CASE("pool: pin twice, unpin twice, a third unpin throws")
{
    Fixture fx("pool_pin_twice");
    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0);
    pool.fetchPage(fx.file, 0);
    CHECK(pool.stats() == IoStats{1, 1, 0, 1, 0});
    CHECK(pool.pinnedFrameCount() == 1); // one frame, pin count 2
    pool.unpinPage(fx.file, 0, false);
    CHECK(pool.pinnedFrameCount() == 1);
    pool.unpinPage(fx.file, 0, false);
    CHECK(pool.pinnedFrameCount() == 0);
    CHECK_THROWS_AS(pool.unpinPage(fx.file, 0, false), std::logic_error);
}

TEST_CASE("pool: a dirty page is written when it is evicted")
{
    Fixture fx("pool_dirty_eviction");
    BufferPool pool(1, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0)[0] = std::byte{0x5A};
    pool.unpinPage(fx.file, 0, true);
    pool.fetchPage(fx.file, 1);
    CHECK(pool.stats() == IoStats{0, 2, 1, 2, 1});
    CHECK(fx.onDisk(fx.file, 0)[0] == std::byte{0x5A});
}

TEST_CASE("pool: dirty is sticky: a later clean unpin does not lose the change")
{
    Fixture fx("pool_sticky_dirty");
    BufferPool pool(1, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0)[0] = std::byte{0x11};
    pool.unpinPage(fx.file, 0, true);
    pool.fetchPage(fx.file, 0); // hit
    pool.unpinPage(fx.file, 0, false);
    pool.fetchPage(fx.file, 1); // evicts page 0, still dirty
    CHECK(pool.stats() == IoStats{1, 2, 1, 2, 1});
    CHECK(fx.onDisk(fx.file, 0)[0] == std::byte{0x11});
}

TEST_CASE("pool: a clean page is evicted without a write")
{
    Fixture fx("pool_clean_eviction");
    BufferPool pool(1, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0);
    pool.unpinPage(fx.file, 0, false);
    pool.fetchPage(fx.file, 1);
    CHECK(pool.stats() == IoStats{0, 2, 1, 2, 0});
}

TEST_CASE("pool: flushPage and flushAll write each dirty page once and evict nothing")
{
    Fixture fx("pool_flush");
    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0)[0] = std::byte{0x22};
    pool.unpinPage(fx.file, 0, true);

    pool.flushPage(fx.file, 0);
    CHECK(pool.stats().writes == 1);
    CHECK(fx.onDisk(fx.file, 0)[0] == std::byte{0x22});
    pool.flushPage(fx.file, 0); // now clean
    pool.flushPage(fx.file, 5); // not resident: nothing, no error
    CHECK(pool.stats().writes == 1);

    // flushAll writes pinned dirty pages too.
    pool.fetchPage(fx.file, 1)[0] = std::byte{0x33};
    pool.unpinPage(fx.file, 1, true);
    pool.fetchPage(fx.file, 1); // pinned again, still dirty
    pool.flushAll();
    CHECK(pool.stats().writes == 2);
    pool.flushAll();
    CHECK(pool.stats().writes == 2);

    CHECK(pool.isResident(fx.file, 0));
    CHECK(pool.isResident(fx.file, 1));
    CHECK(pool.pinnedFrameCount() == 1);
    CHECK(pool.stats().evictions == 0);
}

TEST_CASE("pool: newPage appends a zero page, pinned and dirty, counting nothing")
{
    Fixture fx("pool_new_page"); // pages 0..7
    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    const auto created = pool.newPage(fx.file);
    CHECK(created.pageId == 8);
    CHECK(fx.files.disk(fx.file).numPages() == 9);
    REQUIRE(created.bytes.size() == kPageSize);
    CHECK(allZero(created.bytes));
    CHECK(pool.stats() == IoStats{});
    CHECK(pool.pinnedFrameCount() == 1);
    CHECK(pool.isResident(fx.file, 8));

    // Dirty from birth: a clean unpin still lets flushAll write it.
    created.bytes[100] = std::byte{0x42};
    pool.unpinPage(fx.file, 8, false);
    pool.flushAll();
    CHECK(pool.stats() == IoStats{0, 0, 0, 0, 1});
    CHECK(fx.onDisk(fx.file, 8)[100] == std::byte{0x42});
}

TEST_CASE("pool: newPage replacing a dirty page counts one write and no eviction (chunk 05 Q2)")
{
    Fixture fx("pool_new_page_victim");
    BufferPool pool(1, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0)[0] = std::byte{0xAB};
    pool.unpinPage(fx.file, 0, true);

    const auto created = pool.newPage(fx.file);
    CHECK(pool.stats() == IoStats{0, 1, 0, 1, 1}); // the fetch's miss + read, the victim's write
    CHECK(allZero(created.bytes));                 // the old page's bytes are gone
    CHECK_FALSE(pool.isResident(fx.file, 0));
    CHECK(fx.onDisk(fx.file, 0)[0] == std::byte{0xAB});
}

TEST_CASE("pool: newPage with every frame pinned throws before the file grows")
{
    Fixture fx("pool_new_page_pinned");
    BufferPool pool(1, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0);
    const IoStats before = pool.stats();
    CHECK_THROWS_AS(pool.newPage(fx.file), std::logic_error);
    CHECK(fx.files.disk(fx.file).numPages() == 8);
    CHECK(pool.stats() == before);
}

TEST_CASE("pool: page 0 of two files are two different pages")
{
    Fixture fx("pool_two_files");
    const FileId other = fx.files.create(fx.dir.path() / "u.db");
    fx.files.disk(other).allocatePage();

    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0)[0] = std::byte{'A'};
    pool.unpinPage(fx.file, 0, true);
    pool.fetchPage(other, 0)[0] = std::byte{'B'};
    pool.unpinPage(other, 0, true);
    CHECK(pool.stats() == IoStats{0, 2, 0, 2, 0});
    CHECK(pool.isResident(fx.file, 0));
    CHECK(pool.isResident(other, 0));

    pool.flushAll();
    CHECK(pool.stats().writes == 2);
    CHECK(fx.onDisk(fx.file, 0)[0] == std::byte{'A'});
    CHECK(fx.onDisk(other, 0)[0] == std::byte{'B'});
}

TEST_CASE("pool: discardFile drops a file's pages without writing them")
{
    Fixture fx("pool_discard");
    const FileId other = fx.files.create(fx.dir.path() / "u.db");
    fx.files.disk(other).allocatePage();

    BufferPool pool(2, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0)[0] = std::byte{0x77};
    pool.unpinPage(fx.file, 0, true); // dirty
    pool.fetchPage(other, 0);
    pool.unpinPage(other, 0, false);

    pool.discardFile(fx.file);
    CHECK(pool.stats().writes == 0);
    CHECK_FALSE(pool.isResident(fx.file, 0));
    CHECK(pool.isResident(other, 0));
    CHECK(fx.onDisk(fx.file, 0)[0] == std::byte{0}); // never written

    // The freed frame is reused without an eviction.
    pool.fetchPage(fx.file, 1);
    CHECK(pool.stats() == IoStats{0, 3, 0, 3, 0});
    CHECK(pool.isResident(other, 0));

    // A pinned page blocks the discard, and nothing changes.
    CHECK_THROWS_AS(pool.discardFile(fx.file), std::logic_error);
    CHECK(pool.isResident(fx.file, 1));
    CHECK(pool.pinnedFrameCount() == 1);
}

TEST_CASE("pool: pinnedFrameCount, peakPinnedCount and resetPeak")
{
    Fixture fx("pool_pin_counts");
    BufferPool pool(3, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0);
    CHECK(pool.pinnedFrameCount() == 1);
    pool.fetchPage(fx.file, 1);
    pool.fetchPage(fx.file, 1); // same frame, pin count 2
    CHECK(pool.pinnedFrameCount() == 2);
    CHECK(pool.peakPinnedCount() == 2);

    pool.unpinPage(fx.file, 1, false);
    CHECK(pool.pinnedFrameCount() == 2);
    pool.unpinPage(fx.file, 1, false);
    pool.unpinPage(fx.file, 0, false);
    CHECK(pool.pinnedFrameCount() == 0);
    CHECK(pool.peakPinnedCount() == 2);

    pool.resetPeak();
    CHECK(pool.peakPinnedCount() == 0);
    pool.fetchPage(fx.file, 2);
    CHECK(pool.peakPinnedCount() == 1);
}

// ---------------------------------------------------------------- reserved frames (exec D2)

TEST_CASE("pool: reserveFrames throws out unpinned pages, saving dirty ones, and counts no eviction")
{
    Fixture fx("pool_reserve_evicts");
    BufferPool pool(3, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0);
    pool.unpinPage(fx.file, 0, true); // dirty
    pool.fetchPage(fx.file, 1);
    pool.unpinPage(fx.file, 1, false);
    pool.fetchPage(fx.file, 2);
    pool.unpinPage(fx.file, 2, false);
    const IoStats before = pool.stats();

    // LRU throws out page 0 (saved: one write), then page 1.
    pool.reserveFrames(2);
    CHECK(pool.stats().writes == before.writes + 1);
    CHECK(pool.stats().evictions == before.evictions);
    CHECK(pool.stats().reads == before.reads);
    CHECK_FALSE(pool.isResident(fx.file, 0));
    CHECK_FALSE(pool.isResident(fx.file, 1));
    CHECK(pool.isResident(fx.file, 2));
    CHECK(pool.pinnedFrameCount() == 2);
    CHECK(pool.peakPinnedCount() == 2);

    // Only one frame is left for pages: page 3 replaces page 2, and a second
    // pinned page has no frame.
    pool.fetchPage(fx.file, 3);
    CHECK_FALSE(pool.isResident(fx.file, 2));
    CHECK(pool.peakPinnedCount() == 3);
    CHECK_THROWS_AS(pool.fetchPage(fx.file, 4), std::logic_error);
    pool.unpinPage(fx.file, 3, false);
    pool.releaseFrames(2);
}

TEST_CASE("pool: reserveFrames beyond the frames free of pins throws and changes nothing")
{
    Fixture fx("pool_reserve_too_many");
    BufferPool pool(3, ReplacementPolicy::Lru, fx.files);
    pool.fetchPage(fx.file, 0); // stays pinned
    pool.fetchPage(fx.file, 1);
    pool.unpinPage(fx.file, 1, true);
    const IoStats before = pool.stats();

    CHECK_THROWS_AS(pool.reserveFrames(3), std::logic_error);
    CHECK(pool.pinnedFrameCount() == 1);
    CHECK(pool.stats() == before);
    CHECK(pool.isResident(fx.file, 1));

    pool.reserveFrames(2); // exactly the 2 frames without a pin
    CHECK(pool.pinnedFrameCount() == 3);
    pool.releaseFrames(2);
    pool.unpinPage(fx.file, 0, false);
}

TEST_CASE("pool: releaseFrames makes the frames usable again; releasing too many throws")
{
    Fixture fx("pool_release");
    BufferPool pool(3, ReplacementPolicy::Clock, fx.files);
    pool.reserveFrames(3);
    CHECK_THROWS_AS(pool.fetchPage(fx.file, 0), std::logic_error);
    CHECK_THROWS_AS(pool.releaseFrames(4), std::logic_error);
    CHECK(pool.pinnedFrameCount() == 3);

    pool.releaseFrames(3);
    CHECK(pool.pinnedFrameCount() == 0);
    CHECK(pool.peakPinnedCount() == 3);
    // All three frames hold pages again; released frames are empty, so
    // filling them is not an eviction.
    for (PageId p = 0; p < 3; ++p)
        pool.fetchPage(fx.file, p);
    CHECK(pool.stats().evictions == 0);
    CHECK(pool.pinnedFrameCount() == 3);
    for (PageId p = 0; p < 3; ++p)
        pool.unpinPage(fx.file, p, false);
}
