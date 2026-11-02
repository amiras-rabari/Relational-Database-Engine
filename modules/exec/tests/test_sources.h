#pragma once

// Test-only operators. Never part of db_exec: their memory is not charged
// against B, so they must not appear in real plans.

#include <cstddef>
#include <utility>
#include <vector>

#include "db/exec/operator.h"

namespace db::exec::test
{
    /// Returns the tuples it was given, in order. Does no I/O.
    class VectorSourceOperator : public Operator
    {
    public:
        VectorSourceOperator(ExecContext& ctx, std::vector<Tuple> tuples)
            : Operator(ctx, {}), tuples_(std::move(tuples))
        {
        }

    protected:
        void doOpen() override
        {
            pos_ = 0;
            ++opens;
        }
        bool doNext(Tuple& out) override
        {
            if (pos_ == tuples_.size())
                return false;
            out = tuples_[pos_++];
            return true;
        }
        void doClose() override {}

    public:
        /// How many times open() ran (a test checks that a child was never opened).
        int opens = 0;

    private:
        std::vector<Tuple> tuples_;
        std::size_t pos_ = 0;
    };
} // namespace db::exec::test
