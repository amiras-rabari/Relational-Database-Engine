// Physical plan JSON (exec CHUNK 02): the spec's examples, a round trip of
// every physical op, and every way physicalFromJson rejects a plan.

#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/plan.h"
#include "db/common/plan_json.h"
#include "db/common/schema.h"

namespace
{
    using db::ErrorCategory;
    using db::PlanJson;
    using db::PlanNode;

    const std::string kScanR = R"({"op":"seqscan","table":"R"})";
    const std::string kScanS = R"({"op":"seqscan","table":"S"})";

    // The error a physical plan text gives, or nothing if it is accepted.
    std::optional<db::Error> physicalError(const std::string& text)
    {
        try
        {
            db::physicalFromJsonText(text);
        }
        catch (const db::Error& error)
        {
            return error;
        }
        return std::nullopt;
    }

    void checkRejected(const std::string& text, ErrorCategory category, const std::string& message)
    {
        INFO("plan: ", text);
        const std::optional<db::Error> error = physicalError(text);
        REQUIRE(error.has_value());
        CHECK(error->category() == category);
        CHECK(error->message() == message);
    }

    // Reads `text`, writes it back, reads that again. The tree must survive,
    // and the written text must equal the input pretty-printed the same way
    // (same fields, same order, same values).
    void checkRoundTrip(const std::string& text)
    {
        INFO("plan: ", text);
        const PlanNode first = db::physicalFromJsonText(text);
        const std::string written = db::toJsonText(first);
        CHECK(written == PlanJson::parse(text).dump(2));
        CHECK(db::physicalFromJsonText(written) == first);
    }

    // `levels` limit nodes on top of one seqscan.
    std::string limitChain(int levels)
    {
        std::string text = kScanR;
        for (int i = 0; i < levels; ++i)
        {
            text = R"({"op":"limit","n":1,"child":)" + text + "}";
        }
        return text;
    }
} // namespace

TEST_CASE("chunk02: the spec's example plans read and write back byte for byte")
{
    // exec SPEC §M2.2
    checkRoundTrip(R"({
      "op": "hash_join",
      "condition": "R.b = S.b",
      "left":  { "op": "filter",
                 "condition": "R.a > 500",
                 "child": { "op": "seqscan", "table": "R" } },
      "right": { "op": "seqscan", "table": "S" }
    })");
    // exec SPEC §M2.3 sort and group_by
    checkRoundTrip(R"({ "op": "sort", "keys": [["R.a","asc"],["R.b","desc"]], "child": )" + kScanR + "}");
    checkRoundTrip(R"({ "op": "group_by", "mode": "hash",
      "keys": ["R.dept"],
      "aggregates": [["count","*"],["avg","R.salary"],["max","R.name"]],
      "child": )" + kScanR + "}");
}

TEST_CASE("chunk02: every physical op round-trips")
{
    const std::vector<std::string> plans = {
        kScanR,
        R"({"op":"filter","condition":"R.a > 5 and R.b = 'x'","child":)" + kScanR + "}",
        R"({"op":"project","columns":["R.a","b"],"child":)" + kScanR + "}",
        R"({"op":"distinct","mode":"sort","child":)" + kScanR + "}",
        R"({"op":"distinct","mode":"hash","child":)" + kScanR + "}",
        R"({"op":"sort","keys":[["R.a","desc"]],"child":)" + kScanR + "}",
        R"({"op":"group_by","mode":"sort","keys":[],"aggregates":[["sum","R.a"],["min","R.b"]],"child":)" + kScanR + "}",
        R"({"op":"group_by","mode":"hash","keys":["R.a","R.b"],"aggregates":[],"child":)" + kScanR + "}",
        R"({"op":"nested_loop_join","condition":"R.a < S.a","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"sort_merge_join","condition":"R.a = S.a","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"hash_join","condition":"R.a = S.a","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"union_all","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"union","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"intersect","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"except","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
        R"({"op":"limit","n":0,"child":)" + kScanR + "}",
        R"({"op":"limit","n":2147483647,"child":)" + kScanR + "}",
        R"({"op":"rename","to":"E2","child":)" + kScanR + "}", // our extension (self-joins)
    };
    for (const std::string& text : plans)
        checkRoundTrip(text);
}

TEST_CASE("chunk02: node data is kept (sort direction, aggregates, mode, n)")
{
    const PlanNode g = db::physicalFromJsonText(
        R"({"op":"group_by","mode":"hash","keys":["R.dept"],"aggregates":[["count","*"],["avg","R.salary"]],"child":)" +
        kScanR + "}");
    CHECK(g.op == db::PlanOp::GroupBy);
    CHECK(g.mode == db::ExecMode::Hash);
    CHECK(g.columns == std::vector<db::ColumnRef>{db::ColumnRef{"R", "dept"}});
    REQUIRE(g.aggregates.size() == 2);
    CHECK(g.aggregates[0].function == db::AggregateFunction::Count);
    CHECK_FALSE(g.aggregates[0].column.has_value()); // count(*)
    CHECK(g.aggregates[1].function == db::AggregateFunction::Avg);
    CHECK(g.aggregates[1].column == db::ColumnRef{"R", "salary"});

    const PlanNode s = db::physicalFromJsonText(R"({"op":"sort","keys":[["R.a","asc"],["R.b","desc"]],"child":)" + kScanR + "}");
    REQUIRE(s.sortKeys.size() == 2);
    CHECK(s.sortKeys[0].ascending);
    CHECK_FALSE(s.sortKeys[1].ascending);

    CHECK(db::physicalFromJsonText(R"({"op":"limit","n":10,"child":)" + kScanR + "}").limitCount == 10);
}

TEST_CASE("chunk02: the spec's error messages")
{
    checkRejected(R"({"op":"sort","child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'sort': missing required field 'keys'.");
    checkRejected(R"({"op":"filter","condition":5,"child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'filter': field 'condition' must be a string.");
    checkRejected(R"({"op":"merge_join","condition":"R.a = S.a","left":)" + kScanR + R"(,"right":)" + kScanS + "}",
                  ErrorCategory::Schema, "Unknown plan operator 'merge_join'.");
}

TEST_CASE("chunk02: badly structured fields name the node and the problem")
{
    checkRejected(R"({"op":"sort","keys":[["R.a","up"]],"child":)" + kScanR + "}", ErrorCategory::Schema,
                  R"(Node 'sort': each entry of 'keys' must be [column, "asc" or "desc"].)");
    checkRejected(R"({"op":"sort","keys":["R.a"],"child":)" + kScanR + "}", ErrorCategory::Schema,
                  R"(Node 'sort': each entry of 'keys' must be [column, "asc" or "desc"].)");
    checkRejected(R"({"op":"sort","keys":[],"child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'sort': field 'keys' must be a non-empty array.");
    checkRejected(R"({"op":"distinct","mode":"tree","child":)" + kScanR + "}", ErrorCategory::Schema,
                  R"(Node 'distinct': field 'mode' must be "sort" or "hash".)");
    checkRejected(R"({"op":"distinct","child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'distinct': missing required field 'mode'.");
    checkRejected(R"({"op":"group_by","mode":"sort","keys":"R.a","aggregates":[],"child":)" + kScanR + "}",
                  ErrorCategory::Schema, "Node 'group_by': field 'keys' must be an array of strings.");
    checkRejected(R"({"op":"group_by","mode":"sort","keys":[],"aggregates":[["avg"]],"child":)" + kScanR + "}",
                  ErrorCategory::Schema, "Node 'group_by': each entry of 'aggregates' must be [function, column].");
    checkRejected(R"({"op":"group_by","mode":"sort","keys":[],"aggregates":[["sum","*"]],"child":)" + kScanR + "}",
                  ErrorCategory::Schema, "Node 'group_by': only count may use '*'.");
    checkRejected(R"({"op":"group_by","mode":"sort","keys":[],"aggregates":[["median","R.a"]],"child":)" + kScanR + "}",
                  ErrorCategory::Schema, "Node 'group_by': unknown aggregate function 'median'.");
    checkRejected(R"({"op":"limit","n":-1,"child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'limit': field 'n' must be a non-negative integer.");
    checkRejected(R"({"op":"limit","n":2147483648,"child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'limit': field 'n' must be a non-negative integer.");
    checkRejected(R"({"op":"limit","n":"3","child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Node 'limit': field 'n' must be a non-negative integer.");
    checkRejected(R"({"op":"seqscan","table":"R","foo":1})", ErrorCategory::Schema,
                  "Node 'seqscan': unexpected field 'foo'.");
}

TEST_CASE("chunk02: logical ops are not accepted in a physical plan, and the other way round")
{
    checkRejected(R"({"op":"scan","table":"R"})", ErrorCategory::Schema, "Unknown plan operator 'scan'.");
    checkRejected(R"({"op":"select","condition":"R.a = 1","child":)" + kScanR + "}", ErrorCategory::Schema,
                  "Unknown plan operator 'select'.");
    checkRejected(R"({"op":"hash_join","condition":"R.a = S.a","left":{"op":"join","condition":"R.a = S.a","left":)" +
                      kScanR + R"(,"right":)" + kScanS + R"(},"right":)" + kScanS + "}",
                  ErrorCategory::Schema, "Unknown plan operator 'join'.");

    // The logical reader keeps its own op set: no physical ops, no `mode`.
    CHECK_THROWS_AS(db::fromJsonText(kScanR), db::Error);
    CHECK_THROWS_AS(db::fromJsonText(R"({"op":"distinct","mode":"sort","child":{"op":"scan","table":"R"}})"), db::Error);
}

TEST_CASE("chunk02: malformed JSON is a syntax error with its position")
{
    const std::optional<db::Error> error = physicalError("{\"op\": \"seqscan\",\n \"table\": }");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Syntax);
    CHECK(error->line() == 2);
    CHECK(error->column() > 0);
}

TEST_CASE("chunk02: a bad condition string is rejected while reading, with its own category")
{
    const std::optional<db::Error> error =
        physicalError(R"({"op":"filter","condition":"R.a > 5 @","child":)" + kScanR + "}");
    REQUIRE(error.has_value());
    CHECK(error->category() == ErrorCategory::Lexical);
}

TEST_CASE("chunk02: plans up to kMaxNestingDepth levels are accepted; one more is rejected")
{
    CHECK_FALSE(physicalError(limitChain(db::kMaxNestingDepth)).has_value());
    checkRejected(limitChain(db::kMaxNestingDepth + 1), ErrorCategory::Schema,
                  "Node 'limit': plan nested deeper than 1000 levels.");
}
