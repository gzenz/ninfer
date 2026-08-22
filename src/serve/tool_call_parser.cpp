#include "serve/tool_call_parser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ninfer::serve {
namespace {

using Json = nlohmann::json;

std::string trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) { --end; }
    return std::string(text.substr(begin, end - begin));
}

std::string rtrim_ascii(std::string_view text) {
    std::size_t end = text.size();
    while (end != 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) { --end; }
    return std::string(text.substr(0, end));
}

void skip_ws(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) { ++pos; }
}

bool starts_with_at(std::string_view text, std::size_t pos, std::string_view prefix) {
    return pos <= text.size() && text.substr(pos, prefix.size()) == prefix;
}

std::size_t longest_suffix_prefix(std::string_view text, std::string_view marker) {
    const std::size_t maximum = std::min(text.size(), marker.size() - 1);
    for (std::size_t size = maximum; size != 0; --size) {
        if (text.substr(text.size() - size) == marker.substr(0, size)) { return size; }
    }
    return 0;
}

bool valid_function_name(std::string_view name, std::size_t max_name_length) {
    if (name.empty() || name.size() > max_name_length) { return false; }
    for (const unsigned char c : name) {
        if (std::isalnum(c) == 0 && c != '_' && c != '-') { return false; }
    }
    return true;
}

std::string new_tool_call_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "call_%016llx",
                  static_cast<unsigned long long>(dist(rng)));
    return std::string(buf.data());
}

const std::unordered_map<std::string, std::string>* tool_param_types(
    const ToolParamTypeMap& map, const std::string& tool_name) {
    const auto it = map.find(tool_name);
    return it == map.end() ? nullptr : &it->second;
}

// Whether the schema explicitly permits a non-string JSON type for
// (tool_name, param). The map only records non-string-typed params (string,
// unknown/invalid, and absent-type params are omitted), so a recorded entry
// means the schema positively wants a number/boolean/array/object/null and
// the parser may adopt the deserialized JSON type. Only membership is tested;
// the stored value is never read. Absence => preserve raw text: the client
// owns the schema and decides type interpretation.
bool param_allows_deserialization(const ToolParamTypeMap& map,
                                  const std::string& tool_name,
                                  const std::string& param) {
    const auto* params = tool_param_types(map, tool_name);
    if (params == nullptr) { return false; }
    return params->find(param) != params->end();
}

} // namespace

namespace {

// Valid JSON Schema "type" values that are not string. A parameter is only
// allowed to deserialize when every type it declares is in this set.
const std::unordered_set<std::string>& non_string_schema_types() {
    static const std::unordered_set<std::string> types = {"integer", "number", "boolean",
                                                          "array", "object", "null"};
    return types;
}

// Classify a parameter's schema "type" (a string or an array of strings) into
// the set of declared types. Returns false if "type" is absent or not a
// string/array; in that case classification is uncertain and the caller
// preserves raw text. Returns true and fills `declared` otherwise.
bool classify_param_type(const Json& spec, std::vector<std::string>& declared) {
    const auto type_it = spec.find("type");
    if (type_it == spec.end()) { return false; }
    if (type_it->is_string()) {
        declared.push_back(type_it->get<std::string>());
        return true;
    }
    if (type_it->is_array()) {
        for (const Json& t : *type_it) {
            if (!t.is_string()) { return false; }
            declared.push_back(t.get<std::string>());
        }
        // An empty type array (e.g. "type":[]) is uncertain, not a positive
        // declaration of a non-string type; returning false here preserves
        // raw text and prevents all_non_string_types from succeeding vacuously
        // on an empty vector (which would then crash on declared.front()).
        if (declared.empty()) { return false; }
        return true;
    }
    return false;
}

// Whether every declared type is a valid non-string JSON Schema type. If any
// declared type is "string" or unknown/invalid, the schema permits (or may
// permit) a string value, so the parser must preserve raw text.
bool all_non_string_types(const std::vector<std::string>& declared) {
    const auto& valid = non_string_schema_types();
    for (const std::string& type : declared) {
        if (valid.find(type) == valid.end()) { return false; }
    }
    return true;
}

} // namespace

ToolParamTypeMap build_tool_param_type_map(const std::vector<ToolDefinition>& tools) {
    ToolParamTypeMap map;
    for (const ToolDefinition& tool : tools) {
        // Replace any prior entry for this tool name first, before any early
        // exit, so a redefinition with an empty/malformed/no-properties schema
        // cannot leak stale non-string permissions from a previous definition.
        map[tool.name] = {};
        if (tool.parameters_json.empty()) { continue; }
        const Json schema = Json::parse(tool.parameters_json, nullptr, false);
        if (!schema.is_object()) { continue; }
        const auto props_it = schema.find("properties");
        if (props_it == schema.end() || !props_it->is_object()) { continue; }
        // The entry for tool.name was already reset to empty at the top of
        // the loop; populate it only from this definition's properties.
        std::unordered_map<std::string, std::string>& inner = map[tool.name];
        for (const auto& [name, spec] : props_it->items()) {
            if (!spec.is_object()) { continue; }
            std::vector<std::string> declared;
            if (!classify_param_type(spec, declared)) { continue; }
            // Record only when every declared type is a valid non-string type;
            // string-allowed, unknown/invalid, and absent-type params are left
            // out so the parser preserves raw text for them.
            if (all_non_string_types(declared)) { inner[name] = declared.front(); }
        }
    }
    return map;
}

namespace {

// The six tool-call markers with their open/close polarity. In the well-formed
// format they are strictly nested (call > function > param), so a single depth
// counter tracks all three levels. Markers that appear inside a parameter value
// (e.g. code that quotes the tool-call format) are balanced, so they do not
// disturb the top-level depth.
struct ToolCallMarker {
    std::string_view text;
    bool is_open;
};
constexpr ToolCallMarker kToolCallMarkers[] = {
    {"<tool_call>", true},
    {"<function=", true},
    {"<parameter=", true},
    {"</parameter>", false},
    {"</function>", false},
    {"</tool_call>", false},
};

// Find the earliest tool-call marker at or after `from`. Returns its position
// and sets `is_open` and `marker_len`; returns npos if no marker remains.
std::size_t find_next_marker(std::string_view text, std::size_t from,
                             bool& is_open, std::size_t& marker_len) {
    std::size_t next = std::string_view::npos;
    bool next_is_open = false;
    std::size_t next_len = 0;
    for (const ToolCallMarker& m : kToolCallMarkers) {
        const std::size_t found = text.find(m.text, from);
        if (found != std::string_view::npos &&
            (next == std::string_view::npos || found < next)) {
            next = found;
            next_is_open = m.is_open;
            next_len = m.text.size();
        }
    }
    is_open  = next_is_open;
    marker_len = next_len;
    return next;
}

// Scan `text` from `from`, tracking a depth counter (open markers increment,
// close markers decrement) that starts at `start_depth`. Return the position
// of the close marker that returns the depth to `target_depth`, or npos if the
// depth never reaches it (unbalanced). Used to locate a region's true closing
// marker when its value may contain balanced nested markers.
std::size_t find_matching_close(std::string_view text, std::size_t from,
                                int start_depth, int target_depth) {
    int depth = start_depth;
    std::size_t pos = from;
    while (pos < text.size()) {
        bool is_open = false;
        std::size_t marker_len = 0;
        const std::size_t next = find_next_marker(text, pos, is_open, marker_len);
        if (next == std::string_view::npos) { return std::string_view::npos; }
        if (is_open) {
            ++depth;
        } else {
            --depth;
            if (depth == target_depth) { return next; }
        }
        pos = next + marker_len;
    }
    return std::string_view::npos;
}
bool parse_parameter(std::string_view inner, std::size_t& pos, Json& args,
                     const std::string& tool_name, const ToolParamTypeMap& param_types) {
    constexpr std::string_view kParamOpen  = "<parameter=";
    constexpr std::string_view kParamClose = "</parameter>";
    if (!starts_with_at(inner, pos, kParamOpen)) { return false; }
    const std::size_t name_begin = pos + kParamOpen.size();
    const std::size_t name_end   = inner.find('>', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
    const std::string key       = std::string(inner.substr(name_begin, name_end - name_begin));
    pos                         = name_end + 1;
    // The value may contain balanced nested markers (e.g. code that quotes the
    // tool-call format), so locate the matching close by depth rather than the
    // first occurrence.
    const std::size_t value_end = find_matching_close(inner, pos, 1, 0);
    if (value_end == std::string_view::npos) { return false; }
    const std::string raw_value = trim_ascii(inner.substr(pos, value_end - pos));
    // Only adopt the deserialized JSON type when the schema explicitly
    // permits a non-string type. For string-typed, unknown, or absent
    // params, keep the raw text so the model's value reaches the client
    // with its type intact; the client owns the schema and validates.
    Json parsed = Json::parse(raw_value, nullptr, false);
    const bool can_deserialize = param_allows_deserialization(param_types, tool_name, key);
    args[key] = (parsed.is_discarded() || !can_deserialize) ? Json(raw_value) : parsed;
    pos                         = value_end + kParamClose.size();
    return true;
}

bool parse_one_tool_call(std::string_view block, std::size_t max_name_length,
                          const ToolParamTypeMap& param_types, bool tolerant,
                          ToolCall& out) {
    constexpr std::string_view kFunctionOpen  = "<function=";
    constexpr std::string_view kFunctionClose = "</function>";
    std::size_t pos                           = 0;
    skip_ws(block, pos);
    if (!starts_with_at(block, pos, kFunctionOpen)) { return false; }
    const std::size_t name_begin = pos + kFunctionOpen.size();
    const std::size_t name_end   = block.find('>', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
    const std::string name = std::string(block.substr(name_begin, name_end - name_begin));
    if (!valid_function_name(name, max_name_length)) { return false; }
    pos = name_end + 1;

    // The function body may contain balanced nested markers (e.g. a parameter
    // value quoting the tool-call format), so locate the matching close by
    // depth rather than the first occurrence.
    const std::size_t function_end = find_matching_close(block, pos, 1, 0);
    if (function_end == std::string_view::npos) { return false; }
    const std::string_view params = block.substr(pos, function_end - pos);
    Json args                     = Json::object();
    std::size_t param_pos         = 0;
    for (;;) {
        skip_ws(params, param_pos);
        if (param_pos >= params.size()) { break; }
        if (!parse_parameter(params, param_pos, args, name, param_types)) {
            return false;
        }
    }

    pos = function_end + kFunctionClose.size();
    skip_ws(block, pos);
    // Qwen occasionally emits a duplicate closing tag or explanatory text after a
    // complete function. Only discard that suffix in explicit tolerant mode; the
    // strict parser retains its all-or-nothing behavior.
    if (!tolerant && pos != block.size()) { return false; }

    out.id             = new_tool_call_id();
    out.name           = name;
    out.arguments_json = args.dump();
    return true;
}

ParsedToolCallOutput fallback(const std::string& text) {
    ParsedToolCallOutput out;
    out.content = text;
    return out;
}

} // namespace

ParsedToolCallOutput parse_qwen_tool_call_output(const std::string& text,
                                                 std::size_t max_tool_name_length,
                                                 const ToolParamTypeMap& param_types,
                                                 bool tolerant) {
    constexpr std::string_view kToolOpen  = "<tool_call>";
    constexpr std::string_view kToolClose = "</tool_call>";

    const std::size_t first = text.find(kToolOpen);
    if (first == std::string::npos) { return fallback(text); }

    ParsedToolCallOutput out;
    out.content = rtrim_ascii(std::string_view(text).substr(0, first));

    std::size_t pos = first;
    while (pos < text.size()) {
        skip_ws(text, pos);
        if (pos >= text.size()) { break; }
        if (!starts_with_at(text, pos, kToolOpen)) {
            if (tolerant && !out.tool_calls.empty()) { break; }
            return fallback(text);
        }
        const std::size_t inner_begin = pos + kToolOpen.size();
        // The call may contain balanced nested markers (e.g. a parameter value
        // quoting the tool-call format), so locate the matching close by depth
        // rather than the first occurrence.
        const std::size_t close = find_matching_close(text, inner_begin, 1, 0);
        if (close == std::string::npos && !tolerant) { return fallback(text); }
        const std::size_t block_end = close == std::string::npos ? text.size() : close;
        ToolCall call;
        if (!parse_one_tool_call(std::string_view(text).substr(inner_begin, block_end - inner_begin),
                                 max_tool_name_length, param_types, tolerant, call)) {
            // Once one complete call has been recovered, do not discard it just
            // because Qwen started a malformed second call or added a suffix.
            if (tolerant && !out.tool_calls.empty()) { break; }
            return fallback(text);
        }
        out.tool_calls.push_back(std::move(call));
        if (close == std::string::npos) { break; }
        pos = close + kToolClose.size();
    }

    if (out.tool_calls.empty()) { return fallback(text); }
    out.is_tool_call_response = true;
    return out;
}

std::string ToolCallStreamFilter::feed(std::string_view text) {
    if (finished_) { throw std::logic_error("tool-call stream filter is already finished"); }
    if (text.empty()) { return {}; }
    if (saw_tool_marker_) {
        tool_region_.append(text);
        return {};
    }

    constexpr std::string_view kToolOpen = "<tool_call>";
    pending_.append(text);
    const std::size_t marker = pending_.find(kToolOpen);
    if (marker != std::string::npos) {
        std::size_t safe_end = marker;
        while (safe_end != 0 &&
               std::isspace(static_cast<unsigned char>(pending_[safe_end - 1])) != 0) {
            --safe_end;
        }
        std::string visible = pending_.substr(0, safe_end);
        tool_region_        = pending_.substr(safe_end);
        pending_.clear();
        saw_tool_marker_ = true;
        emitted_bytes_ += visible.size();
        return visible;
    }

    const std::size_t prefix = longest_suffix_prefix(pending_, kToolOpen);
    std::size_t safe_end     = pending_.size() - prefix;
    while (safe_end != 0 && std::isspace(static_cast<unsigned char>(pending_[safe_end - 1])) != 0) {
        --safe_end;
    }
    std::string visible = pending_.substr(0, safe_end);
    pending_.erase(0, safe_end);
    emitted_bytes_ += visible.size();
    return visible;
}

std::string ToolCallStreamFilter::finish(bool is_tool_call_response) {
    if (finished_) { throw std::logic_error("tool-call stream filter is already finished"); }
    finished_ = true;
    if (is_tool_call_response) {
        pending_.clear();
        tool_region_.clear();
        return {};
    }
    std::string tail = std::move(pending_);
    tail += tool_region_;
    tool_region_.clear();
    emitted_bytes_ += tail.size();
    return tail;
}

} // namespace ninfer::serve
