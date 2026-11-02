#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "db/common/schema.h"
#include "db/exec/counters.h"
#include "db/exec/exec_context.h"
#include "db/storage/io_stats.h"

namespace db::exec
{
    /// One node of a physical plan (SPEC §M2.1): open, next, close, counters.
    ///
    /// How the work is split between this base class and each operator:
    /// - Each operator (seqscan, filter, hash_join, ...) writes only its own
    ///   work, in doOpen(), doNext() and doClose().
    /// - The public open(), next() and close() are written once, here. Each
    ///   one notes the pool's I/O counters, runs the operator's own doX(),
    ///   then charges this node the pages it read and wrote itself. next()
    ///   also counts the tuples that come out.
    /// So no operator can forget to count its pages or its output tuples.
    /// EXAMPLE
    /* This is the example plan from modules/exec/SPEC.md:41-49:

    {
        "op": "hash_join",
        "condition": "R.b = S.b",
        "left":  { "op": "filter",
                   "condition": "R.a > 500",
                   "child": { "op": "seqscan", "table": "R" } },
        "right": { "op": "seqscan", "table": "S" }
    }
    */
    ///
    ///void open();
    /// The shell calls root.open(). Each operator opens its own children inside its doOpen():
    /// - hash_join.open() → filter.open() → scanR.open()
    /// - The hash join does its blocking work here: it pulls every tuple from filter
    /// and from scanS and writes them to partition files.
    ///
    /// bool next(Tuple& out);
    /// The shell repeatedly calls root.next(t). Each call pulls one tuple up the tree:
    /// - hash_join.next returns one joined row, read from its partitions.
    /// - During open, filter.next was called many times. Each call loops on scanR.next until
    /// a row has R.a > 500, then returns that row.
    /// - false means END: there are no more rows.
    ///
    /// void close();
    /// root.close() closes the whole tree. The hash join deletes its temp files and closes
    /// its children,
    /// and each scan releases its pin. The isOpen_ flag makes a second close do nothing.


    class Operator
    {
    public:
        // This is the list of a node's inputs eg. unique_ptr means the parent owns its children:
        // destroying the hash join destroys the whole tree alias as children
        using Children = std::vector<std::unique_ptr<Operator>>;

        Operator(ExecContext& ctx, Children children);
        // Members clean up after themselves (PageGuard, TempFile, children),
        // so destroying an operator releases every pin even without close().
        // If an error stops the plan halfway, destroying hash_join destroys everything below it
        // since we have virtual function descructor must be marked virtual as well
        virtual ~Operator() = default;

        // Not copyable: a copy would own the same pins and children twice.
        Operator(const Operator&) = delete;
        Operator& operator=(const Operator&) = delete;

        /// Prepare and do any blocking work.
        void open();
        /// Fill `out` with the next tuple and return true, or return false at END.
        bool next(Tuple& out);
        /// Release every pin and temp file. A second close does nothing.
        void close();

        /// The work this node did itself (not its children); still readable
        /// after close().
        const ExecCounters& counters() const { return counters_; }
        const Children& children() const { return children_; }
        /// The columns of the tuples this operator returns. A parent binds
        /// its own column names against its child's schema().
        const Schema& schema() const { return schema_; }

    protected:
        // The operator's own work. Each operator writes these three; only
        // open(), next() and close() above call them.
        // Using virtual enables subclasses to override base functionality
        // so that polymorphic code operates on derived behavior through base class interfaces.
        virtual void doOpen() = 0;
        virtual bool doNext(Tuple& out) = 0;
        virtual void doClose() = 0;

        ExecContext& ctx() const { return ctx_; }
        Operator& child(std::size_t i) const { return *children_[i]; }
        /// Operators add their own comparisons here.
        ExecCounters& mutableCounters() { return counters_; }
        /// Each operator calls this once, in its constructor, after it has
        /// checked its names and types (so errors come before any page is read).
        void setSchema(Schema schema) { schema_ = std::move(schema); }

    private:
        /// What startCall() notes, for finishCall() to use.
        struct CallStart
        {
            storage::IoStats callerChildIo; // our caller's running total, put back at the end
            storage::IoStats poolBefore; // the pool's counters before our own work ran
        };

        /// Called first in open()/next()/close().
        CallStart startCall();
        /// Called last in open()/next()/close(): adds this node's own page
        /// reads and writes to its counters.
        void finishCall(const CallStart& start);

        ExecContext& ctx_;
        Children children_;
        ExecCounters counters_;
        Schema schema_;
        bool isOpen_ = false;
    };

    /// The counters of `op` added to those of every node below it. The trace
    /// prints these page reads/writes (D1 (b)): for a join that equals the
    /// textbook cost, which includes reading its inputs (SPEC §M2.5,
    /// hash_join reads=96).
    ExecCounters subtreeCounters(const Operator& op);
} // namespace db::exec
