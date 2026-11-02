// CHUNK 03 tests: BoundedTupleBuffer, the only way an operator may keep many
// tuples in memory. Its size is counted in pages, and each page it uses is a
// frame reserved in the buffer pool (D2 (b)).

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "db/common/schema.h"
#include "db/common/value.h"
#include "db/exec/tuple_buffer.h"
#include "db/storage/buffer_pool.h"
#include "db/storage/file_registry.h"
#include "db/storage/replacement_policy.h"

using namespace db;
using namespace db::exec;

namespace
{
    // Two INTs and a VARCHAR(10): with a 3-byte string the record is
    // 4 + 4 + (2 + 3) = 13 bytes, plus a 4-byte slot = 17 bytes on a page.
    // 240 * 17 = 4080 fits in 4088 bytes; 241 * 17 = 4097 does not.
    Schema smallSchema()
    {
        return Schema({{"R", "a", Type::Int, 0}, {"R", "b", Type::Int, 0}, {"R", "c", Type::Varchar, 10}});
    }

    Tuple smallTuple(int i)
    {
        return {Value::ofInt(i), Value::ofInt(i), Value::ofVarchar("abc")};
    }

    // A pool with 4 frames. The buffer only reserves frames, so no file is needed.
    struct Pool
    {
        storage::FileRegistry files;
        storage::BufferPool pool{4, storage::ReplacementPolicy::Lru, files};
    };
} // namespace

TEST_CASE("buffer: one page holds 240 tuples of 13 bytes, the 241st is refused")
{
    Pool p;
    const Schema schema = smallSchema();
    BoundedTupleBuffer buffer(p.pool, schema, 1);
    for (int i = 0; i < 240; ++i)
        REQUIRE(buffer.tryAdd(smallTuple(i)));
    CHECK_FALSE(buffer.tryAdd(smallTuple(240)));
    CHECK(buffer.size() == 240); // the refused tuple was not kept
    CHECK(buffer.pagesUsed() == 1);
    CHECK(p.pool.pinnedFrameCount() == 1); // one frame reserved in the pool
}

TEST_CASE("buffer: two pages hold 480 tuples; frames are reserved one page at a time")
{
    Pool p;
    const Schema schema = smallSchema();
    BoundedTupleBuffer buffer(p.pool, schema, 2);
    CHECK(buffer.pagesUsed() == 0); // nothing reserved before the first tuple
    CHECK(p.pool.pinnedFrameCount() == 0);
    for (int i = 0; i < 480; ++i)
    {
        REQUIRE(buffer.tryAdd(smallTuple(i)));
        if (i == 239)
            CHECK(p.pool.pinnedFrameCount() == 1);
    }
    CHECK_FALSE(buffer.tryAdd(smallTuple(480)));
    CHECK(buffer.pagesUsed() == 2);
    CHECK(p.pool.pinnedFrameCount() == 2);
}

TEST_CASE("buffer: popBack and clear give frames back; the destructor too")
{
    Pool p;
    const Schema schema = smallSchema();
    {
        BoundedTupleBuffer buffer(p.pool, schema, 2);
        for (int i = 0; i < 241; ++i)
            REQUIRE(buffer.tryAdd(smallTuple(i)));
        CHECK(buffer.pagesUsed() == 2); // 241 * 17 = 4097 bytes

        buffer.popBack(); // 240 * 17 = 4080 bytes: one page again
        CHECK(buffer.pagesUsed() == 1);
        CHECK(buffer.size() == 240);
        CHECK(p.pool.pinnedFrameCount() == 1);
        CHECK(buffer.tuples().back() == smallTuple(239));

        buffer.clear();
        CHECK(buffer.pagesUsed() == 0);
        CHECK(buffer.size() == 0);
        CHECK(p.pool.pinnedFrameCount() == 0);

        REQUIRE(buffer.tryAdd(smallTuple(0)));
        CHECK(p.pool.pinnedFrameCount() == 1);
    }
    CHECK(p.pool.pinnedFrameCount() == 0); // the destructor released its frame
    CHECK(p.pool.peakPinnedCount() == 2);
}
