#pragma once

#include "db/storage/buffer_pool.h"
#include "db/storage/io_stats.h"
#include "db/storage/storage_engine.h"

namespace db::exec
{
    /// What every operator in one plan shares: the storage engine (SPEC §M2.1:
    /// operators reach data only through Module 1). Make one per plan run; it
    /// must live longer than every operator that uses it.
    class ExecContext
    {
    public:
        explicit ExecContext(storage::StorageEngine& engine) : engine_(engine) {}

        // Not copyable: two copies would keep two different childIo totals.
        ExecContext(const ExecContext&) = delete;
        ExecContext& operator=(const ExecContext&) = delete;

        storage::StorageEngine& engine() const { return engine_; }

        /// Asked from the engine on every call, never kept: the engine makes a
        /// new pool object after resetPool().
        storage::BufferPool& pool() const { return engine_.pool(); }

        /// Used only by Operator to split I/O between a node and its children:
        /// the page reads/writes done by the children of the operator call
        /// that is running now. See operator.cpp.
        storage::IoStats childIo{};

    private:
        storage::StorageEngine& engine_;
    };
} // namespace db::exec
