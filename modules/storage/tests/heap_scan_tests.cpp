// Heap scan (SPEC §M1.5): rows in directory then slot order, at most one
// page pinned at a time (exec seqscan budget = 1 frame), reads = pages of the table at any
// B >= 2 under LRU (no materialisation), B = 1 works, delete-all gives an empty scan, lazy start,
// close, moves, determinism.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"
#include "db/storage/io_stats.h"
#include "db/storage/page.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/rid.h"

namespace
{

    namespace fs = std::filesystem;
    using db::Column;
    using db::Schema;
    using db::Tuple;
    using db::Type;
    using db::Value;
    using db::storage::BufferPool;
    using db::storage::FileId;
    using db::storage::FileRegistry;
    using db::storage::HeapFile;
    using db::storage::HeapScan;
    using db::storage::IoStats;
    using db::storage::ReplacementPolicy;
    using db::storage::Rid;

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

    // The §M1.3 example schema (a INT, b VARCHAR(5), c INT) of table R.
    Schema exampleSchema()
    {
        return Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Varchar, 5},
            Column{"R", "c", Type::Int, 0}});
    }

    // (i, 'bob', i): 13 bytes, 240 per page.
    Tuple row(std::int32_t i)
    {
        return Tuple{Value::ofInt(i), Value::ofVarchar("bob"), Value::ofInt(i)};
    }

    // (i, <2000 - 8 bytes of text>): a 2000-byte record, 2 per page.
    Schema wideSchema()
    {
        return Schema({Column{"R", "a", Type::Int, 0}, Column{"R", "b", Type::Varchar, 255},
            Column{"R", "c", Type::Varchar, 255}, Column{"R", "d", Type::Varchar, 255},
            Column{"R", "e", Type::Varchar, 255}, Column{"R", "f", Type::Varchar, 255},
            Column{"R", "g", Type::Varchar, 255}, Column{"R", "h", Type::Varchar, 255},
            Column{"R", "i", Type::Varchar, 255}});
    }

    Tuple wideRow(std::int32_t i)
    {
        // 4 + 8 * 2 length bytes = 20; 1980 bytes of text: 7 * 255 + 195.
        Tuple tuple{Value::ofInt(i)};
        for (int column = 0; column < 7; ++column)
            tuple.push_back(Value::ofVarchar(std::string(255, 'x')));
        tuple.push_back(Value::ofVarchar(std::string(195, 'y')));
        return tuple;
    }

    // Every (rid, tuple) a scan yields, in order. Collecting them is fine in
    // a test; the scan itself never does this.
    struct Scanned
    {
        std::vector<Rid> rids;
        std::vector<Tuple> tuples;
    };

    Scanned scanAll(const HeapFile& heap)
    {
        Scanned out;
        HeapScan scan = heap.scan();
        Rid rid;
        Tuple tuple;
        while (scan.next(rid, tuple))
        {
            out.rids.push_back(rid);
            out.tuples.push_back(tuple);
        }
        return out;
    }

    // A table file "R.db" filled through a pool that is then flushed and
    // destroyed, so each test can scan it with a fresh ("cold") pool of any
    // size: nothing resident, every page fetch a miss.
    struct Table
    {
        TempDir dir;
        FileRegistry files;
        FileId file;
        Schema schema;

        Table(const std::string& name, Schema tableSchema) : dir(name), schema(std::move(tableSchema))
        {
            file = files.create(dir.path() / "R.db");
            BufferPool pool(4, ReplacementPolicy::Lru, files);
            HeapFile::create(pool, file);
            pool.flushAll();
        }

        // Inserts `count` rows made by `makeRow(0..count-1)`, then flushes.
        template <typename MakeRow>
        void fill(int count, MakeRow makeRow)
        {
            BufferPool pool(4, ReplacementPolicy::Lru, files);
            HeapFile heap(pool, file, schema, "R");
            for (int i = 0; i < count; ++i)
                heap.insert(makeRow(i));
            pool.flushAll();
        }

        HeapFile open(BufferPool& pool) const { return HeapFile(pool, file, schema, "R"); }
    };

    // Scans the whole table with a cold pool of `frames` frames. Checks the
    // pin rule and that no pin is left; returns the counter difference and
    // how many rows came back.
    struct ColdScan
    {
        IoStats cost;
        std::size_t rows = 0;
    };

    ColdScan coldScan(Table& table, std::size_t frames)
    {
        BufferPool pool(frames, ReplacementPolicy::Lru, table.files);
        const HeapFile heap = table.open(pool);
        pool.resetPeak();
        const IoStats before = pool.stats();
        ColdScan result;
        result.rows = scanAll(heap).rids.size();
        result.cost = pool.stats() - before;
        CHECK(pool.peakPinnedCount() <= 1);
        CHECK(pool.pinnedFrameCount() == 0);
        return result;
    }

} // namespace

TEST_CASE("HeapScan: an empty table gives no rows, again and again, and leaves nothing pinned")
{
    Table table("scan_empty", exampleSchema());
    BufferPool pool(2, ReplacementPolicy::Lru, table.files);
    const HeapFile heap = table.open(pool);
    HeapScan scan = heap.scan();
    Rid rid;
    Tuple tuple;
    CHECK_FALSE(scan.next(rid, tuple));
    CHECK_FALSE(scan.next(rid, tuple));
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapScan: lazy start - making a scan reads and pins nothing")
{
    Table table("scan_lazy", exampleSchema());
    table.fill(10, row);
    BufferPool pool(2, ReplacementPolicy::Lru, table.files);
    const HeapFile heap = table.open(pool);
    const IoStats before = pool.stats();
    {
        HeapScan scan = heap.scan();
        CHECK(pool.pinnedFrameCount() == 0);
    }
    CHECK(pool.stats() == before);
}

TEST_CASE("HeapScan: 500 rows come back in insert order; (1, 239) is followed by (2, 0)")
{
    Table table("scan_order", exampleSchema());
    table.fill(500, row);
    BufferPool pool(4, ReplacementPolicy::Lru, table.files);
    const Scanned scanned = scanAll(table.open(pool));
    REQUIRE(scanned.rids.size() == 500);
    for (int i = 0; i < 500; ++i)
    {
        // 240 rows per page: row i is on page 1 + i / 240, slot i % 240.
        const Rid expected{static_cast<std::uint32_t>(1 + i / 240), static_cast<std::uint16_t>(i % 240)};
        REQUIRE(scanned.rids[i] == expected);
        REQUIRE(scanned.tuples[i] == row(i));
    }
    CHECK(scanned.rids[239] == Rid{1, 239});
    CHECK(scanned.rids[240] == Rid{2, 0});
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapScan: a table twenty times larger than B completes with 1 pin; reads = table pages at any B")
{
    // 160 rows of 2000 bytes = 80 data pages + directory page 0 = 81 pages,
    // 20 times B = 4.
    Table table("scan_20x", wideSchema());
    table.fill(160, wideRow);

    for (std::size_t frames : {std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{64}})
    {
        CAPTURE(frames);
        const ColdScan scan = coldScan(table, frames);
        CHECK(scan.rows == 160);
        // Every page read once, whatever B is: the scan streams, it does
        // not keep pages or rows. With B = 64 the read count would drop
        // only if something were read twice and cached, which never happens.
        CHECK(scan.cost.reads == 81);
        CHECK(scan.cost.misses == 81);
        // The directory page is fetched 81 times (once per data page, plus
        // once to find there is no next entry); only the first is a miss.
        // Under LRU it is always the newest page, so it is never evicted.
        CHECK(scan.cost.hits == 80);
        CHECK(scan.cost.writes == 0);
    }
}

TEST_CASE("HeapScan: works with B = 1 (one page pinned at a time); the directory page is then re-read")
{
    // Same 81-page table. With one frame, every fetch replaces the page
    // before it: the directory page is read 81 times and each data page once.
    Table table("scan_one_frame", wideSchema());
    table.fill(160, wideRow);
    const ColdScan scan = coldScan(table, 1);
    CHECK(scan.rows == 160);
    CHECK(scan.cost.reads == 80 + 81);
    CHECK(scan.cost.hits == 0);
    CHECK(scan.cost.writes == 0);
}

TEST_CASE("HeapScan: deleting every record gives an empty scan, without error")
{
    Table table("scan_delete_all", exampleSchema());
    table.fill(500, row);
    {
        BufferPool pool(4, ReplacementPolicy::Lru, table.files);
        HeapFile heap = table.open(pool);
        for (const Rid& rid : scanAll(heap).rids)
            heap.remove(rid);
        pool.flushAll();
    }
    const ColdScan scan = coldScan(table, 2);
    CHECK(scan.rows == 0);
    // The empty pages are still listed, so they are still read.
    CHECK(scan.cost.reads == 4);
}

TEST_CASE("HeapScan: a page whose rows were all deleted is skipped; the scan goes from page 1 to page 3")
{
    Table table("scan_skip_page", exampleSchema());
    table.fill(500, row);
    BufferPool pool(4, ReplacementPolicy::Lru, table.files);
    HeapFile heap = table.open(pool);
    for (std::uint16_t slot = 0; slot < 240; ++slot)
        heap.remove(Rid{2, slot});
    // Also some single holes on page 1.
    heap.remove(Rid{1, 0});
    heap.remove(Rid{1, 100});

    const Scanned scanned = scanAll(heap);
    CHECK(scanned.rids.size() == 500 - 240 - 2);
    CHECK(scanned.rids.front() == Rid{1, 1});
    CHECK(scanned.rids[98] == Rid{1, 99});
    CHECK(scanned.rids[99] == Rid{1, 101});
    CHECK(scanned.rids[237] == Rid{1, 239});
    CHECK(scanned.rids[238] == Rid{3, 0});
    CHECK(scanned.tuples[238] == row(480));
}

TEST_CASE("HeapScan: crosses into directory page 682 and never pins more than 1 page (B = 2)")
{
    // 1400 rows of 2000 bytes: data pages 1..681 on directory page 0,
    // 683..701 on directory page 682.
    Table table("scan_two_directories", wideSchema());
    table.fill(1400, wideRow);

    BufferPool pool(2, ReplacementPolicy::Lru, table.files);
    const HeapFile heap = table.open(pool);
    pool.resetPeak();
    const IoStats before = pool.stats();
    const Scanned scanned = scanAll(heap);
    const IoStats cost = pool.stats() - before;

    REQUIRE(scanned.rids.size() == 1400);
    CHECK(scanned.rids[1361] == Rid{681, 1});
    CHECK(scanned.rids[1362] == Rid{683, 0});
    CHECK(scanned.rids[1399] == Rid{701, 1});
    for (int i = 0; i < 1400; ++i)
        REQUIRE(scanned.tuples[i] == wideRow(i));
    // 700 data pages + 2 directory pages, each read once.
    CHECK(cost.reads == 702);
    CHECK(pool.peakPinnedCount() <= 1);
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapScan: close in the middle drops every pin; next then returns false")
{
    Table table("scan_close", exampleSchema());
    table.fill(500, row);
    BufferPool pool(4, ReplacementPolicy::Lru, table.files);
    const HeapFile heap = table.open(pool);
    HeapScan scan = heap.scan();
    Rid rid;
    Tuple tuple;
    for (int i = 0; i < 300; ++i)
        REQUIRE(scan.next(rid, tuple));
    CHECK(pool.pinnedFrameCount() == 1); // data page 2 only; the directory page is not kept pinned
    scan.close();
    CHECK(pool.pinnedFrameCount() == 0);
    CHECK_FALSE(scan.next(rid, tuple));
    scan.close(); // again: nothing happens
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapScan: a scan that goes out of scope mid-way releases its pins")
{
    Table table("scan_scope", exampleSchema());
    table.fill(500, row);
    BufferPool pool(4, ReplacementPolicy::Lru, table.files);
    const HeapFile heap = table.open(pool);
    {
        HeapScan scan = heap.scan();
        Rid rid;
        Tuple tuple;
        REQUIRE(scan.next(rid, tuple));
        CHECK(pool.pinnedFrameCount() == 1); // the data page only
    }
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapScan: a move hands over the position; the moved-from scan is finished")
{
    Table table("scan_move", exampleSchema());
    table.fill(500, row);
    BufferPool pool(4, ReplacementPolicy::Lru, table.files);
    const HeapFile heap = table.open(pool);
    Rid rid;
    Tuple tuple;

    SUBCASE("moved while running")
    {
        HeapScan first = heap.scan();
        for (int i = 0; i < 250; ++i)
            REQUIRE(first.next(rid, tuple));
        HeapScan second = std::move(first);
        CHECK(pool.pinnedFrameCount() == 1); // the pin moved, none added
        CHECK_FALSE(first.next(rid, tuple)); // NOLINT(bugprone-use-after-move): testing it on purpose
        REQUIRE(second.next(rid, tuple));
        CHECK(rid == Rid{2, 10});
        CHECK(tuple == row(250));
    }
    SUBCASE("moved before it started")
    {
        HeapScan first = heap.scan();
        HeapScan second = std::move(first);
        CHECK_FALSE(first.next(rid, tuple)); // NOLINT(bugprone-use-after-move)
        CHECK(pool.pinnedFrameCount() == 0);
        REQUIRE(second.next(rid, tuple));
        CHECK(rid == Rid{1, 0});
    }
    SUBCASE("move assignment releases the target's own pins first")
    {
        HeapScan first = heap.scan();
        HeapScan second = heap.scan();
        for (int i = 0; i < 300; ++i)
            REQUIRE(first.next(rid, tuple)); // first is on data page 2
        REQUIRE(second.next(rid, tuple));    // second is on data page 1
        // Frames pinned: page 1 (second) and page 2 (first). No scan keeps
        // the directory page pinned.
        CHECK(pool.pinnedFrameCount() == 2);
        second = std::move(first);
        // second gave back page 1, which is now unpinned.
        CHECK(pool.pinnedFrameCount() == 1);
        REQUIRE(second.next(rid, tuple));
        CHECK(rid == Rid{2, 60});
        CHECK(tuple == row(300));
    }
    CHECK(pool.pinnedFrameCount() == 0);
}

TEST_CASE("HeapScan: the same scan from the same pool state gives the same counters")
{
    Table table("scan_determinism", wideSchema());
    table.fill(160, wideRow);
    const ColdScan first = coldScan(table, 4);
    const ColdScan second = coldScan(table, 4);
    CHECK(first.cost == second.cost);
    CHECK(first.rows == second.rows);
}
