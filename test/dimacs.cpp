#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/dimacs.h"

namespace
{

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

ppcdcl::DimacsCnf parse(const std::string &text)
{
    std::istringstream input(text);
    return ppcdcl::parse_dimacs(input, "fixture.cnf");
}

void expect_parse_error(const std::string &text, const std::string &message_fragment)
{
    try
    {
        (void)parse(text);
    }
    catch (const ppcdcl::DimacsParseError &error)
    {
        require(std::string(error.what()).find(message_fragment) != std::string::npos,
                "parse error did not contain '" + message_fragment + "': " + error.what());
        return;
    }
    throw std::runtime_error("expected DIMACS parse failure containing '" +
                             message_fragment + "'");
}

void test_token_stream_clause_boundaries()
{
    const ppcdcl::DimacsCnf cnf = parse(
        "\n"
        "   c leading comment\n"
        "p cnf 4 3\n"
        "1\n"
        " c a comment may split a clause across physical lines\n"
        "-2 0 3 4 0\n"
        "\n"
        "-1 0\n"
        "c trailing comment\n");

    require(cnf.variable_count == 4, "wrong variable count");
    require(cnf.declared_clause_count == 3, "wrong declared clause count");
    require(cnf.clauses == std::vector<std::vector<int>>({{1, -2}, {3, 4}, {-1}}),
            "clauses were not parsed as a zero-terminated token stream");
}

void test_explicit_empty_clause_is_preserved()
{
    const ppcdcl::DimacsCnf cnf = parse("p cnf 2 2\n0 1 -2 0\n");
    require(cnf.clauses.size() == 2, "wrong clause count for empty-clause fixture");
    require(cnf.clauses[0].empty(), "explicit empty clause was discarded");
    require(cnf.clauses[1] == std::vector<int>({1, -2}),
            "clause following an empty clause was parsed incorrectly");

    const ppcdcl::DimacsCnf empty = parse("c no variables or clauses\np cnf 0 0\n");
    require(empty.variable_count == 0 && empty.clauses.empty(),
            "valid empty CNF was rejected or changed");
}

void test_header_validation()
{
    expect_parse_error("c only comments\n\n", "missing DIMACS 'p cnf' header");
    expect_parse_error("1 -2 0\n", "expected exactly");
    expect_parse_error("p sat 2 1\n1 0\n", "expected exactly");
    expect_parse_error("p cnf 2 1 extra\n1 0\n", "expected exactly");
    expect_parse_error("p cnf 2x 1\n1 0\n", "invalid variable count");
    expect_parse_error("p cnf 2 -1\n", "clause count must be between");
    expect_parse_error("p cnf 999999999999999999999999 0\n",
                       "out-of-range variable count");
}

void test_literal_validation_and_bounds()
{
    expect_parse_error("p cnf 2 1\n1 nope 0\n", "invalid literal");
    expect_parse_error("p cnf 2 1\n3 0\n", "outside the declared variable range");
    expect_parse_error("p cnf 2 1\n-3 0\n", "outside the declared variable range");
    expect_parse_error("p cnf 2147483647 1\n-2147483648 0\n",
                       "outside the declared variable range");
    expect_parse_error("p cnf 2 1\n999999999999999999999999 0\n",
                       "out-of-range literal");
    expect_parse_error("p cnf 2 1\n1 c inline comments are not comment lines\n",
                       "invalid literal");
}

void test_clause_count_and_terminator_validation()
{
    expect_parse_error("p cnf 2 2\n1 0\n", "header declares 2 clauses");
    expect_parse_error("p cnf 2 1\n1 0 -2 0\n",
                       "literal appears after all declared clauses");
    expect_parse_error("p cnf 2 1\n1 0 -2\n",
                       "literal appears after all declared clauses");
    expect_parse_error("p cnf 2 1\n1 -2\n", "missing its mandatory zero terminator");
    expect_parse_error("p cnf 2 0\n0\n", "more clauses than declared");
}

} // namespace

int main()
{
    test_token_stream_clause_boundaries();
    test_explicit_empty_clause_is_preserved();
    test_header_validation();
    test_literal_validation_and_bounds();
    test_clause_count_and_terminator_validation();
    std::cout << "DIMACS parser tests passed" << std::endl;
    return 0;
}
