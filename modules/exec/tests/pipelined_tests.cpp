// CHUNK 04 tests: seqscan, filter, project, limit, union_all and rename on
// real tables. Page counts assume LRU with B = 4 and a cold pool, where the
// directory page stays cached during a scan (storage INTERFACE, heap scan).

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "db/common/condition.h"
#include "db/common/error.h"
#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/counters.h"
#include "db/exec/exec_context.h"
#include "db/exec/operator.h"
#include "db/storage/io_stats.h"
#include "operators/pipelined.h"
#include "tests/test_db.h"
#include "tests/test_sources.h"

using namespace db;
using namespace db::exec;
using db::exec::test::drain;
using db::exec::test::TestDb;

namespace
{
    // R(a INT, b VARCHAR(10), c INT). With a 3-byte b a record is
    // 4 + (2 + 3) + 4 = 13 bytes, so 240 rows fill one data page.
    Schema rSchema()
    {
        return Schema({{"R", "a", Type::Int, 0}, {"R", "b", Type::Varchar, 10}, {"R", "c", Type::Int, 0}});
    }

    // Row i: a = i, b = 'csc' for even i and 'eng' for odd i, c = i mod 7.
    std::vector<Tuple> rRows(int count)
    {
        std::vector<Tuple> rows;
        for (int i = 0; i < count; ++i)
            rows.push_back({Value::ofInt(i), Value::ofVarchar(i % 2 == 0 ? "csc" : "eng"), Value::ofInt(i % 7)});
        return rows;
    }

    // A database with R = 2,400 rows = exactly 10 data pages, and a cold pool.
    struct TenPages
    {
        TestDb db;
        explicit TenPages(const std::string& name) : db(name)
        {
            db.createTable("R", rSchema(), rRows(2400));
            db.coldPool();
        }
    };

    std::unique_ptr<Operator> seqscan(ExecContext& ctx, const std::string& table)
    {
        return std::make_unique<SeqScanOperator>(ctx, table);
    }

    std::vector<ColumnRef> refs(const std::vector<std::string>& qualifiedNames)
    {
        std::vector<ColumnRef> result;
        for (const std::string& name : qualifiedNames)
        {
            const std::size_t dot = name.find('.');
            result.push_back({name.substr(0, dot), name.substr(dot + 1)});
        }
        return result;
    }
} // namespace

TEST_CASE("pipelined: seqscan returns every row in insert order and reads each page once")
{
    TenPages fx("seqscan");
    ExecContext ctx(fx.db.engine());
    SeqScanOperator scan(ctx, "R");
    CHECK(scan.schema().column(1).qualifiedName() == "R.b");

    const std::vector<Tuple> rows = drain(scan);
    CHECK(rows == rRows(2400));
    CHECK(scan.counters().tuplesOut == 2400);
    CHECK(scan.counters().pageReads == 11); // 10 data pages + directory page 0
    CHECK(scan.counters().comparisons == 0);
}

TEST_CASE("pipelined: filter counts one comparison per tuple examined, not per 'and' part")
{
    TenPages fx("filter");
    ExecContext ctx(fx.db.engine());
    FilterOperator filter(ctx, seqscan(ctx, "R"), parseCondition("R.a > 500 and R.b = 'csc'"));

    const std::vector<Tuple> rows = drain(filter);
    CHECK(rows.size() == 949); // even a from 502 to 2398
    CHECK(filter.counters().comparisons == 2400);
    CHECK(filter.counters().tuplesOut == 949);
    CHECK(filter.counters().pageReads == 0); // the scan below did the reading
}

TEST_CASE("pipelined: a type error in a filter is reported before any page is read")
{
    TenPages fx("filter_type");
    ExecContext ctx(fx.db.engine());
    const storage::IoStats before = fx.db.engine().pool().stats();
    try
    {
        FilterOperator filter(ctx, seqscan(ctx, "R"), parseCondition("R.a = 'x'"));
        FAIL("expected db::Error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Type);
    }
    CHECK(fx.db.engine().pool().stats() == before);
}

TEST_CASE("pipelined: project keeps duplicates; naming a column twice is a schema error")
{
    TenPages fx("project");
    ExecContext ctx(fx.db.engine());
    ProjectOperator project(ctx, seqscan(ctx, "R"), refs({"R.b"}));
    CHECK(project.schema().size() == 1);

    const std::vector<Tuple> rows = drain(project);
    REQUIRE(rows.size() == 2400); // only 'csc' and 'eng', every row kept
    CHECK(rows[0] == Tuple{Value::ofVarchar("csc")});
    CHECK(rows[1] == Tuple{Value::ofVarchar("eng")});
    CHECK(rows[2] == Tuple{Value::ofVarchar("csc")});

    CHECK_THROWS_AS(ProjectOperator(ctx, seqscan(ctx, "R"), refs({"R.a", "R.a"})), Error);
}

TEST_CASE("pipelined: limit stops pulling and closes its child after n tuples")
{
    TenPages fx("limit");
    ExecContext ctx(fx.db.engine());
    LimitOperator limit(ctx, seqscan(ctx, "R"), 3);

    limit.open();
    Tuple t;
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(limit.next(t));
        CHECK(t == rRows(3)[i]);
    }
    CHECK(fx.db.engine().pool().pinnedFrameCount() == 0); // child already closed
    CHECK_FALSE(limit.next(t));
    CHECK_FALSE(limit.next(t)); // END again
    limit.close();

    const ExecCounters& scan = limit.children()[0]->counters();
    CHECK(scan.tuplesOut == 3);
    CHECK(scan.pageReads == 2); // directory page + first data page only
}

TEST_CASE("pipelined: limit 0 never opens its child")
{
    TenPages fx("limit0");
    ExecContext ctx(fx.db.engine());
    LimitOperator limit(ctx, seqscan(ctx, "R"), 0);
    CHECK(drain(limit).empty());
    CHECK(subtreeCounters(limit) == ExecCounters{});

    // A seqscan reads nothing at open (lazy start), so the page count alone
    // cannot show that the child stayed closed. Count opens directly.
    auto source = std::make_unique<test::VectorSourceOperator>(ctx, rRows(2));
    test::VectorSourceOperator& sourceRef = *source;
    LimitOperator limitSource(ctx, std::move(source), 0);
    CHECK(drain(limitSource).empty());
    CHECK(sourceRef.opens == 0);
}

TEST_CASE("pipelined: union_all concatenates and takes the left names")
{
    TestDb db("union_all");
    db.createTable("A", Schema({{"A", "a", Type::Int, 0}, {"A", "b", Type::Varchar, 5}}),
        {{Value::ofInt(1), Value::ofVarchar("x")}, {Value::ofInt(2), Value::ofVarchar("y")},
         {Value::ofInt(1), Value::ofVarchar("x")}});
    db.createTable("B", Schema({{"B", "p", Type::Int, 0}, {"B", "q", Type::Varchar, 9}}),
        {{Value::ofInt(1), Value::ofVarchar("x")}, {Value::ofInt(3), Value::ofVarchar("z")}});
    db.createTable("C", Schema({{"C", "p", Type::Int, 0}, {"C", "q", Type::Int, 0}}), {});
    db.createTable("D", Schema({{"D", "p", Type::Int, 0}}), {});
    ExecContext ctx(db.engine());

    UnionAllOperator both(ctx, seqscan(ctx, "A"), seqscan(ctx, "B"));
    CHECK(both.schema().column(0).qualifiedName() == "A.a");
    CHECK(both.schema().column(1).qualifiedName() == "A.b");
    const std::vector<Tuple> rows = drain(both);
    REQUIRE(rows.size() == 5); // duplicates kept
    CHECK(rows[3] == Tuple{Value::ofInt(1), Value::ofVarchar("x")});
    CHECK(rows[4] == Tuple{Value::ofInt(3), Value::ofVarchar("z")});

    // VARCHAR against INT in column 2, and 2 columns against 1.
    try
    {
        UnionAllOperator bad(ctx, seqscan(ctx, "A"), seqscan(ctx, "C"));
        FAIL("expected db::Error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Schema);
    }
    CHECK_THROWS_AS(UnionAllOperator(ctx, seqscan(ctx, "A"), seqscan(ctx, "D")), Error);
}

TEST_CASE("pipelined: seqscan of an unknown table is a name error at construction")
{
    TestDb db("unknown");
    ExecContext ctx(db.engine());
    try
    {
        SeqScanOperator scan(ctx, "X");
        FAIL("expected db::Error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Name);
    }
}

TEST_CASE("pipelined: an empty table through every operator gives END at once")
{
    TestDb db("empty");
    db.createTable("E", rSchema(), {});
    db.coldPool();
    ExecContext ctx(db.engine());

    std::vector<std::unique_ptr<Operator>> plans;
    plans.push_back(seqscan(ctx, "E"));
    plans.push_back(std::make_unique<FilterOperator>(ctx, seqscan(ctx, "E"), parseCondition("E.a > 1")));
    plans.push_back(std::make_unique<ProjectOperator>(ctx, seqscan(ctx, "E"), refs({"E.c"})));
    plans.push_back(std::make_unique<LimitOperator>(ctx, seqscan(ctx, "E"), 5));
    plans.push_back(std::make_unique<UnionAllOperator>(ctx, seqscan(ctx, "E"), seqscan(ctx, "E")));
    plans.push_back(std::make_unique<RenameOperator>(ctx, seqscan(ctx, "E"), "F"));

    for (const auto& plan : plans)
    {
        CHECK(drain(*plan).empty());
        const ExecCounters total = subtreeCounters(*plan);
        CHECK(total.tuplesOut == 0);
        CHECK(total.comparisons == 0);
        CHECK(total.pageWrites == 0);
    }
    // Only the first scan read directory page 0; every later one found it cached.
    CHECK(db.engine().pool().stats().reads == 1);
}

TEST_CASE("pipelined: closing and reopening a seqscan starts again from the first row")
{
    TenPages fx("rescan");
    ExecContext ctx(fx.db.engine());
    SeqScanOperator scan(ctx, "R");
    const std::vector<Tuple> first = drain(scan);
    const std::vector<Tuple> second = drain(scan);
    CHECK(first.size() == 2400);
    CHECK(second == first);
    CHECK(scan.counters().tuplesOut == 4800);
}

TEST_CASE("pipelined: rename requalifies the columns and passes tuples through")
{
    TestDb db("rename");
    db.createTable("R", rSchema(), rRows(5));
    ExecContext ctx(db.engine());
    RenameOperator rename(ctx, seqscan(ctx, "R"), "E");
    CHECK(rename.schema().column(0).qualifiedName() == "E.a");
    CHECK(rename.schema().column(2).qualifiedName() == "E.c");
    CHECK(drain(rename) == rRows(5));
}
