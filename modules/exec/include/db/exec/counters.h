#pragma once

#include <cstdint>

namespace db::exec
{
    /// The four per-operator counters of SPEC §M2.1. Each operator stores only
    /// its OWN work here (not its children's), so the plan's totals are the
    /// plain sum over all nodes. Named ExecCounters, not OperatorCounters,
    /// because common already has an OperatorCounters with other fields.
    /// uint64_t because comparisons can pass 2^32 on a large sort.
    struct ExecCounters
    {
        std::uint64_t tuplesOut = 0;   // tuples this operator returned from next()
        std::uint64_t comparisons = 0; // conditions evaluated / keys compared
        std::uint64_t pageReads = 0;   // disk reads caused by this operator itself
        std::uint64_t pageWrites = 0;  // disk writes caused by this operator itself

        bool operator==(const ExecCounters&) const = default;
    };

    /// Field-by-field sum (plan totals, subtree sums).
    inline ExecCounters operator+(const ExecCounters& a, const ExecCounters& b)
    {
        return ExecCounters{a.tuplesOut + b.tuplesOut, a.comparisons + b.comparisons,
                            a.pageReads + b.pageReads, a.pageWrites + b.pageWrites};
    }
} // namespace db::exec
