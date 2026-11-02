#pragma once

/// @file
/// The B-division rule (SPEC §M2.1: "how B is divided among operators is
/// your decision and the design document must state the rule"). Exec uses
/// the phase rule (D3 (b)): an operator holds frames only while it is
/// working, and in each phase it gives every child that runs at the same
/// time exactly that child's minimum and keeps the rest. The full phase
/// table is in modules/exec/STATUS.md (and later DESIGN_M2.md).
///
/// divideFrames is a pure function of the plan and B: no data sizes, no
/// addresses, no clock. So the same plan at the same B always gets the same
/// shares (same counters), and M4 can call it to estimate with exactly the
/// frames exec will use (DEPENDENCIES E5).

#include <cstddef>
#include <vector>

#include "db/common/plan.h"

namespace db::exec
{
    /// The frames one plan node may use.
    struct FrameShare
    {
        /// Frames for everything the node does inside open(): its own
        /// blocking work and the children it opens. A sort's merge passes
        /// and a hash join's partitioning use these.
        std::size_t openFrames = 0;
        /// Frames for everything the node does while next() is called: its
        /// own streaming work and the children it pulls from. A sort's final
        /// merge, a hash join's probe and a nested loop's block use these.
        std::size_t streamFrames = 0;
        /// Frames the node itself may hold while its children are running:
        /// the run of a sort's pass 0, the P partition pages of a hash join,
        /// the nested loop block, the scan's one page. 0 for filter,
        /// project, limit, rename and union_all, which hold one tuple only.
        std::size_t ownFrames = 0;

        friend bool operator==(const FrameShare&, const FrameShare&) = default;
    };

    /// The shares of every node of a plan.
    struct FramePlan
    {
        /// One share per node, in pre-order: the node, then its first
        /// child's subtree, then its second child's subtree. A vector by
        /// position, not a map by address, so two runs give equal plans.
        std::vector<FrameShare> shares;
        /// The smallest B this plan runs with.
        std::size_t minimumFrames = 0;

        friend bool operator==(const FramePlan&, const FramePlan&) = default;
    };

    /// Divides `frames` (B) among the nodes of physical plan `plan`.
    /// The root gets all B frames for both open and stream.
    /// If the plan needs more than B, throws db::Error(Schema) naming the
    /// lowest node that does not fit and the smallest B the plan needs, e.g.
    /// "Node 'sort_merge_join' needs at least 3 frames; the plan needs at
    /// least 3, but B is 2." (open question 2 (a)).
    FramePlan divideFrames(const PlanNode& plan, std::size_t frames);
} // namespace db::exec
