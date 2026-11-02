// CHUNK 03 tests: divideFrames, the B-division rule (phase rule, D3 (b)).
// Every number here is a worked example from CHUNK_03 Step 5, with seqscan
// holding 1 frame. Shares are listed in pre-order: node, then its left
// subtree, then its right subtree.

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "db/common/error.h"
#include "db/common/plan.h"
#include "db/exec/frame_plan.h"

using namespace db;
using namespace db::exec;

namespace
{
    PlanNode seqscan(std::string table)
    {
        PlanNode node;
        node.op = PlanOp::SeqScan;
        node.table = std::move(table);
        return node;
    }

    // A node of `op` over the given children. divideFrames looks only at the
    // op and the children, so the other fields (keys, condition) stay empty.
    PlanNode node(PlanOp op, std::vector<PlanNode> children)
    {
        PlanNode result;
        result.op = op;
        result.children = std::move(children);
        return result;
    }

    // {openFrames, streamFrames, ownFrames}
    FrameShare share(std::size_t open, std::size_t stream, std::size_t own)
    {
        return FrameShare{open, stream, own};
    }
} // namespace

TEST_CASE("frames: sort(seqscan) at B = 4: runs of 3 pages, merges with 4")
{
    const FramePlan plan = divideFrames(node(PlanOp::Sort, {seqscan("R")}), 4);
    CHECK(plan.minimumFrames == 3);
    REQUIRE(plan.shares.size() == 2);
    CHECK(plan.shares[0] == share(4, 4, 3)); // pass 0 keeps 4 - 1 = 3
    CHECK(plan.shares[1] == share(4, 1, 1));
}

TEST_CASE("frames: nested_loop_join(seqscan, seqscan): block = B - 2")
{
    const PlanNode nlj = node(PlanOp::NestedLoopJoin, {seqscan("R"), seqscan("S")});

    const FramePlan b4 = divideFrames(nlj, 4);
    CHECK(b4.minimumFrames == 3);
    REQUIRE(b4.shares.size() == 3);
    CHECK(b4.shares[0] == share(4, 4, 2)); // 4 - 1 outer page - 1 inner page
    CHECK(b4.shares[1] == share(4, 1, 1));
    CHECK(b4.shares[2] == share(1, 1, 1)); // the inner gets exactly its need

    const FramePlan b3 = divideFrames(nlj, 3);
    CHECK(b3.shares[0] == share(3, 3, 1));
}

TEST_CASE("frames: nested_loop_join with a sort as inner: the inner keeps its whole need for every block")
{
    // The inner sort is reopened for every block, so its merge passes (3
    // frames) run while the block and the outer page are held: 1 + 1 + 3 = 5.
    const PlanNode nlj = node(PlanOp::NestedLoopJoin, {seqscan("R"), node(PlanOp::Sort, {seqscan("S")})});
    const FramePlan plan = divideFrames(nlj, 5);
    CHECK(plan.minimumFrames == 5);
    REQUIRE(plan.shares.size() == 4);
    CHECK(plan.shares[0] == share(5, 5, 1));
    CHECK(plan.shares[1] == share(5, 1, 1));
    CHECK(plan.shares[2] == share(3, 3, 2)); // the sort: runs of 2 pages, merges with 3
    CHECK(plan.shares[3] == share(3, 1, 1));
    CHECK_THROWS_AS(divideFrames(nlj, 4), Error);
}

TEST_CASE("frames: hash_join(seqscan, seqscan) at B = 4: P = B - 1 = 3, probe uses 4")
{
    const FramePlan plan = divideFrames(node(PlanOp::HashJoin, {seqscan("R"), seqscan("S")}), 4);
    CHECK(plan.minimumFrames == 2);
    REQUIRE(plan.shares.size() == 3);
    CHECK(plan.shares[0] == share(4, 4, 3));
    CHECK(plan.shares[1] == share(4, 1, 1));
    CHECK(plan.shares[2] == share(4, 1, 1));
}

TEST_CASE("frames: sort_merge_join(seqscan, seqscan) at B = 4: each sort gets 3, then 4")
{
    const FramePlan plan = divideFrames(node(PlanOp::SortMergeJoin, {seqscan("R"), seqscan("S")}), 4);
    CHECK(plan.minimumFrames == 3);
    REQUIRE(plan.shares.size() == 3);
    CHECK(plan.shares[0] == share(4, 4, 3));
    CHECK(plan.shares[1] == share(4, 1, 1));
    CHECK(plan.shares[2] == share(4, 1, 1));
}

TEST_CASE("frames: sort(hash_join(seqscan, seqscan)) at B = 4: the join streams with 2")
{
    const PlanNode hj = node(PlanOp::HashJoin, {seqscan("R"), seqscan("S")});
    const FramePlan plan = divideFrames(node(PlanOp::Sort, {hj}), 4);
    CHECK(plan.minimumFrames == 3);
    REQUIRE(plan.shares.size() == 4);
    CHECK(plan.shares[0] == share(4, 4, 2)); // pass 0 keeps 4 - 2
    CHECK(plan.shares[1] == share(4, 2, 3)); // opens with all 4 (P = 3), streams with 2
    CHECK(plan.shares[2] == share(4, 1, 1));
    CHECK(plan.shares[3] == share(4, 1, 1));
}

TEST_CASE("frames: a plan that needs more than B is rejected, naming the node and the minimum")
{
    // The filter on top needs nothing itself, so the message must name the
    // join below it.
    const PlanNode smj = node(PlanOp::SortMergeJoin, {seqscan("R"), seqscan("S")});
    const PlanNode plan = node(PlanOp::Filter, {smj});
    try
    {
        divideFrames(plan, 2);
        FAIL("expected db::Error");
    }
    catch (const Error& e)
    {
        CHECK(e.category() == ErrorCategory::Schema);
        const std::string message = e.what();
        CHECK(message.find("'sort_merge_join'") != std::string::npos);
        CHECK(message.find("at least 3 frames") != std::string::npos);
    }
    CHECK_NOTHROW(divideFrames(plan, 3));
}

TEST_CASE("frames: same plan and B give the same shares every time")
{
    const PlanNode plan = node(PlanOp::Sort,
        {node(PlanOp::HashJoin, {seqscan("R"), node(PlanOp::NestedLoopJoin, {seqscan("S"), seqscan("T")})})});
    CHECK(divideFrames(plan, 5) == divideFrames(plan, 5));
}

TEST_CASE("frames: a filter between a scan and a hash join changes nothing (pass-through)")
{
    const PlanNode plain = node(PlanOp::HashJoin, {seqscan("R"), seqscan("S")});
    const PlanNode filtered = node(PlanOp::HashJoin, {node(PlanOp::Filter, {seqscan("R")}), seqscan("S")});
    const FramePlan a = divideFrames(plain, 4);
    const FramePlan b = divideFrames(filtered, 4);
    CHECK(b.minimumFrames == a.minimumFrames);
    REQUIRE(b.shares.size() == 4);
    CHECK(b.shares[0] == a.shares[0]);     // the join
    CHECK(b.shares[1] == share(4, 1, 0));  // the filter keeps nothing
    CHECK(b.shares[2] == a.shares[1]);     // the scan under it
    CHECK(b.shares[3] == a.shares[2]);
}
