#pragma once

/// @file
/// The plan tree, shared by logical plans (M3) and physical plans (M2, M4):
/// the node types, their one-line text form, and the output schema of a
/// logical plan.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db/common/condition.h"
#include "db/common/schema.h"

namespace db
{

    /// What a plan node does.
    enum class PlanOp
    {
        /// Reads a stored table.
        Scan,
        /// Keeps the tuples of its child that satisfy a condition.
        Select,
        /// Keeps the listed columns of its child (duplicates stay).
        Project,
        /// Pairs of left and right tuples that satisfy a condition.
        Join,
        /// Every pair of a left and a right tuple.
        Cross,
        /// Its child with every column qualified by a new name.
        Rename,
        /// Tuples in the left or the right child.
        Union,
        /// Tuples in both children.
        Intersect,
        /// Tuples of the left child that are not in the right one.
        Except,
        /// Its child with duplicate tuples removed.
        Distinct,

        // Physical only (exec SPEC §M2.3). Project, Distinct, Union,
        // Intersect, Except and Rename above are used in physical plans too.

        /// Reads a stored table page by page.
        SeqScan,
        /// Keeps the tuples of its child that satisfy a condition.
        Filter,
        /// Its child in the order of the sort keys (external merge sort).
        Sort,
        /// One output tuple per group of equal key values, with aggregates.
        GroupBy,
        /// Block nested loop join.
        NestedLoopJoin,
        /// Sorts both inputs on the join keys, then merges them.
        SortMergeJoin,
        /// Grace hash join; the left input is the build side.
        HashJoin,
        /// Every tuple of the left child, then every tuple of the right one
        /// (duplicates kept).
        UnionAll,
        /// The first limitCount tuples of its child.
        Limit
    };

    /// The name of an operation, as plan JSON spells it. Logical:
    /// `scan select project join cross rename union intersect except
    /// distinct`. Physical adds `seqscan filter sort group_by
    /// nested_loop_join sort_merge_join hash_join union_all limit`.
    std::string_view opName(PlanOp op);

    /// How a physical distinct or group_by finds equal tuples: by sorting or
    /// by hashing. The plan chooses it (M4 decides; exec never guesses).
    enum class ExecMode
    {
        Sort,
        Hash
    };

    /// One key of a Sort: the column, and its direction.
    struct SortKey
    {
        ColumnRef column;
        bool ascending = true; // false = descending

        friend bool operator==(const SortKey&, const SortKey&) = default;
    };

    /// The functions a GroupBy can compute.
    enum class AggregateFunction
    {
        Count,
        Sum,
        Avg,
        Min,
        Max
    };

    /// One aggregate of a GroupBy, e.g. `avg(R.salary)`. An empty column
    /// means `*`, which only Count may use: `count(*)`.
    struct Aggregate
    {
        AggregateFunction function = AggregateFunction::Count;
        std::optional<ColumnRef> column;

        friend bool operator==(const Aggregate&, const Aggregate&) = default;
    };

    /// A plan tree node. Copyable, no pointers. Build nodes with the named
    /// constructors; the fields an op does not use keep their defaults.
    struct PlanNode
    {
        /// What this node does.
        PlanOp op = PlanOp::Scan;
        /// The table a Scan or SeqScan reads.
        std::string table;
        /// The condition of a Select, Join, Filter or one of the three
        /// physical joins.
        std::optional<Condition> condition;
        /// The columns of a Project, or the grouping columns of a GroupBy,
        /// as written (qualified or bare).
        std::vector<ColumnRef> columns;
        /// The new name of a Rename.
        std::string alias;
        /// Sort or hash, for a physical Distinct or GroupBy. Empty in a
        /// logical plan.
        std::optional<ExecMode> mode;
        /// The keys of a Sort, most important first.
        std::vector<SortKey> sortKeys;
        /// The aggregates of a GroupBy, in output order.
        std::vector<Aggregate> aggregates;
        /// How many tuples a Limit lets through (0 or more).
        std::int32_t limitCount = 0;
        /// Inputs: none for Scan and SeqScan; one for the ops with a `child`
        /// in JSON; left then right for the ops with `left`/`right`.
        std::vector<PlanNode> children;

        /// Reads the stored table `table`.
        static PlanNode scan(std::string table);
        /// The tuples of `child` for which `condition` holds.
        static PlanNode select(Condition condition, PlanNode child);
        /// The `columns` of `child`, in that order.
        static PlanNode project(std::vector<ColumnRef> columns, PlanNode child);
        /// The pairs of `left` and `right` tuples for which `condition` holds.
        static PlanNode join(Condition condition, PlanNode left, PlanNode right);
        /// Every pair of a `left` and a `right` tuple.
        static PlanNode cross(PlanNode left, PlanNode right);
        /// `child` with every column qualified by `alias`.
        static PlanNode rename(std::string alias, PlanNode child);
        /// `left union right` (`union` itself is a C++ keyword).
        static PlanNode unionOf(PlanNode left, PlanNode right);
        /// `left intersect right`.
        static PlanNode intersect(PlanNode left, PlanNode right);
        /// `left minus right` in Bonus 1 syntax.
        static PlanNode except(PlanNode left, PlanNode right);
        /// `child` without duplicate tuples.
        static PlanNode distinct(PlanNode child);
    };

    /// True when both trees have the same shape, ops and node data (fields a
    /// node's op does not use are ignored). Children are compared in order,
    /// so `union(A, B)` and `union(B, A)` differ.
    bool operator==(const PlanNode& left, const PlanNode& right);

    /// The plan on one line: `scan(R)`, `select[COND](CHILD)`,
    /// `project[a, b](CHILD)`, `rename[E2](CHILD)`, `join[COND](L, R)`,
    /// `cross(L, R)`, `union(L, R)`, `intersect(L, R)`, `except(L, R)`,
    /// `distinct(CHILD)`. Conditions print in `style`; column refs print as
    /// written. Logical plans only: throws std::logic_error for a
    /// physical-only op or a node without its condition.
    std::string toString(const PlanNode& plan, ConditionStyle style = ConditionStyle::Plan);

    /// The schema of the plan's result, computed bottom-up (children before
    /// the node, left before right, so the first error found is always the
    /// same):
    /// - Scan: `tableSchema(table)`; std::nullopt is Error(Name)
    ///   "Unknown relation 'X'.".
    /// - Select: the child's schema; the condition must bind to it
    ///   (bindCondition: Name, Ambiguity and Type errors).
    /// - Project: projectSchema (resolution errors, Error(Schema) for a column
    ///   named twice).
    /// - Join: concatSchemas (Error(Schema) for a qualified name on both
    ///   sides); the condition must bind with bindJoinCondition.
    /// - Cross: concatSchemas.
    /// - Rename: requalify with the alias.
    /// - Union, Intersect, Except: the left schema, after
    ///   checkUnionCompatible (Error(Schema)).
    /// - Distinct: the child's schema.
    /// Logical plans only. Throws std::logic_error for a physical-only op or
    /// a malformed node (wrong number of children, missing condition).
    Schema outputSchema(const PlanNode& plan,
                        const std::function<std::optional<Schema>(const std::string&)>& tableSchema);

} // namespace db
