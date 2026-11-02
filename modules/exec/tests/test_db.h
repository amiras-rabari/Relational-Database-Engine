#pragma once

// Test-only: a fresh storage engine in a temp folder, tables filled with
// rows from a formula (same pages on every run), and a helper that runs an
// operator to the end. Files are made only through storage.

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "db/common/schema.h"
#include "db/exec/operator.h"
#include "db/storage/heap_file.h"
#include "db/storage/replacement_policy.h"
#include "db/storage/storage_engine.h"

namespace db::exec::test
{
    class TestDb
    {
    public:
        /// A new, empty database with `frames` frames (LRU).
        explicit TestDb(const std::string& name, std::size_t frames = 4)
            : dir_(std::filesystem::temp_directory_path() / ("comp3005_exec_db_" + name)), frames_(frames)
        {
            std::filesystem::remove_all(dir_);
            engine_ = storage::StorageEngine::open(dir_, frames, storage::ReplacementPolicy::Lru);
        }

        /// Every test must end with no page pinned and no frame reserved.
        ~TestDb()
        {
            CHECK(engine_->pool().pinnedFrameCount() == 0);
            engine_.reset();
            std::error_code ignored;
            std::filesystem::remove_all(dir_, ignored);
        }

        TestDb(const TestDb&) = delete;
        TestDb& operator=(const TestDb&) = delete;

        storage::StorageEngine& engine() { return *engine_; }

        /// Creates table `name` and inserts `rows` in order.
        void createTable(const std::string& name, const Schema& schema, const std::vector<Tuple>& rows)
        {
            engine_->catalog().createTable(name, schema);
            storage::HeapFile heap = engine_->openHeapFile(name);
            for (const Tuple& row : rows)
                heap.insert(row);
            engine_->catalog().updateCounts(name, heap.numDataPages(), rows.size());
        }

        /// Saves everything and starts a new, empty pool with counters at 0,
        /// so the next scan reads every page from disk. Make operators after
        /// this call: a heap file keeps the pool it was opened with.
        void coldPool() { engine_->resetPool(frames_); }

    private:
        std::filesystem::path dir_;
        std::size_t frames_;
        std::unique_ptr<storage::StorageEngine> engine_;
    };

    /// open, next until END, close; returns every tuple.
    inline std::vector<Tuple> drain(Operator& op)
    {
        std::vector<Tuple> rows;
        op.open();
        Tuple t;
        while (op.next(t))
            rows.push_back(t);
        op.close();
        return rows;
    }
} // namespace db::exec::test
