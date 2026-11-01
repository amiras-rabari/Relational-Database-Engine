// The `storage` CLI (storage chunk 14) through runStorageCli with string
// streams: create-table → load → scan end to end with the exact §M1.9 JSON
// text, readable output, counters (D4, lazy --limit), determinism, and every
// exit code (usage 2, user error 1).

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "storage_cli.h"

namespace
{
    namespace fs = std::filesystem;

    // A fresh folder under the system temp directory, removed when the test
    // starts and again when it ends. Each test case uses its own name.
    class TempDir
    {
    public:
        explicit TempDir(const std::string& name)
            : path_(fs::temp_directory_path() / "comp3005_storage_cli_tests" / name)
        {
            fs::remove_all(path_);
            fs::create_directories(path_);
        }
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        const fs::path& path() const { return path_; }
        std::string db() const { return (path_ / "db").string(); }

    private:
        fs::path path_;
    };

    struct Run
    {
        int code;
        std::string out;
        std::string err;
    };

    Run run(const std::vector<std::string>& args)
    {
        std::ostringstream out;
        std::ostringstream err;
        const int code = db::apps::runStorageCli(args, out, err);
        return Run{code, out.str(), err.str()};
    }

    // Writes `text` to `<dir>/name` (binary) and returns the path.
    std::string writeFile(const TempDir& dir, const std::string& name, const std::string& text)
    {
        const fs::path path = dir.path() / name;
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path.string();
    }

    bool contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    // create-table R(a INT, b VARCHAR(20), c INT) and load `csv` into it.
    void createAndLoad(const TempDir& dir, const std::string& csv)
    {
        const Run created = run({"create-table", "--db", dir.db(), "--name", "R", "--schema", "a:INT,b:VARCHAR(20),c:INT"});
        REQUIRE(created.code == 0);
        CHECK(created.out == "created table R\n");
        CHECK(created.err.empty());
        const Run loaded = run({"load", "--db", dir.db(), "--table", "R", "--file", writeFile(dir, "r.csv", csv)});
        REQUIRE(loaded.code == 0);
        CHECK(loaded.err.empty());
    }

    const std::string kTwoRows = "42,bob,7\n43,amy,9\n";
} // namespace

TEST_CASE("storage cli: create-table, load, scan --json gives the exact §M1.9 text")
{
    TempDir dir("end_to_end");
    createAndLoad(dir, kTwoRows);

    const Run scan = run({"scan", "--db", dir.db(), "--table", "R", "--frames", "8", "--json"});
    REQUIRE(scan.code == 0);
    CHECK(scan.err.empty());
    // One data page under D4: directory page 0 (fetched twice: for entry 0,
    // then to find there is no next entry, a hit) and data page 1, so 2
    // misses and 1 hit (the catalog pages read at open are not counted).
    CHECK(scan.out
          == "{\"table\":\"R\",\"schema\":[[\"a\",\"INT\"],[\"b\",\"VARCHAR\",20],[\"c\",\"INT\"]],"
             "\"rows\":[[42,\"bob\",7],[43,\"amy\",9]],"
             "\"stats\":{\"hits\":1,\"misses\":2,\"evictions\":0,\"reads\":2,\"writes\":0}}\n");

    // The same through a JSON parser: exactly the five stats keys.
    const nlohmann::json json = nlohmann::json::parse(scan.out);
    CHECK(json.at("table") == "R");
    CHECK(json.at("rows") == nlohmann::json::parse("[[42,\"bob\",7],[43,\"amy\",9]]"));
    CHECK(json.at("stats").size() == 5);
}

TEST_CASE("storage cli: load prints the row count; scan without --json is readable text")
{
    TempDir dir("text_output");
    REQUIRE(run({"create-table", "--catalog", dir.db(), "--name", "R", "--schema", "a:INT,b:VARCHAR(20),c:INT"}).code == 0);
    const Run loaded = run({"load", "--db", dir.db(), "--table", "R", "--file", writeFile(dir, "r.csv", kTwoRows)});
    CHECK(loaded.code == 0);
    CHECK(loaded.out == "loaded 2 rows into R\n");

    const Run scan = run({"scan", "--db", dir.db(), "--table", "R"});
    CHECK(scan.code == 0);
    CHECK(scan.out
          == "table R (a INT, b VARCHAR(20), c INT)\n"
             "42 | bob | 7\n"
             "43 | amy | 9\n"
             "2 rows\n"
             "stats: hits=1 misses=2 evictions=0 reads=2 writes=0\n");
}

TEST_CASE("storage cli: --limit stops right after row N; --limit 0 reads nothing")
{
    TempDir dir("limit");
    createAndLoad(dir, kTwoRows);

    const Run one = run({"scan", "--db", dir.db(), "--table", "R", "--limit", "1", "--json"});
    CHECK(one.code == 0);
    const nlohmann::json json = nlohmann::json::parse(one.out);
    CHECK(json.at("rows") == nlohmann::json::parse("[[42,\"bob\",7]]"));
    CHECK(json.at("stats").at("reads") == 2);

    const Run zero = run({"scan", "--db", dir.db(), "--table", "R", "--limit", "0", "--json"});
    CHECK(zero.code == 0);
    CHECK(contains(zero.out, "\"rows\":[],\"stats\":{\"hits\":0,\"misses\":0,\"evictions\":0,\"reads\":0,\"writes\":0}"));
}

TEST_CASE("storage cli: a many-page scan at B = 2 has exact counters, identical on two runs; CLOCK gives the same rows")
{
    TempDir dir("deterministic");
    std::string csv;
    for (int i = 0; i < 2000; ++i)
        csv += std::to_string(i) + ",name" + std::to_string(i) + "," + std::to_string(i % 7) + "\n";
    createAndLoad(dir, csv);

    const std::vector<std::string> args = {"scan", "--db", dir.db(), "--table", "R", "--frames", "2", "--json"};
    const Run first = run(args);
    const Run second = run(args);
    REQUIRE(first.code == 0);
    CHECK(first.out == second.out);

    const nlohmann::json json = nlohmann::json::parse(first.out);
    CHECK(json.at("rows").size() == 2000);
    // Under LRU, directory page 0 stays in one frame (it is used just before
    // every data page, so it is always the newest) and the P data pages take
    // turns in the other: 1 + P misses and reads. The directory page is
    // fetched P + 1 times; all but the first are hits. Evictions = P, not
    // P - 1: open read catalog page 0 into a frame (before the snapshot), so
    // only one frame is free and data page 1 already evicts the catalog page.
    const std::uint64_t reads = json.at("stats").at("reads");
    const std::uint64_t dataPages = reads - 1;
    CHECK(dataPages > 10);
    CHECK(json.at("stats").at("misses") == reads);
    CHECK(json.at("stats").at("evictions") == dataPages);
    CHECK(json.at("stats").at("hits") == dataPages);
    CHECK(json.at("stats").at("writes") == 0);

    // CLOCK returns the same rows, and the same counters on every run. Its
    // counters differ from LRU's: at B = 2 the hand can evict the directory
    // page, which is then read again, so it never reads fewer pages.
    std::vector<std::string> clock = args;
    clock.push_back("--policy");
    clock.push_back("clock");
    const Run withClock = run(clock);
    const Run withClockAgain = run(clock);
    REQUIRE(withClock.code == 0);
    CHECK(withClock.out == withClockAgain.out);
    const nlohmann::json clockJson = nlohmann::json::parse(withClock.out);
    CHECK(clockJson.at("rows") == json.at("rows"));
    CHECK(clockJson.at("stats").at("reads").get<std::uint64_t>() >= reads);
}

TEST_CASE("storage cli: usage errors exit 2 with a message on err")
{
    TempDir dir("usage");
    const std::string db = dir.db();
    struct Case
    {
        std::vector<std::string> args;
        std::string expected;
    };
    const std::vector<Case> cases = {
        {{}, "missing command"},
        {{"drop", "--db", db}, "unknown command 'drop'"},
        {{"scan", "--table", "R"}, "missing required option --db"},
        {{"scan", "--db", db}, "missing required option --table"},
        {{"load", "--db", db, "--table", "R"}, "missing required option --file"},
        {{"create-table", "--db", db, "--name", "R"}, "missing required option --schema"},
        {{"scan", "--db", db, "--table"}, "missing value for --table"},
        {{"scan", "--db", "--table", "R"}, "missing value for --db"},
        {{"scan", "--db", db, "--table", "R", "--colour", "red"}, "unknown option --colour"},
        {{"load", "--db", db, "--table", "R", "--file", "x", "--json"}, "unknown option --json"},
        {{"load", "--db", db, "--table", "R", "--file", "x", "--limit", "3"}, "unknown option --limit"},
        {{"scan", "--db", db, "--table", "R", "R2"}, "expected an option"},
        {{"scan", "--db", db, "--catalog", db, "--table", "R"}, "given twice"},
        {{"scan", "--db", db, "--table", "R", "--frames", "0"}, "--frames must be"},
        {{"scan", "--db", db, "--table", "R", "--frames", "x"}, "--frames must be"},
        {{"load", "--db", db, "--table", "R", "--file", "x", "--frames", "-4"}, "--frames must be"},
        {{"scan", "--db", db, "--table", "R", "--limit", "-1"}, "--limit must be"},
        {{"scan", "--db", db, "--table", "R", "--policy", "LRU"}, "--policy must be lru or clock"},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.expected);
        const Run r = run(c.args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.rfind("usage error: ", 0) == 0);
        CHECK(contains(r.err, c.expected));
        CHECK(contains(r.err, "usage:"));
    }
    // No usage error created the database folder.
    CHECK_FALSE(fs::exists(db));
}

TEST_CASE("storage cli: unknown table, bad schema, duplicate table and missing file exit 1")
{
    TempDir dir("user_errors");
    createAndLoad(dir, kTwoRows);

    Run r = run({"scan", "--db", dir.db(), "--table", "Nope", "--json"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "error: name: Unknown table 'Nope'.\n");

    r = run({"create-table", "--db", dir.db(), "--name", "S", "--schema", "a:FLOAT"});
    CHECK(r.code == 1);
    CHECK(r.err.rfind("error: schema: Schema item 1 'a:FLOAT'", 0) == 0);

    r = run({"create-table", "--db", dir.db(), "--name", "R", "--schema", "a:INT"});
    CHECK(r.code == 1);
    CHECK(r.err == "error: schema: Table 'R' already exists.\n");

    r = run({"load", "--db", dir.db(), "--table", "R", "--file", (dir.path() / "missing.csv").string()});
    CHECK(r.code == 1);
    CHECK(r.err.rfind("error: name: Cannot open CSV file", 0) == 0);
}

TEST_CASE("storage cli: a bad CSV line exits 1 naming line and column; earlier rows stay")
{
    TempDir dir("bad_csv");
    REQUIRE(run({"create-table", "--db", dir.db(), "--name", "R", "--schema", "a:INT,b:VARCHAR(20),c:INT"}).code == 0);
    const Run loaded =
        run({"load", "--db", dir.db(), "--table", "R", "--file", writeFile(dir, "r.csv", "42,bob,7\n43,amy,9\nx,bob,7\n")});
    CHECK(loaded.code == 1);
    CHECK(loaded.out.empty());
    CHECK(loaded.err.rfind("error: type: line 3, column 1 (a): ", 0) == 0);
    CHECK(contains(loaded.err, "at line 3, column 1"));

    const Run scan = run({"scan", "--db", dir.db(), "--table", "R", "--json"});
    CHECK(scan.code == 0);
    CHECK(nlohmann::json::parse(scan.out).at("rows").size() == 2);
}

TEST_CASE("storage cli: scan with --frames 1 is a clear error, exit 1")
{
    TempDir dir("one_frame");
    createAndLoad(dir, kTwoRows);
    const Run r = run({"scan", "--db", dir.db(), "--table", "R", "--frames", "1", "--json"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.rfind("error: a scan needs at least 2 frames", 0) == 0);
    // A load pins two pages too; create-table needs only one.
    const Run load = run({"load", "--db", dir.db(), "--table", "R", "--file", writeFile(dir, "more.csv", "1,x,1\n"),
        "--frames", "1"});
    CHECK(load.code == 1);
    CHECK(load.err.rfind("error: a load needs at least 2 frames", 0) == 0);
    CHECK(run({"create-table", "--db", dir.db(), "--name", "S", "--schema", "a:INT", "--frames", "1"}).code == 0);
}

TEST_CASE("storage cli: layout reproduces the §M1.4 page (rows 2-4 of the worked table)")
{
    TempDir dir("layout_m14");
    // 13, 20 and 13 bytes, as in the §M1.4 example.
    createAndLoad(dir, "42,bob,7\n1,abcdefghij,2\n43,amy,9\n");

    const Run json = run({"layout", "--db", dir.db(), "--table", "R", "--page", "1", "--json"});
    REQUIRE(json.code == 0);
    CHECK(json.out
          == "{\"table\":\"R\",\"page_id\":1,\"kind\":\"data\",\"slot_count\":3,\"free_end\":4050,\"free_space\":4030,"
             "\"slots\":[{\"slot\":0,\"offset\":4083,\"length\":13,\"live\":true},"
             "{\"slot\":1,\"offset\":4063,\"length\":20,\"live\":true},"
             "{\"slot\":2,\"offset\":4050,\"length\":13,\"live\":true}]}\n");

    const Run text = run({"layout", "--db", dir.db(), "--table", "R", "--page", "1"});
    REQUIRE(text.code == 0);
    CHECK(text.out
          == "table R, page 1 of 2 (data page)\n"
             "offset  size  field         value\n"
             "     0     4  page_id       1\n"
             "     4     2  slot_count    3\n"
             "     6     2  free_end      4050\n"
             "free space = free_end - (8 + 4 x slot_count) = 4030 bytes\n"
             "slot  at  offset  length  state  record\n"
             "   0   8    4083      13  live   bytes 4083..4095  (42, 'bob', 7)\n"
             "   1  12    4063      20  live   bytes 4063..4082  (1, 'abcdefghij', 2)\n"
             "   2  16    4050      13  live   bytes 4050..4062  (43, 'amy', 9)\n");
}

TEST_CASE("storage cli: layout of directory page 0, and its errors")
{
    TempDir dir("layout_dir");
    createAndLoad(dir, kTwoRows);

    const Run text = run({"layout", "--db", dir.db(), "--table", "R", "--page", "0"});
    REQUIRE(text.code == 0);
    CHECK(text.out
          == "table R, page 0 of 2 (directory page)\n"
             "offset  size  field         value\n"
             "     0     4  page_id       0\n"
             "     4     4  next_dir_page none (0xFFFFFFFF)\n"
             "     8     2  entry_count   1\n"
             "entry  at  data_page  free_bytes\n"
             "    0  10          1        4054\n");

    const Run json = run({"layout", "--db", dir.db(), "--table", "R", "--page", "0", "--json"});
    CHECK(json.out
          == "{\"table\":\"R\",\"page_id\":0,\"kind\":\"directory\",\"next_dir_page\":4294967295,\"entry_count\":1,"
             "\"entries\":[{\"index\":0,\"data_page_id\":1,\"free_bytes\":4054}]}\n");

    const Run past = run({"layout", "--db", dir.db(), "--table", "R", "--page", "2"});
    CHECK(past.code == 1);
    CHECK(contains(past.err, "error: name:"));
    CHECK(run({"layout", "--db", dir.db(), "--table", "S", "--page", "0"}).code == 1);
    CHECK(run({"layout", "--db", dir.db(), "--table", "R", "--page", "-1"}).code == 2);
    CHECK(run({"layout", "--db", dir.db(), "--table", "R"}).code == 2);
}

TEST_CASE("storage cli: trace reproduces the §M1.6 example with the exact JSON, both policies")
{
    TempDir dir("trace_m16");
    // Blank lines, spaces, CRLF and no final newline are all fine (15d).
    const std::string file = writeFile(dir, "t.txt", "1\n2\r\n\n 3 \n1\n4\n2");

    const Run lru = run({"trace", "--db", dir.db(), "--file", file, "--frames", "3", "--policy", "lru", "--json"});
    REQUIRE(lru.code == 0);
    CHECK(lru.out
          == "{\"policy\":\"lru\",\"frames\":3,\"hits\":1,\"misses\":5,\"evictions\":2,\"reads\":5,\"writes\":0}\n");

    const Run clock = run({"trace", "--db", dir.db(), "--file", file, "--frames", "3", "--policy", "clock", "--json"});
    REQUIRE(clock.code == 0);
    CHECK(clock.out
          == "{\"policy\":\"clock\",\"frames\":3,\"hits\":2,\"misses\":4,\"evictions\":1,\"reads\":4,\"writes\":0}\n");

    // Same command twice, same counters; the default policy is LRU.
    CHECK(run({"trace", "--db", dir.db(), "--file", file, "--frames", "3", "--json"}).out == lru.out);
    // The scratch file is gone afterwards.
    CHECK_FALSE(fs::exists(fs::path(dir.db()) / "tmp" / "trace_scratch.db"));

    const Run text = run({"trace", "--db", dir.db(), "--file", file, "--frames", "3"});
    CHECK(text.code == 0);
    CHECK(contains(text.out, "hits=1 misses=5 evictions=2 reads=5 writes=0"));
}

TEST_CASE("storage cli: trace errors")
{
    TempDir dir("trace_errors");
    const Run bad = run({"trace", "--db", dir.db(), "--file", writeFile(dir, "bad.txt", "1\n2\nx7\n"), "--json"});
    CHECK(bad.code == 1);
    CHECK(contains(bad.err, "error: syntax:"));
    CHECK(contains(bad.err, "at line 3"));
    CHECK(run({"trace", "--db", dir.db(), "--file", writeFile(dir, "neg.txt", "-1\n")}).code == 1);
    CHECK(run({"trace", "--db", dir.db(), "--file", (dir.path() / "missing.txt").string()}).code == 1);
    CHECK(run({"trace", "--db", dir.db(), "--file", writeFile(dir, "ok.txt", "1\n"), "--policy", "fifo"}).code == 2);
    CHECK(run({"trace", "--db", dir.db(), "--file", writeFile(dir, "ok2.txt", "1\n"), "--frames", "0"}).code == 2);
    CHECK(run({"trace", "--db", dir.db()}).code == 2);
}
