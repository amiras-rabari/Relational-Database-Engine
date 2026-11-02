#pragma once

// The pipelined operators (SPEC §M2.3): seqscan, filter, project, limit,
// union_all and the physical rename (DECISIONS 2026-11-01). None of them
// blocks; each passes one tuple at a time up the tree. Only seqscan pins a
// page (1 frame); the others hold one tuple and get 0 frames (chunk 03).
//
// Every constructor checks its names and types and sets the output schema,
// so a bad plan fails before any page is read. Private to exec: the plan
// builder (chunk 05) creates these from plan nodes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "db/common/condition.h"
#include "db/common/evaluator.h"
#include "db/common/schema.h"
#include "db/exec/exec_context.h"
#include "db/exec/operator.h"
#include "db/storage/heap_file.h"
#include "db/storage/heap_scan.h"

namespace db::exec
{
    /// Every live tuple of a stored table, in heap order, one data page
    /// pinned at a time. Unknown table -> Error(Name) "Unknown table 'X'."
    /// from the constructor. close() then open() starts again from the first
    /// tuple (nested loop rescans its inner input this way).
    class SeqScanOperator : public Operator
    {
    public:
        SeqScanOperator(ExecContext& ctx, const std::string& table);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        storage::HeapFile heap_;

        ///  the cursor that walks through the table, remembering "I'm on page 3, slot 17".
        ///  It's optional because it only exists while the operator is open
        /// Exists only between open and close. Resetting it destroys the
        /// scan, which unpins its page.
        std::optional<storage::HeapScan> scan_;
    };

    /// The child's tuples for which `condition` holds. Bound once in the
    /// constructor (Name, Ambiguity, Type errors). One comparison per tuple
    /// examined, however many and/or parts the condition has (§M2.3).
    class FilterOperator : public Operator
    {
    public:
        FilterOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const Condition& condition);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        BoundCondition condition_;
    };

    /// The listed columns of each child tuple, in the listed order.
    /// Duplicates are kept (bags, §M2.3). A column listed twice ->
    /// Error(Schema); unknown or ambiguous names -> Name / Ambiguity.
    class ProjectOperator : public Operator
    {
    public:
        ProjectOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const std::vector<ColumnRef>& columns);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// positions_[i] = where output column i is in the child's tuple.
        /// which columns of the child's row to keep, in output order. For project [R.c, R.a]
        /// over (R.a, R.b, R.c):  positions_ = {2, 0}
        std::vector<std::size_t> positions_;
        ///  a place to receive the child's full row before cutting it down
        ///  It's a member, not a local variable, so its memory is reused for every row
        ///  instead of being allocated each time.
        Tuple input_;
    };

    /// The first `count` tuples of the child. After the last one it closes
    /// the child at once and never pulls again (§M2.3 "must actually stop").
    /// count = 0: the child is never opened (chunk 04 Q1 (a)).
    class LimitOperator : public Operator
    {
    public:
        LimitOperator(ExecContext& ctx, std::unique_ptr<Operator> child, std::int32_t count);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        /// the n in limit n. It never changes
        std::int32_t count_;
        /// ow many rows it has given out so far. When returned_ == count_,
        /// it closes its child and returns END from then on.
        std::int32_t returned_ = 0;
    };

    /// Every tuple of `left`, then every tuple of `right`, duplicates kept.
    /// Both must have the same number of columns with the same types,
    /// position by position, else Error(Schema). Output names are the left's.
    /// The right child is opened only after the left one is closed, so it
    /// can use the frames the left one held (chunk 03 rule).
    class UnionAllOperator : public Operator
    {
    public:
        UnionAllOperator(ExecContext& ctx, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;

    private:
        ///   which input it's reading now. false while reading the left child,
        ///   true the left input is finished and closed, and it's reading the right one
        bool onRight_ = false;
    };

    /// The child's tuples unchanged, with every column requalified by
    /// `alias` (R.a becomes E.a), so a self-join has distinct names.
    class RenameOperator : public Operator
    {
    public:
        RenameOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const std::string& alias);

    protected:
        void doOpen() override;
        bool doNext(Tuple& out) override;
        void doClose() override;
    };
} // namespace db::exec
