#include "operators/pipelined.h"

#include <utility>

#include "db/storage/rid.h"

namespace db::exec
{
    namespace
    {
        // The base class takes a list of children; most operators here have one.
        Operator::Children oneChild(std::unique_ptr<Operator> child)
        {
            // Children is alias for std::vector<std::unique_ptr<Operator>>
            Operator::Children children;
            children.push_back(std::move(child));
            return children;
        }

        Operator::Children twoChildren(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right)
        {
            Operator::Children children;
            children.push_back(std::move(left));
            children.push_back(std::move(right));
            return children;
        }
    } // namespace

    // ---------------------------------------------------------------- seqscan

    // openHeapFile throws Error(Name) for an unknown table and reads nothing,
    // so the check happens here, before open().
    SeqScanOperator::SeqScanOperator(ExecContext& ctx, const std::string& table)
        : Operator(ctx, {}), heap_(ctx.engine().openHeapFile(table))
    {
        // Already qualified with the table name (R.a, R.b, ...).
        setSchema(heap_.schema());
    }

    void SeqScanOperator::doOpen()
    {
        // A new scan starts at the first data page. It pins nothing until
        // the first next().
        // If an old cursor was still there (open called again), assigning a new one destroys the old one first,
        // so a reopen always starts from row 1.
        scan_ = heap_.scan();
    }

    bool SeqScanOperator::doNext(Tuple& out)
    {
        storage::Rid rid; // the scan reports it; seqscan does not need it
        return scan_->next(rid, out);
    }

    void SeqScanOperator::doClose()
    {
        // reset() destroys the HeapScan inside the optional, which becomes empty.
        //  HeapScan's destructor unpins whatever page it still holds
        scan_.reset();
    }

    // ---------------------------------------------------------------- filter

    FilterOperator::FilterOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const Condition& condition)
        : Operator(ctx, oneChild(std::move(child))), condition_(bindCondition(condition, this->child(0).schema()))
    {
        setSchema(this->child(0).schema());
    }

    void FilterOperator::doOpen()
    {
        child(0).open();
    }

    bool FilterOperator::doNext(Tuple& out)
    {
        // Pull until a tuple passes. Every tuple looked at is one comparison,
        // whether it passes or not.
        while (child(0).next(out))
        {
            ++mutableCounters().comparisons;
            if (condition_.evaluate(out))
                return true;
        }
        return false;
    }

    void FilterOperator::doClose()
    {
        child(0).close();
    }

    // ---------------------------------------------------------------- project

    ProjectOperator::ProjectOperator(ExecContext& ctx, std::unique_ptr<Operator> child,
                                     const std::vector<ColumnRef>& columns)
        : Operator(ctx, oneChild(std::move(child)))
    {
        // Resolves every name and fills positions_ in one step.
        setSchema(projectSchema(this->child(0).schema(), columns, &positions_));
    }

    void ProjectOperator::doOpen()
    {
        child(0).open();
    }

    bool ProjectOperator::doNext(Tuple& out)
    {
        if (!child(0).next(input_))
            return false;
        out.clear();
        // projectSchema rejects a column listed twice, so each value is used
        // once and can be moved instead of copied.
        for (std::size_t position : positions_)
            out.push_back(std::move(input_[position]));
        return true;
    }

    void ProjectOperator::doClose()
    {
        child(0).close();
    }

    // ---------------------------------------------------------------- limit

    LimitOperator::LimitOperator(ExecContext& ctx, std::unique_ptr<Operator> child, std::int32_t count)
        : Operator(ctx, oneChild(std::move(child))), count_(count)
    {
        setSchema(this->child(0).schema());
    }

    void LimitOperator::doOpen()
    {
        returned_ = 0;
        // limit 0 returns nothing, so the child is never opened: no I/O.
        if (count_ > 0)
            child(0).open();
    }

    bool LimitOperator::doNext(Tuple& out)
    {
        if (returned_ == count_)
            return false;
        if (!child(0).next(out))
            return false;
        ++returned_;
        // The last tuple: close the child now, so it reads no more pages and
        // releases its frames at once. Our own close() will find it closed.
        if (returned_ == count_)
            child(0).close();
        return true;
    }

    void LimitOperator::doClose()
    {
        child(0).close(); // does nothing if already closed or never opened
    }

    // ---------------------------------------------------------------- union_all

    UnionAllOperator::UnionAllOperator(ExecContext& ctx, std::unique_ptr<Operator> left,
                                       std::unique_ptr<Operator> right)
        : Operator(ctx, twoChildren(std::move(left), std::move(right)))
    {
        checkUnionCompatible(child(0).schema(), child(1).schema(), "union_all");
        setSchema(child(0).schema()); // §M2.3: output takes the left input's names
    }

    void UnionAllOperator::doOpen()
    {
        onRight_ = false;
        child(0).open(); // the right child waits until the left one is finished
    }

    bool UnionAllOperator::doNext(Tuple& out)
    {
        if (!onRight_)
        {
            if (child(0).next(out))
                return true;
            // Left finished: close it first, so its frames are free before
            // the right child opens.
            child(0).close();
            child(1).open();
            onRight_ = true;
        }
        // Right tuples need no change: names live in the schema, not the tuple.
        return child(1).next(out);
    }

    void UnionAllOperator::doClose()
    {
        child(0).close();
        child(1).close();
    }

    // ---------------------------------------------------------------- rename

    RenameOperator::RenameOperator(ExecContext& ctx, std::unique_ptr<Operator> child, const std::string& alias)
        : Operator(ctx, oneChild(std::move(child)))
    {
        setSchema(requalify(this->child(0).schema(), alias));
    }

    void RenameOperator::doOpen()
    {
        child(0).open();
    }

    bool RenameOperator::doNext(Tuple& out)
    {
        return child(0).next(out);
    }

    void RenameOperator::doClose()
    {
        child(0).close();
    }
} // namespace db::exec
