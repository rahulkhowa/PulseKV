// tests/test_parser.cpp
//
// Phase 2 unit tests for Parser and Response.
// Build:
//   g++ -std=c++20 -I../include -o test_parser test_parser.cpp ../src/protocol/parser.cpp
// Run:
//   ./test_parser

#include <cassert>
#include <iostream>
#include <string>

#include "pulsekv/protocol/parser.hpp"

using namespace pulsekv;

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        ++tests_run;                                                        \
        if (!(expr)) {                                                      \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__          \
                      << "  " << #expr << "\n";                            \
        } else {                                                            \
            ++tests_passed;                                                 \
        }                                                                   \
    } while (false)

// ---- Empty / whitespace ----------------------------------------------------

void test_empty_line() {
    auto r = Parser::parse("");
    CHECK(r.status == ParseStatus::EMPTY);
}

void test_whitespace_only() {
    auto r = Parser::parse("   \t  ");
    CHECK(r.status == ParseStatus::EMPTY);
}

// ---- PING ------------------------------------------------------------------

void test_ping() {
    auto r = Parser::parse("PING");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::PING);
}

void test_ping_lowercase() {
    auto r = Parser::parse("ping");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::PING);
}

void test_ping_extra_arg() {
    auto r = Parser::parse("PING extra");
    CHECK(r.status == ParseStatus::ERR);
    CHECK(!r.error_message.empty());
}

// ---- SET -------------------------------------------------------------------

void test_set_basic() {
    auto r = Parser::parse("SET name Rahul");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::SET);
    CHECK(r.command.args[0] == "name");
    CHECK(r.command.args[1] == "Rahul");
    CHECK(r.command.ttl_seconds == 0);
}

void test_set_lowercase_cmd() {
    auto r = Parser::parse("set foo bar");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::SET);
}

void test_set_missing_value() {
    auto r = Parser::parse("SET key");
    CHECK(r.status == ParseStatus::ERR);
}

void test_set_no_args() {
    auto r = Parser::parse("SET");
    CHECK(r.status == ParseStatus::ERR);
}

void test_set_with_ttl() {
    auto r = Parser::parse("SET session token EX 60");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::SET);
    CHECK(r.command.args[0] == "session");
    CHECK(r.command.args[1] == "token");
    CHECK(r.command.ttl_seconds == 60);
}

void test_set_ttl_invalid_qualifier() {
    auto r = Parser::parse("SET key val PX 1000");
    CHECK(r.status == ParseStatus::ERR);
}

void test_set_ttl_zero() {
    auto r = Parser::parse("SET key val EX 0");
    CHECK(r.status == ParseStatus::ERR);
}

void test_set_ttl_negative() {
    auto r = Parser::parse("SET key val EX -5");
    CHECK(r.status == ParseStatus::ERR);
}

void test_set_ttl_non_numeric() {
    auto r = Parser::parse("SET key val EX abc");
    CHECK(r.status == ParseStatus::ERR);
}

void test_set_too_many_args() {
    auto r = Parser::parse("SET a b c d e");
    CHECK(r.status == ParseStatus::ERR);
}

// ---- GET -------------------------------------------------------------------

void test_get_basic() {
    auto r = Parser::parse("GET name");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::GET);
    CHECK(r.command.args[0] == "name");
}

void test_get_no_args() {
    auto r = Parser::parse("GET");
    CHECK(r.status == ParseStatus::ERR);
}

void test_get_too_many_args() {
    auto r = Parser::parse("GET a b");
    CHECK(r.status == ParseStatus::ERR);
}

void test_get_lowercase() {
    auto r = Parser::parse("get mykey");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::GET);
}

// ---- DEL -------------------------------------------------------------------

void test_del_basic() {
    auto r = Parser::parse("DEL name");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::DEL);
    CHECK(r.command.args[0] == "name");
}

void test_del_no_args() {
    auto r = Parser::parse("DEL");
    CHECK(r.status == ParseStatus::ERR);
}

void test_del_too_many_args() {
    auto r = Parser::parse("DEL a b");
    CHECK(r.status == ParseStatus::ERR);
}

// ---- EXISTS ----------------------------------------------------------------

void test_exists_basic() {
    auto r = Parser::parse("EXISTS flag");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::EXISTS);
    CHECK(r.command.args[0] == "flag");
}

void test_exists_no_args() {
    auto r = Parser::parse("EXISTS");
    CHECK(r.status == ParseStatus::ERR);
}

void test_exists_too_many_args() {
    auto r = Parser::parse("EXISTS a b");
    CHECK(r.status == ParseStatus::ERR);
}

// ---- Unknown command -------------------------------------------------------

void test_unknown_command() {
    auto r = Parser::parse("FLUSHALL");
    CHECK(r.status == ParseStatus::ERR);
    CHECK(r.error_message.find("FLUSHALL") != std::string::npos);
}

void test_unknown_command_mixed_case() {
    auto r = Parser::parse("hElLo");
    CHECK(r.status == ParseStatus::ERR);
}

// ---- Response builder ------------------------------------------------------

void test_response_ok_no_data() {
    CHECK(Response::ok() == "+OK\r\n");
}

void test_response_ok_with_value() {
    CHECK(Response::ok("Rahul") == "+Rahul\r\n");
}

void test_response_null_bulk() {
    CHECK(Response::null_bulk() == "$-1\r\n");
}

void test_response_integer_one() {
    CHECK(Response::integer(1) == ":1\r\n");
}

void test_response_integer_zero() {
    CHECK(Response::integer(0) == ":0\r\n");
}

void test_response_error() {
    CHECK(Response::error("ERR not found") == "-ERR not found\r\n");
}

// ---- Whitespace handling ---------------------------------------------------

void test_leading_trailing_spaces() {
    auto r = Parser::parse("  GET   mykey  ");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::GET);
    CHECK(r.command.args[0] == "mykey");
}

void test_tabs_as_delimiters() {
    auto r = Parser::parse("SET\tfoo\tbar");
    CHECK(r.status == ParseStatus::OK);
    CHECK(r.command.type == CommandType::SET);
    CHECK(r.command.args[0] == "foo");
    CHECK(r.command.args[1] == "bar");
}

// ---- main ------------------------------------------------------------------

int main() {
    std::cout << "PulseKV — Phase 2 Parser Tests\n";
    std::cout << "================================\n\n";

    // Empty / whitespace
    test_empty_line();
    test_whitespace_only();

    // PING
    test_ping();
    test_ping_lowercase();
    test_ping_extra_arg();

    // SET
    test_set_basic();
    test_set_lowercase_cmd();
    test_set_missing_value();
    test_set_no_args();
    test_set_with_ttl();
    test_set_ttl_invalid_qualifier();
    test_set_ttl_zero();
    test_set_ttl_negative();
    test_set_ttl_non_numeric();
    test_set_too_many_args();

    // GET
    test_get_basic();
    test_get_no_args();
    test_get_too_many_args();
    test_get_lowercase();

    // DEL
    test_del_basic();
    test_del_no_args();
    test_del_too_many_args();

    // EXISTS
    test_exists_basic();
    test_exists_no_args();
    test_exists_too_many_args();

    // Unknown
    test_unknown_command();
    test_unknown_command_mixed_case();

    // Response builder
    test_response_ok_no_data();
    test_response_ok_with_value();
    test_response_null_bulk();
    test_response_integer_one();
    test_response_integer_zero();
    test_response_error();

    // Whitespace edge cases
    test_leading_trailing_spaces();
    test_tabs_as_delimiters();

    std::cout << "\nResults: " << tests_passed << " / " << tests_run << " passed\n";
    if (tests_passed == tests_run) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cout << (tests_run - tests_passed) << " TESTS FAILED\n";
        return 1;
    }
}
