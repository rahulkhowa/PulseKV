#pragma once

#include <string>
#include <vector>

namespace pulsekv {

// ---------------------------------------------------------------------------
// CommandType — all supported commands
// ---------------------------------------------------------------------------
enum class CommandType {
    SET,
    GET,
    DEL,
    EXISTS,
    PING,
    EXPIRE,   // EXPIRE key seconds — set TTL on existing key
    TTL,      // TTL key — return remaining TTL in seconds (-1 = no expiry, -2 = not found)
    UNKNOWN,
};

// ---------------------------------------------------------------------------
// Command — result of parsing a single request line
// ---------------------------------------------------------------------------
struct Command {
    CommandType type = CommandType::UNKNOWN;
    std::vector<std::string> args;  // args[0] = key, args[1] = value, etc.

    // For SET with TTL (Phase 10): SET key value EX <seconds>
    // args = { key, value } and ttl_seconds > 0 signals a TTL was given.
    long long ttl_seconds = 0;  // 0 means no expiry
};

// ---------------------------------------------------------------------------
// ParseResult — outcome of a parse attempt
// ---------------------------------------------------------------------------
enum class ParseStatus {
    OK,             // Valid command — execute it
    EMPTY,          // Blank / whitespace-only line — ignore silently
    ERR,            // Malformed request — send the error_message to the client
};

struct ParseResult {
    ParseStatus status = ParseStatus::EMPTY;
    Command     command;
    std::string error_message;  // Non-empty only when status == ERROR
};

// ---------------------------------------------------------------------------
// Parser — stateless, pure function interface
//
// parse() takes a single line (without the trailing \r\n) and returns a
// ParseResult describing what to do next.
//
// The parser is intentionally simple:
//   - Split on whitespace
//   - Case-insensitive command keyword
//   - Validate argument count
//   - No binary-safe values in Phase 2 (values must be single tokens)
//
// Thread-safety: parse() is a free function — fully re-entrant.
// ---------------------------------------------------------------------------
class Parser {
public:
    // Parse one request line.
    // 'line' must NOT include the trailing \r\n.
    [[nodiscard]] static ParseResult parse(std::string_view line);

private:
    // Split 'line' into whitespace-separated tokens.
    [[nodiscard]] static std::vector<std::string> tokenize(std::string_view line);
};

// ---------------------------------------------------------------------------
// ResponseBuilder — constructs wire-format responses
//
// Protocol:
//   +OK\r\n            — success, no data
//   +<value>\r\n       — success with string data
//   -ERR <msg>\r\n     — error
//   :1\r\n / :0\r\n    — integer (EXISTS / DEL result)
//   $-1\r\n            — null bulk (key not found)
// ---------------------------------------------------------------------------
struct Response {
    [[nodiscard]] static std::string ok();
    [[nodiscard]] static std::string ok(const std::string& value);
    [[nodiscard]] static std::string null_bulk();
    [[nodiscard]] static std::string integer(int n);
    [[nodiscard]] static std::string integer(long long n);
    [[nodiscard]] static std::string error(const std::string& msg);
};

} // namespace pulsekv
