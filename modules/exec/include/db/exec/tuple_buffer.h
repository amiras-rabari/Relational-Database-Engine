#pragma once

#include <cstddef>
#include <vector>

#include "db/common/schema.h"
#include "db/storage/buffer_pool.h"

namespace db::exec
{
    /// A list of tuples whose size is counted in pages. This is the only way
    /// an operator may keep many tuples in memory (sort runs, hash tables,
    /// the nested loop block): SPEC §M2.1 counts tuples in a C++ container
    /// against the B frames.
    ///
    /// A tuple costs what it would take on a slotted page: its record bytes
    /// (storage encodedLength) plus a 4-byte slot. A page offers
    /// 4096 - 8 = 4088 bytes, so the buffer uses ceil(total bytes / 4088)
    /// pages (open question 3 (a)). Example: 13-byte records cost 17 bytes,
    /// so one page holds 240 of them, the same as storage's recordsPerPage(13).
    ///
    /// Each page the buffer uses is a frame reserved in the buffer pool
    /// (D2 (b)), taken when the buffer grows onto a new page and given back
    /// by popBack, clear and the destructor. The pool's peak counter then
    /// includes this memory.
    ///
    /// Not copyable: two copies would give the same frames back twice.
    /// The pool and the schema must live longer than the buffer.
    class BoundedTupleBuffer
    {
    public:
        /// An empty buffer that may use up to `capacityPages` pages.
        /// Reserves nothing yet.
        BoundedTupleBuffer(storage::BufferPool& pool, const Schema& schema, std::size_t capacityPages);
        ~BoundedTupleBuffer();

        /// disable copy constructor and diables copy assignments
        BoundedTupleBuffer(const BoundedTupleBuffer&) = delete;
        BoundedTupleBuffer& operator=(const BoundedTupleBuffer&) = delete;

        /// Keeps `tuple` if the buffer still fits in its capacity with it,
        /// reserving one more frame if it starts a new page. Returns false
        /// and changes nothing if it would not fit (the caller then writes
        /// its tuples out, e.g. a sort run). A pool with no frame to reserve
        /// throws std::logic_error (the plan used more than divideFrames gave).
        bool tryAdd(Tuple tuple);

        /// Removes the last tuple. Gives a frame back when the remaining
        /// tuples fit in one page fewer. The buffer must not be empty.
        void popBack();

        /// Removes every tuple and gives every frame back.
        void clear();

        /// Pages (= reserved frames) in use now.
        std::size_t pagesUsed() const;
        /// Number of tuples kept.
        std::size_t size() const;

        /// The kept tuples, in the order added. The caller may reorder them
        /// (sort a run) but must add and remove only through this class.
        std::vector<Tuple>& tuples();

    private:
        /// What `tuple` costs on a page: record bytes + 4-byte slot.
        std::size_t bytesOnPage(const Tuple& tuple) const;

        storage::BufferPool& pool_;
        const Schema& schema_;
        std::size_t capacityPages_;
        std::vector<Tuple> tuples_;
        /// Sum of bytesOnPage over tuples_.
        std::size_t totalBytes_ = 0;
        std::size_t pagesUsed_ = 0;
    };
} // namespace db::exec
