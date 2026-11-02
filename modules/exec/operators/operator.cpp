#include "db/exec/operator.h"

#include <utility>

#include "db/storage/buffer_pool.h"
#include "db/storage/io_stats.h"

namespace db::exec
{
    Operator::Operator(ExecContext& ctx, Children children) : ctx_(ctx), children_(std::move(children)) {}

    // How each node gets only its OWN page reads and writes.
    //
    // The pool's counters only grow. So "pool after - pool before" around one
    // call is all the I/O of that call, including what our children did
    // while we called them. We must take the children's part away.
    //
    // ctx.childIo is a running total: every call, when it finishes, adds its
    // whole I/O (its own + its children's) to it. So during our call it adds
    // up exactly what our children did, and we subtract that.
    //
    // Example: parent.next() calls child.next(), which reads 1 page; then the
    // parent reads 1 page itself.
    //   parent starts:  remembers childIo (say x), sets childIo = 0
    //   child starts:   remembers 0, sets childIo = 0
    //   child finishes: whole = 1, own = 1 - 0 = 1, childIo = 0 + 1 = 1
    //   parent finishes: whole = 2, own = 2 - childIo(1) = 1, childIo = x + 2
    // Each node is charged 1, and the two add up to the 2 reads the disk did.

    Operator::CallStart Operator::startCall()
    {
        CallStart start{ctx_.childIo, ctx_.pool().stats()};
        ctx_.childIo = storage::IoStats{}; // from now on it counts only our children
        return start;
    }

    void Operator::finishCall(const CallStart& start)
    {
        const storage::IoStats whole = ctx_.pool().stats() - start.poolBefore; // us + our children
        const storage::IoStats own = whole - ctx_.childIo;                     // us alone
        counters_.pageReads += own.reads;
        counters_.pageWrites += own.writes;
        ctx_.childIo = start.callerChildIo + whole; // our caller subtracts all of it
    }

    void Operator::open()
    {
        const CallStart start = startCall();
        doOpen();
        finishCall(start);
        isOpen_ = true;
    }

    bool Operator::next(Tuple& out)
    {
        const CallStart start = startCall();
        const bool produced = doNext(out);
        finishCall(start);
        if (produced)
            ++counters_.tuplesOut; // counted here, so operators never count their own output
        return produced;
    }

    void Operator::close()
    {
        if (!isOpen_)
            return; // second close: nothing left to release
        const CallStart start = startCall();
        doClose();
        finishCall(start);
        isOpen_ = false;
    }

    ExecCounters subtreeCounters(const Operator& op)
    {
        ExecCounters sum = op.counters();
        for (const auto& c : op.children())
            sum = sum + subtreeCounters(*c);
        return sum;
    }
} // namespace db::exec
