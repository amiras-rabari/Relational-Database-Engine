#include "db/common/plan_json.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "condition_parser.h"
#include "db/common/error.h"
#include "db/common/limits.h"
#include "db/common/tokenizer.h"

namespace db
{

    namespace
    {

        // The plan JSON field names. Shown in spec examples: op, table,
        // condition, columns, child, left/right of join, to. The fields of
        // cross, the set operations and distinct follow the spec's pattern
        // (two inputs use left/right, one input uses child).
        constexpr const char* kOp = "op";
        constexpr const char* kTable = "table";
        constexpr const char* kCondition = "condition";
        constexpr const char* kColumns = "columns";
        constexpr const char* kChild = "child";
        constexpr const char* kLeft = "left";
        constexpr const char* kRight = "right";
        constexpr const char* kTo = "to";
        // Physical plans (exec SPEC §M2.3). `n` (limit's count) is our choice:
        // the spec leaves that name to the student (DECISIONS.md 2026-11-01).
        constexpr const char* kKeys = "keys";
        constexpr const char* kAggregates = "aggregates";
        constexpr const char* kMode = "mode";
        constexpr const char* kN = "n";

        // Values that appear inside fields.
        constexpr const char* kModeSort = "sort";
        constexpr const char* kModeHash = "hash";
        constexpr const char* kAscending = "asc";
        constexpr const char* kDescending = "desc";
        constexpr const char* kStar = "*"; // the column of count(*)

        // The ops each reader accepts. A logical op in a physical plan (or the
        // other way round) is an unknown operator for that reader.
        constexpr PlanOp kLogicalOps[] = {PlanOp::Scan,   PlanOp::Select, PlanOp::Project,   PlanOp::Join,
                                          PlanOp::Cross,  PlanOp::Rename, PlanOp::Union,     PlanOp::Intersect,
                                          PlanOp::Except, PlanOp::Distinct};
        // Rename is our extension: a self-join needs two names for one table,
        // and the spec's physical list has no rename (DECISIONS.md 2026-11-01).
        constexpr PlanOp kPhysicalOps[] = {PlanOp::SeqScan,  PlanOp::Filter,         PlanOp::Project,
                                           PlanOp::Distinct, PlanOp::Sort,           PlanOp::GroupBy,
                                           PlanOp::NestedLoopJoin, PlanOp::SortMergeJoin, PlanOp::HashJoin,
                                           PlanOp::UnionAll, PlanOp::Union,          PlanOp::Intersect,
                                           PlanOp::Except,   PlanOp::Limit,          PlanOp::Rename};

        constexpr AggregateFunction kAllFunctions[] = {AggregateFunction::Count, AggregateFunction::Sum,
                                                       AggregateFunction::Avg, AggregateFunction::Min,
                                                       AggregateFunction::Max};

        const char* functionName(AggregateFunction function)
        {
            switch (function)
            {
                case AggregateFunction::Count:
                    return "count";
                case AggregateFunction::Sum:
                    return "sum";
                case AggregateFunction::Avg:
                    return "avg";
                case AggregateFunction::Min:
                    return "min";
                case AggregateFunction::Max:
                    return "max";
            }
            throw std::logic_error("functionName: unknown AggregateFunction value");
        }

        // The ops whose node carries a `condition`.
        bool hasCondition(PlanOp op)
        {
            return op == PlanOp::Select || op == PlanOp::Join || op == PlanOp::Filter ||
                   op == PlanOp::NestedLoopJoin || op == PlanOp::SortMergeJoin || op == PlanOp::HashJoin;
        }

        // The fields of each op besides `op`, in the order they are written.
        // Distinct is the one op whose fields differ between the two plan
        // kinds: only the physical form has `mode`.
        std::vector<const char*> fieldsOf(PlanOp op, bool physical)
        {
            switch (op)
            {
                case PlanOp::Scan:
                case PlanOp::SeqScan:
                    return {kTable};
                case PlanOp::Select:
                case PlanOp::Filter:
                    return {kCondition, kChild};
                case PlanOp::Project:
                    return {kColumns, kChild};
                case PlanOp::Join:
                case PlanOp::NestedLoopJoin:
                case PlanOp::SortMergeJoin:
                case PlanOp::HashJoin:
                    return {kCondition, kLeft, kRight};
                case PlanOp::Rename:
                    return {kTo, kChild};
                case PlanOp::Distinct:
                    if (physical)
                    {
                        return {kMode, kChild};
                    }
                    return {kChild};
                case PlanOp::Sort:
                    return {kKeys, kChild};
                case PlanOp::GroupBy:
                    return {kMode, kKeys, kAggregates, kChild};
                case PlanOp::Limit:
                    return {kN, kChild};
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                case PlanOp::UnionAll:
                    return {kLeft, kRight};
            }
            throw std::logic_error("fieldsOf: unknown PlanOp value");
        }

        // The op called `name`, if the reader for this plan kind accepts it.
        std::optional<PlanOp> opFromName(const std::string& name, bool physical)
        {
            if (physical)
            {
                for (PlanOp op : kPhysicalOps)
                {
                    if (opName(op) == name)
                    {
                        return op;
                    }
                }
                return std::nullopt;
            }
            for (PlanOp op : kLogicalOps)
            {
                if (opName(op) == name)
                {
                    return op;
                }
            }
            return std::nullopt;
        }

        std::size_t inputCount(PlanOp op)
        {
            switch (op)
            {
                case PlanOp::Scan:
                case PlanOp::SeqScan:
                    return 0;
                case PlanOp::Select:
                case PlanOp::Project:
                case PlanOp::Rename:
                case PlanOp::Distinct:
                case PlanOp::Filter:
                case PlanOp::Sort:
                case PlanOp::GroupBy:
                case PlanOp::Limit:
                    return 1;
                case PlanOp::Join:
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                case PlanOp::NestedLoopJoin:
                case PlanOp::SortMergeJoin:
                case PlanOp::HashJoin:
                case PlanOp::UnionAll:
                    return 2;
            }
            throw std::logic_error("inputCount: unknown PlanOp value");
        }

        // --- Writing -------------------------------------------------------

        // Fills `out` in place, so a recursion level holds only references.
        void writeNode(const PlanNode& node, PlanJson& out)
        {
            if (node.children.size() != inputCount(node.op))
            {
                throw std::logic_error("toJson: plan node '" + std::string(opName(node.op)) +
                                       "' has the wrong number of inputs");
            }
            if (hasCondition(node.op) && !node.condition)
            {
                throw std::logic_error("toJson: plan node '" + std::string(opName(node.op)) +
                                       "' has no condition");
            }

            out = PlanJson::object();
            out[kOp] = opName(node.op);
            // `mode` comes first after `op` (spec order). Only physical
            // distinct/group_by nodes have one.
            if (node.mode)
            {
                out[kMode] = *node.mode == ExecMode::Sort ? kModeSort : kModeHash;
            }
            switch (node.op)
            {
                case PlanOp::Scan:
                case PlanOp::SeqScan:
                    out[kTable] = node.table;
                    return;
                case PlanOp::Select:
                case PlanOp::Filter:
                    out[kCondition] = toString(*node.condition, ConditionStyle::Plan);
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Sort:
                {
                    // [["R.a", "asc"], ["R.b", "desc"]]
                    PlanJson& keys = out[kKeys];
                    keys = PlanJson::array();
                    for (const SortKey& key : node.sortKeys)
                    {
                        keys.push_back(PlanJson::array({key.column.toString(), key.ascending ? kAscending : kDescending}));
                    }
                    writeNode(node.children[0], out[kChild]);
                    return;
                }
                case PlanOp::GroupBy:
                {
                    // "keys": ["R.dept"], "aggregates": [["count", "*"], ["avg", "R.salary"]]
                    PlanJson& keys = out[kKeys];
                    keys = PlanJson::array();
                    for (const ColumnRef& column : node.columns)
                    {
                        keys.push_back(column.toString());
                    }
                    PlanJson& aggregates = out[kAggregates];
                    aggregates = PlanJson::array();
                    for (const Aggregate& aggregate : node.aggregates)
                    {
                        aggregates.push_back(PlanJson::array(
                            {functionName(aggregate.function), aggregate.column ? aggregate.column->toString() : kStar}));
                    }
                    writeNode(node.children[0], out[kChild]);
                    return;
                }
                case PlanOp::Limit:
                    out[kN] = node.limitCount;
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Project:
                {
                    PlanJson& columns = out[kColumns];
                    columns = PlanJson::array();
                    for (const ColumnRef& ref : node.columns)
                    {
                        columns.push_back(ref.toString());
                    }
                    writeNode(node.children[0], out[kChild]);
                    return;
                }
                case PlanOp::Join:
                case PlanOp::NestedLoopJoin:
                case PlanOp::SortMergeJoin:
                case PlanOp::HashJoin:
                    out[kCondition] = toString(*node.condition, ConditionStyle::Plan);
                    writeNode(node.children[0], out[kLeft]);
                    writeNode(node.children[1], out[kRight]);
                    return;
                case PlanOp::Rename:
                    out[kTo] = node.alias;
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Distinct:
                    writeNode(node.children[0], out[kChild]);
                    return;
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                case PlanOp::UnionAll:
                    writeNode(node.children[0], out[kLeft]);
                    writeNode(node.children[1], out[kRight]);
                    return;
            }
        }

        // --- Reading -------------------------------------------------------

        [[noreturn]] void fail(const std::string& message)
        {
            throw Error(ErrorCategory::Schema, message);
        }

        std::string nodePrefix(const std::string& op)
        {
            return "Node '" + op + "': ";
        }

        const std::string& readName(const PlanJson& json, const char* field, const std::string& op)
        {
            const PlanJson& value = json.at(field);
            if (!value.is_string())
            {
                fail(nodePrefix(op) + "field '" + field + "' must be a string.");
            }
            const std::string& name = value.get_ref<const std::string&>();
            if (name.empty())
            {
                fail(nodePrefix(op) + "field '" + field + "' must not be empty.");
            }
            return name;
        }

        // A condition inside a plan continues the plan's depth count (the
        // limits.h rule), so it is parsed through the same seam as the RA
        // parser uses, starting at the level of its node.
        Condition readCondition(const PlanJson& json, const std::string& op, int level)
        {
            const PlanJson& value = json.at(kCondition);
            if (!value.is_string())
            {
                fail(nodePrefix(op) + "field 'condition' must be a string.");
            }
            const std::string& text = value.get_ref<const std::string&>();
            try
            {
                const std::vector<Token> tokens = tokenize(text);
                std::size_t position = 0;
                detail::ParsedCondition parsed = detail::parseConditionAt(tokens, position, level);
                if (tokens[position].kind != TokenKind::End)
                {
                    detail::throwSyntaxError(tokens[position], "Expected end of condition");
                }
                return std::move(parsed.condition);
            }
            catch (const Error& error)
            {
                // A position inside the condition string is not a position in
                // the JSON text, so it moves into the message.
                std::string message = nodePrefix(op) + "invalid condition \"" + text + "\": " + error.message();
                if (error.line() != 0)
                {
                    message += " (line " + std::to_string(error.line()) + ", column " +
                               std::to_string(error.column()) + " of the condition)";
                }
                throw Error(error.category(), message);
            }
        }

        // A column string uses the condition grammar's column_ref rule:
        // `name` or `qualifier.name`.
        ColumnRef readColumn(const std::string& text, const std::string& op)
        {
            const std::string invalid = nodePrefix(op) + "invalid column '" + text + "'.";
            std::vector<Token> tokens;
            try
            {
                tokens = tokenize(text);
            }
            catch (const Error&)
            {
                fail(invalid);
            }
            if (tokens.size() == 2 && detail::isNameToken(tokens[0].kind))
            {
                return ColumnRef{std::nullopt, tokens[0].text};
            }
            if (tokens.size() == 4 && detail::isNameToken(tokens[0].kind) && tokens[1].kind == TokenKind::Dot &&
                detail::isNameToken(tokens[2].kind))
            {
                return ColumnRef{tokens[0].text, tokens[2].text};
            }
            fail(invalid);
        }

        // An array of column strings: project's `columns` (never empty) or
        // group_by's `keys` (empty means one group for the whole input).
        std::vector<ColumnRef> readColumns(const PlanJson& json, const char* field, const std::string& op,
                                           bool mayBeEmpty)
        {
            const std::string notStrings = nodePrefix(op) + "field '" + field + "' must be an array of strings.";
            const PlanJson& value = json.at(field);
            if (!value.is_array())
            {
                fail(notStrings);
            }
            if (value.empty() && !mayBeEmpty)
            {
                fail(nodePrefix(op) + "field '" + field + "' must not be empty.");
            }
            std::vector<ColumnRef> columns;
            for (const PlanJson& item : value)
            {
                if (!item.is_string())
                {
                    fail(notStrings);
                }
                columns.push_back(readColumn(item.get_ref<const std::string&>(), op));
            }
            return columns;
        }

        // `mode`: "sort" or "hash".
        ExecMode readMode(const PlanJson& json, const std::string& op)
        {
            const PlanJson& value = json.at(kMode);
            if (value == kModeSort)
            {
                return ExecMode::Sort;
            }
            if (value == kModeHash)
            {
                return ExecMode::Hash;
            }
            fail(nodePrefix(op) + "field 'mode' must be \"sort\" or \"hash\".");
        }

        // sort's `keys`: a non-empty array of [column, "asc" or "desc"].
        std::vector<SortKey> readSortKeys(const PlanJson& json, const std::string& op)
        {
            const PlanJson& value = json.at(kKeys);
            if (!value.is_array() || value.empty())
            {
                fail(nodePrefix(op) + "field 'keys' must be a non-empty array.");
            }
            std::vector<SortKey> keys;
            for (const PlanJson& item : value)
            {
                const bool isPair = item.is_array() && item.size() == 2 && item[0].is_string();
                if (!isPair || (item[1] != kAscending && item[1] != kDescending))
                {
                    fail(nodePrefix(op) + "each entry of 'keys' must be [column, \"asc\" or \"desc\"].");
                }
                keys.push_back(SortKey{readColumn(item[0].get_ref<const std::string&>(), op), item[1] == kAscending});
            }
            return keys;
        }

        // group_by's `aggregates`: an array of [function, column], where the
        // column may be "*" only for count. May be empty (group_by used as a
        // duplicate-free list of the keys).
        std::vector<Aggregate> readAggregates(const PlanJson& json, const std::string& op)
        {
            const PlanJson& value = json.at(kAggregates);
            if (!value.is_array())
            {
                fail(nodePrefix(op) + "field 'aggregates' must be an array.");
            }
            std::vector<Aggregate> aggregates;
            for (const PlanJson& item : value)
            {
                if (!item.is_array() || item.size() != 2 || !item[0].is_string() || !item[1].is_string())
                {
                    fail(nodePrefix(op) + "each entry of 'aggregates' must be [function, column].");
                }
                const std::string& name = item[0].get_ref<const std::string&>();
                const std::string& column = item[1].get_ref<const std::string&>();

                std::optional<AggregateFunction> function;
                for (AggregateFunction candidate : kAllFunctions)
                {
                    if (name == functionName(candidate))
                    {
                        function = candidate;
                    }
                }
                if (!function)
                {
                    fail(nodePrefix(op) + "unknown aggregate function '" + name + "'.");
                }

                Aggregate aggregate{*function, std::nullopt};
                if (column == kStar)
                {
                    if (*function != AggregateFunction::Count)
                    {
                        fail(nodePrefix(op) + "only count may use '*'.");
                    }
                }
                else
                {
                    aggregate.column = readColumn(column, op);
                }
                aggregates.push_back(aggregate);
            }
            return aggregates;
        }

        // limit's `n`: a whole number from 0 to the largest INT (int32).
        // JSON reads a non-negative whole number as unsigned, so a negative
        // number, a fraction or a string all fail the first test.
        std::int32_t readLimitCount(const PlanJson& json, const std::string& op)
        {
            const PlanJson& value = json.at(kN);
            if (!value.is_number_unsigned() ||
                value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            {
                fail(nodePrefix(op) + "field 'n' must be a non-negative integer.");
            }
            return static_cast<std::int32_t>(value.get<std::uint64_t>());
        }

        // Checks `op` and the field set of one node; returns its op.
        // `physical` picks which op set and field lists apply.
        PlanOp readHeader(const PlanJson& json, bool physical)
        {
            const auto opField = json.find(kOp);
            if (opField == json.end())
            {
                fail("Plan node is missing required field 'op'.");
            }
            if (!opField->is_string())
            {
                fail("Plan node field 'op' must be a string.");
            }
            const std::string& name = opField->get_ref<const std::string&>();
            const std::optional<PlanOp> op = opFromName(name, physical);
            if (!op)
            {
                fail("Unknown plan operator '" + name + "'.");
            }

            const std::vector<const char*> fields = fieldsOf(*op, physical);
            for (const char* field : fields)
            {
                if (!json.contains(field))
                {
                    fail(nodePrefix(name) + "missing required field '" + field + "'.");
                }
            }
            for (const auto& item : json.items())
            {
                bool known = item.key() == kOp;
                for (const char* field : fields)
                {
                    known = known || item.key() == field;
                }
                if (!known)
                {
                    fail(nodePrefix(name) + "unexpected field '" + item.key() + "'.");
                }
            }
            return *op;
        }

        // Reads one node into `out`. `parentLevel` counts the operator nodes
        // above it; the check comes before any recursion, so a too-deep plan
        // is rejected without walking it.
        void readNode(const PlanJson& json, int parentLevel, bool physical, PlanNode& out);

        void readInput(const PlanJson& parent, const char* field, const std::string& op, int level, bool physical,
                       PlanNode& out)
        {
            const PlanJson& input = parent.at(field);
            if (!input.is_object())
            {
                fail(nodePrefix(op) + "field '" + field + "' must be an object.");
            }
            readNode(input, level, physical, out);
        }

        void readNode(const PlanJson& json, int parentLevel, bool physical, PlanNode& out)
        {
            out.op = readHeader(json, physical);
            const std::string& op = json.at(kOp).get_ref<const std::string&>();
            // Scans are leaves and do not count toward the depth (limits.h).
            const bool isScan = out.op == PlanOp::Scan || out.op == PlanOp::SeqScan;
            const int level = parentLevel + (isScan ? 0 : 1);
            if (level > kMaxNestingDepth)
            {
                fail(nodePrefix(op) + "plan nested deeper than " + std::to_string(kMaxNestingDepth) + " levels.");
            }

            switch (out.op)
            {
                case PlanOp::Scan:
                case PlanOp::SeqScan:
                    out.table = readName(json, kTable, op);
                    break;
                case PlanOp::Select:
                case PlanOp::Join:
                case PlanOp::Filter:
                case PlanOp::NestedLoopJoin:
                case PlanOp::SortMergeJoin:
                case PlanOp::HashJoin:
                    out.condition = readCondition(json, op, level);
                    break;
                case PlanOp::Project:
                    out.columns = readColumns(json, kColumns, op, false);
                    break;
                case PlanOp::Rename:
                    out.alias = readName(json, kTo, op);
                    break;
                case PlanOp::Distinct:
                    if (physical)
                    {
                        out.mode = readMode(json, op);
                    }
                    break;
                case PlanOp::Sort:
                    out.sortKeys = readSortKeys(json, op);
                    break;
                case PlanOp::GroupBy:
                    out.mode = readMode(json, op);
                    out.columns = readColumns(json, kKeys, op, true);
                    out.aggregates = readAggregates(json, op);
                    break;
                case PlanOp::Limit:
                    out.limitCount = readLimitCount(json, op);
                    break;
                case PlanOp::Cross:
                case PlanOp::Union:
                case PlanOp::Intersect:
                case PlanOp::Except:
                case PlanOp::UnionAll:
                    break;
            }

            out.children.resize(inputCount(out.op));
            if (out.children.size() == 1)
            {
                readInput(json, kChild, op, level, physical, out.children[0]);
            }
            else if (out.children.size() == 2)
            {
                readInput(json, kLeft, op, level, physical, out.children[0]);
                readInput(json, kRight, op, level, physical, out.children[1]);
            }
        }

        // The whole plan: the top must be an object, then node by node.
        PlanNode readPlan(const PlanJson& json, bool physical)
        {
            if (!json.is_object())
            {
                fail("Plan must be a JSON object.");
            }
            PlanNode plan;
            readNode(json, 0, physical, plan);
            return plan;
        }

        // nlohmann reports the byte where parsing stopped; turn it into the
        // 1-based line and column of that byte.
        std::pair<int, int> lineAndColumn(std::string_view text, std::size_t byte)
        {
            int line = 1;
            int column = 1;
            const std::size_t end = byte == 0 ? 0 : std::min(byte - 1, text.size());
            for (std::size_t i = 0; i < end; ++i)
            {
                if (text[i] == '\n')
                {
                    ++line;
                    column = 1;
                }
                else
                {
                    ++column;
                }
            }
            return {line, column};
        }

        // Drops nlohmann's "[json.exception.parse_error.101] parse error at
        // line L, column C: " prefix; the position is reported separately.
        std::string parseErrorDetail(const std::string& what)
        {
            std::string detail = what;
            const std::size_t bracket = detail.find("] ");
            if (bracket != std::string::npos)
            {
                detail.erase(0, bracket + 2);
            }
            const std::string positionPrefix = "parse error at line";
            if (detail.compare(0, positionPrefix.size(), positionPrefix) == 0)
            {
                const std::size_t colon = detail.find(": ");
                if (colon != std::string::npos)
                {
                    detail.erase(0, colon + 2);
                }
            }
            return detail;
        }

        // Turns JSON text into a JSON value; text that is not JSON is a
        // Syntax error at its line and column.
        PlanJson parseText(std::string_view text)
        {
            try
            {
                return PlanJson::parse(text.begin(), text.end());
            }
            catch (const PlanJson::parse_error& error)
            {
                const auto [line, column] = lineAndColumn(text, error.byte);
                throw Error(ErrorCategory::Syntax, "Malformed plan JSON: " + parseErrorDetail(error.what()), line,
                            column);
            }
        }

    } // namespace

    PlanJson toJson(const PlanNode& plan)
    {
        PlanJson json;
        writeNode(plan, json);
        return json;
    }

    PlanNode fromJson(const PlanJson& json)
    {
        return readPlan(json, false);
    }

    PlanNode physicalFromJson(const PlanJson& json)
    {
        return readPlan(json, true);
    }

    std::string toJsonText(const PlanNode& plan, int indent)
    {
        try
        {
            return toJson(plan).dump(indent < 0 ? -1 : indent);
        }
        catch (const PlanJson::type_error&)
        {
            // dump() is the one place that requires valid UTF-8.
            throw Error(ErrorCategory::Schema, "Plan cannot be written as JSON: a string is not valid UTF-8.");
        }
    }

    PlanNode fromJsonText(std::string_view text)
    {
        return fromJson(parseText(text));
    }

    PlanNode physicalFromJsonText(std::string_view text)
    {
        return physicalFromJson(parseText(text));
    }

} // namespace db
