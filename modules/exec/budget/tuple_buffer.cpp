#include "db/exec/tuple_buffer.h"

#include <utility>

#include "db/storage/page.h"
#include "db/storage/record_codec.h"
#include "db/storage/slotted_page.h"

namespace db::exec
{
    namespace
    {
        // Pages needed for `bytes` bytes of records and slots:
        // ceil(bytes / 4088), where 4088 = 4096 - 8 (page minus slotted header).
        std::size_t pagesFor(std::size_t bytes)
        {
            const std::size_t usable = storage::kPageSize - storage::kPageHeaderSize;
            return (bytes + usable - 1) / usable;
        }
    } // namespace

    BoundedTupleBuffer::BoundedTupleBuffer(storage::BufferPool& pool, const Schema& schema, std::size_t capacityPages)
        : pool_(pool), schema_(schema), capacityPages_(capacityPages)
    {
    }

    BoundedTupleBuffer::~BoundedTupleBuffer()
    {
        pool_.releaseFrames(pagesUsed_);
    }

    std::size_t BoundedTupleBuffer::bytesOnPage(const Tuple& tuple) const
    {
        return storage::encodedLength(tuple, schema_) + storage::kSlotSize;
    }

    bool BoundedTupleBuffer::tryAdd(Tuple tuple)
    {
        const std::size_t newTotal = totalBytes_ + bytesOnPage(tuple);
        const std::size_t newPages = pagesFor(newTotal);
        if (newPages > capacityPages_)
            return false;

        // Reserve before keeping the tuple: if the pool has no frame left it
        // throws, and the buffer is unchanged.
        if (newPages > pagesUsed_)
            pool_.reserveFrames(newPages - pagesUsed_);
        tuples_.push_back(std::move(tuple));
        totalBytes_ = newTotal;
        pagesUsed_ = newPages;
        return true;
    }

    void BoundedTupleBuffer::popBack()
    {
        totalBytes_ -= bytesOnPage(tuples_.back());
        // pop_back() returns void and removes the last element from the container, destroying it
        // and reducing the vector's size() by 1 in O(1) constant time.
        tuples_.pop_back();
        // Removing one tuple can free at most one page; give it back.
        const std::size_t newPages = pagesFor(totalBytes_);
        pool_.releaseFrames(pagesUsed_ - newPages);
        pagesUsed_ = newPages;
    }

    void BoundedTupleBuffer::clear()
    {
        pool_.releaseFrames(pagesUsed_);
        tuples_.clear();
        totalBytes_ = 0;
        pagesUsed_ = 0;
    }

    std::size_t BoundedTupleBuffer::pagesUsed() const
    {
        return pagesUsed_;
    }

    std::size_t BoundedTupleBuffer::size() const
    {
        return tuples_.size();
    }

    std::vector<Tuple>& BoundedTupleBuffer::tuples()
    {
        return tuples_;
    }
} // namespace db::exec
