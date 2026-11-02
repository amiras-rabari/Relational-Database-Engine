// CHUNK 01 tests: the Operator interface, its counters, and how page reads are split between a node and its children.

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "db/exec/operator.h"
#include "db/storage/page_guard.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/temp_file.h"
#include "tests/test_sources.h"

using namespace db;
using namespace db::exec;
namespace fs = std::filesystem;

namespace
{
    // A fresh engine with ONE frame and a temp file of 2 pages (0 and 1),
    // both on disk. With one frame, fetching the page that is not resident
    // is always a miss: alternating page 0 / page 1 reads 1 page each time.
    // Page 1 is resident when the fixture is ready.
    struct OneFrameEngine
    {
        fs::path dir;
        std::unique_ptr<storage::StorageEngine> engine;
        storage::TempFile file;

        explicit OneFrameEngine(const std::string& name)
            : dir(fs::temp_directory_path() / ("comp3005_exec_" + name))
        {
            fs::remove_all(dir);
            engine = storage::StorageEngine::open(dir, 1, storage::ReplacementPolicy::Lru);
            file = engine->createTempFile();
            for (storage::PageId p = 0; p < 2; ++p)
            {
                { storage::PageGuard g = storage::PageGuard::newPage(engine->pool(), file.fileId()); }
                engine->pool().flushPage(file.fileId(), p); // on disk, so the later miss is a real read
            }
        }

        ~OneFrameEngine()
        {
            file = storage::TempFile(); // drops the temp file before the engine goes
            engine.reset();
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    };

    // Test operator: on each next(), pulls one tuple from its child (if any),
    // then reads page `page` itself. Always produces a tuple.
    class ReadPageOperator : public Operator
    {
    public:
        ReadPageOperator(ExecContext& ctx, storage::FileId file, storage::PageId page, Children kids = {})
            : Operator(ctx, std::move(kids)), file_(file), page_(page)
        {
        }

    protected:
        void doOpen() override
        {
            for (const auto& c : children())
                c->open();
        }
        bool doNext(Tuple& out) override
        {
            Tuple fromChild;
            if (!children().empty())
                child(0).next(fromChild);
            storage::PageGuard g = storage::PageGuard::fetch(ctx().pool(), file_, page_);
            out = Tuple{Value::ofInt(1)};
            return true;
        }
        void doClose() override
        {
            for (const auto& c : children())
                c->close();
        }

    private:
        storage::FileId file_;
        storage::PageId page_;
    };

    // Test operator: pins one page from open() until close().
    class PinHolderOperator : public Operator
    {
    public:
        PinHolderOperator(ExecContext& ctx, storage::FileId file) : Operator(ctx, {}), file_(file) {}

    protected:
        void doOpen() override { guard_ = storage::PageGuard::fetch(ctx().pool(), file_, 0); }
        bool doNext(Tuple&) override { return false; }
        void doClose() override { guard_ = storage::PageGuard(); }

    private:
        storage::FileId file_;
        storage::PageGuard guard_;
    };

    Operator::Children kids(std::unique_ptr<Operator> op)
    {
        Operator::Children v;
        v.push_back(std::move(op));
        return v;
    }

    // Test 3's plan: parent reads page 1, its child reads page 0. Returns
    // {child, parent} own counters after one parent next().
    std::pair<ExecCounters, ExecCounters> runTwoLevels(const std::string& name)
    {
        OneFrameEngine db(name);
        ExecContext ctx(*db.engine);
        auto childOp = std::make_unique<ReadPageOperator>(ctx, db.file.fileId(), 0);
        const Operator* childPtr = childOp.get();
        ReadPageOperator parent(ctx, db.file.fileId(), 1, kids(std::move(childOp)));

        const storage::IoStats before = db.engine->pool().stats();
        parent.open();
        Tuple t;
        CHECK(parent.next(t));
        parent.close();
        const storage::IoStats whole = db.engine->pool().stats() - before;

        const ExecCounters c = childPtr->counters();
        const ExecCounters p = parent.counters();
        CHECK(c.pageReads + p.pageReads == whole.reads);  // nothing lost, nothing counted twice
        CHECK(subtreeCounters(parent).pageReads == whole.reads);
        return {c, p};
    }
} // namespace

TEST_CASE("chunk01: a source returns its tuples, then END; tuplesOut counts them")
{
    OneFrameEngine db("source");
    ExecContext ctx(*db.engine);
    test::VectorSourceOperator src(ctx, {Tuple{Value::ofInt(1)}, Tuple{Value::ofInt(2)}, Tuple{Value::ofInt(3)}});

    src.open();
    Tuple t;
    CHECK(src.next(t));
    CHECK(src.next(t));
    CHECK(src.next(t));
    CHECK(t == Tuple{Value::ofInt(3)});
    CHECK_FALSE(src.next(t));
    src.close();

    CHECK(src.counters() == ExecCounters{3, 0, 0, 0});
}

TEST_CASE("chunk01: closing twice is harmless")
{
    OneFrameEngine db("close_twice");
    ExecContext ctx(*db.engine);
    test::VectorSourceOperator src(ctx, {});
    src.open();
    src.close();
    src.close();
    CHECK(src.counters() == ExecCounters{});
}

TEST_CASE("chunk01: each node is charged only its own page reads")
{
    const auto [c, p] = runTwoLevels("two_levels");
    CHECK(c.pageReads == 1);
    CHECK(p.pageReads == 1);
    CHECK(c.tuplesOut == 1);
    CHECK(p.tuplesOut == 1);
}

TEST_CASE("chunk01: each node is charged only its own reads, three levels deep")
{
    OneFrameEngine db("three_levels");
    ExecContext ctx(*db.engine);
    // Reads happen bottom-up: child page 0, parent page 1, grandparent page 0.
    auto childOp = std::make_unique<ReadPageOperator>(ctx, db.file.fileId(), 0);
    const Operator* childPtr = childOp.get();
    auto parentOp = std::make_unique<ReadPageOperator>(ctx, db.file.fileId(), 1, kids(std::move(childOp)));
    const Operator* parentPtr = parentOp.get();
    ReadPageOperator grand(ctx, db.file.fileId(), 0, kids(std::move(parentOp)));

    grand.open();
    Tuple t;
    CHECK(grand.next(t));
    grand.close();

    CHECK(childPtr->counters().pageReads == 1);
    CHECK(parentPtr->counters().pageReads == 1);
    CHECK(grand.counters().pageReads == 1);
    CHECK(subtreeCounters(grand).pageReads == 3);
    CHECK(subtreeCounters(*parentPtr).pageReads == 2);
}

TEST_CASE("chunk01: destroying an open operator releases its pins")
{
    OneFrameEngine db("destroy_open");
    ExecContext ctx(*db.engine);
    {
        PinHolderOperator op(ctx, db.file.fileId());
        op.open();
        CHECK(db.engine->pool().pinnedFrameCount() == 1);
    } // destroyed without close()
    CHECK(db.engine->pool().pinnedFrameCount() == 0);
}

TEST_CASE("chunk01: counters are identical across repeated runs")
{
    CHECK(runTwoLevels("repeat_a") == runTwoLevels("repeat_b"));
}
