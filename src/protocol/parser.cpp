#include "pulsekv/protocol/parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <string>

namespace pulsekv {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// Parser::tokenize
// ---------------------------------------------------------------------------
std::vector<std::string> Parser::tokenize(std::string_view line) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_token = false;

    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (in_token) {
                tokens.push_back(std::move(token));
                token.clear();
                in_token = false;
            }
        } else {
            token += c;
            in_token = true;
        }
    }
    if (in_token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

// ---------------------------------------------------------------------------
// Parser::parse
// ---------------------------------------------------------------------------
ParseResult Parser::parse(std::string_view line) {
    auto tokens = tokenize(line);

    // Empty or whitespace-only line — ignore silently.
    if (tokens.empty()) {
        return {ParseStatus::EMPTY, {}, {}};
    }

    const std::string cmd = to_upper(tokens[0]);

    // ---- PING --------------------------------------------------------------
    if (cmd == "PING") {
        if (tokens.size() != 1) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'PING'"};
        }
        Command c;
        c.type = CommandType::PING;
        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- GET ---------------------------------------------------------------
    if (cmd == "GET") {
        if (tokens.size() != 2) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'GET'"};
        }
        Command c;
        c.type = CommandType::GET;
        c.args = {tokens[1]};
        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- DEL ---------------------------------------------------------------
    if (cmd == "DEL") {
        if (tokens.size() != 2) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'DEL'"};
        }
        Command c;
        c.type = CommandType::DEL;
        c.args = {tokens[1]};
        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- EXISTS ------------------------------------------------------------
    if (cmd == "EXISTS") {
        if (tokens.size() != 2) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'EXISTS'"};
        }
        Command c;
        c.type = CommandType::EXISTS;
        c.args = {tokens[1]};
        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- EXPIRE ------------------------------------------------------------
    // EXPIRE key seconds — apply a new TTL to an existing key
    if (cmd == "EXPIRE") {
        if (tokens.size() != 3) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'EXPIRE'"};
        }
        long long secs = 0;
        auto [ptr, ec] = std::from_chars(tokens[2].data(),
                                          tokens[2].data() + tokens[2].size(),
                                          secs);
        if (ec != std::errc{} || secs <= 0) {
            return {ParseStatus::ERR, {},
                    "ERR invalid expire time in 'EXPIRE'"};
        }
        Command c;
        c.type        = CommandType::EXPIRE;
        c.args        = {tokens[1]};
        c.ttl_seconds = secs;
        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- TTL ---------------------------------------------------------------
    // TTL key — return remaining TTL (-1 = no TTL, -2 = not found / expired)
    if (cmd == "TTL") {
        if (tokens.size() != 2) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'TTL'"};
        }
        Command c;
        c.type = CommandType::TTL;
        c.args = {tokens[1]};
        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- SET ---------------------------------------------------------------
    // Phase 2: SET key value
    // Phase 10: SET key value EX <seconds>  (handled via ttl_seconds)
    if (cmd == "SET") {
        // Minimum: SET key value (3 tokens)
        if (tokens.size() < 3) {
            return {ParseStatus::ERR, {},
                    "ERR wrong number of arguments for 'SET'"};
        }

        Command c;
        c.type = CommandType::SET;
        c.args = {tokens[1], tokens[2]};  // args[0]=key, args[1]=value

        // Optional EX <seconds> suffix (Phase 10 extension, parsed now for
        // completeness but the Store won't use ttl_seconds until Phase 10).
        if (tokens.size() == 5) {
            const std::string qualifier = to_upper(tokens[3]);
            if (qualifier != "EX") {
                return {ParseStatus::ERR, {},
                        "ERR syntax error"};
            }
            long long secs = 0;
            auto [ptr, ec] = std::from_chars(tokens[4].data(),
                                              tokens[4].data() + tokens[4].size(),
                                              secs);
            if (ec != std::errc{} || secs <= 0) {
                return {ParseStatus::ERR, {},
                        "ERR invalid expire time in 'SET'"};
            }
            c.ttl_seconds = secs;
        } else if (tokens.size() != 3) {
            // 4 tokens, or >5 tokens — invalid
            return {ParseStatus::ERR, {},
                    "ERR syntax error"};
        }

        return {ParseStatus::OK, std::move(c), {}};
    }

    // ---- UNKNOWN -----------------------------------------------------------
    return {ParseStatus::ERR, {},
            "ERR unknown command '" + tokens[0] + "'"};
}

// ---------------------------------------------------------------------------
// ResponseBuilder
// ---------------------------------------------------------------------------

std::string Response::ok() {
    return "+OK\r\n";
}

std::string Response::ok(const std::string& value) {
    return "+" + value + "\r\n";
}

std::string Response::null_bulk() {
    return "$-1\r\n";
}

std::string Response::integer(int n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string Response::integer(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string Response::error(const std::string& msg) {
    return "-" + msg + "\r\n";
}

} // namespace pulsekv
