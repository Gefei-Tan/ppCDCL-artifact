#ifndef PPCDCL_DIMACS_H
#define PPCDCL_DIMACS_H

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ppcdcl
{

struct DimacsCnf
{
    int variable_count = 0;
    int declared_clause_count = 0;
    std::vector<std::vector<int>> clauses;
};

class DimacsParseError : public std::runtime_error
{
public:
    explicit DimacsParseError(const std::string &message)
        : std::runtime_error(message)
    {
    }
};

namespace dimacs_detail
{

inline std::string location(const std::string &source_name, std::size_t line_number)
{
    std::ostringstream out;
    out << source_name;
    if (line_number != 0)
    {
        out << ':' << line_number;
    }
    return out.str();
}

inline long long parse_decimal_integer(const std::string &token,
                                       const std::string &source_name,
                                       std::size_t line_number,
                                       const std::string &description)
{
    if (token.empty())
    {
        throw DimacsParseError(location(source_name, line_number) +
                               ": empty " + description);
    }

    std::size_t digit = 0;
    if (token[digit] == '+' || token[digit] == '-')
    {
        ++digit;
    }
    if (digit == token.size())
    {
        throw DimacsParseError(location(source_name, line_number) +
                               ": invalid " + description + " '" + token + "'");
    }
    for (; digit < token.size(); ++digit)
    {
        if (token[digit] < '0' || token[digit] > '9')
        {
            throw DimacsParseError(location(source_name, line_number) +
                                   ": invalid " + description + " '" + token + "'");
        }
    }

    errno = 0;
    char *end = nullptr;
    const long long value = std::strtoll(token.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0')
    {
        throw DimacsParseError(location(source_name, line_number) +
                               ": out-of-range " + description + " '" + token + "'");
    }
    return value;
}

inline bool is_blank_or_comment(const std::string &line)
{
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    return first == std::string::npos || line[first] == 'c';
}

} // namespace dimacs_detail

// Parse a DIMACS CNF token stream. Comments are whole lines whose first
// non-whitespace character is 'c'. Clause boundaries are determined solely by
// zero tokens, not by physical line boundaries.
inline DimacsCnf parse_dimacs(std::istream &input,
                             const std::string &source_name = "<stream>")
{
    DimacsCnf result;
    bool header_seen = false;
    std::vector<int> current_clause;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line))
    {
        ++line_number;
        if (dimacs_detail::is_blank_or_comment(line))
        {
            continue;
        }

        std::istringstream tokens(line);
        if (!header_seen)
        {
            std::vector<std::string> header_tokens;
            std::string token;
            while (tokens >> token)
            {
                header_tokens.push_back(token);
            }
            if (header_tokens.size() != 4 || header_tokens[0] != "p" ||
                header_tokens[1] != "cnf")
            {
                throw DimacsParseError(
                    dimacs_detail::location(source_name, line_number) +
                    ": expected exactly 'p cnf <variables> <clauses>'");
            }

            const long long variable_count = dimacs_detail::parse_decimal_integer(
                header_tokens[2], source_name, line_number, "variable count");
            const long long clause_count = dimacs_detail::parse_decimal_integer(
                header_tokens[3], source_name, line_number, "clause count");
            if (variable_count < 0 || variable_count > INT_MAX)
            {
                throw DimacsParseError(
                    dimacs_detail::location(source_name, line_number) +
                    ": variable count must be between 0 and INT_MAX");
            }
            if (clause_count < 0 || clause_count > INT_MAX)
            {
                throw DimacsParseError(
                    dimacs_detail::location(source_name, line_number) +
                    ": clause count must be between 0 and INT_MAX");
            }

            result.variable_count = static_cast<int>(variable_count);
            result.declared_clause_count = static_cast<int>(clause_count);
            header_seen = true;
            continue;
        }

        std::string token;
        while (tokens >> token)
        {
            const long long literal = dimacs_detail::parse_decimal_integer(
                token, source_name, line_number, "literal");
            if (literal == 0)
            {
                if (result.clauses.size() >=
                    static_cast<std::size_t>(result.declared_clause_count))
                {
                    throw DimacsParseError(
                        dimacs_detail::location(source_name, line_number) +
                        ": input contains more clauses than declared by the header");
                }
                result.clauses.push_back(current_clause);
                current_clause.clear();
                continue;
            }

            if (result.clauses.size() >=
                static_cast<std::size_t>(result.declared_clause_count))
            {
                throw DimacsParseError(
                    dimacs_detail::location(source_name, line_number) +
                    ": literal appears after all declared clauses were terminated");
            }
            if (literal < -static_cast<long long>(result.variable_count) ||
                literal > static_cast<long long>(result.variable_count))
            {
                throw DimacsParseError(
                    dimacs_detail::location(source_name, line_number) +
                    ": literal '" + token + "' is outside the declared variable range");
            }
            // The range check above also excludes LLONG_MIN/INT_MIN hazards and
            // guarantees that this cast is representable.
            current_clause.push_back(static_cast<int>(literal));
        }
    }

    if (input.bad())
    {
        throw DimacsParseError(source_name + ": I/O error while reading DIMACS input");
    }
    if (!header_seen)
    {
        throw DimacsParseError(source_name + ": missing DIMACS 'p cnf' header");
    }
    if (!current_clause.empty())
    {
        throw DimacsParseError(source_name +
                               ": final clause is missing its mandatory zero terminator");
    }
    if (result.clauses.size() !=
        static_cast<std::size_t>(result.declared_clause_count))
    {
        std::ostringstream message;
        message << source_name << ": header declares " << result.declared_clause_count
                << " clauses, but input contains " << result.clauses.size();
        throw DimacsParseError(message.str());
    }
    return result;
}

inline DimacsCnf parse_dimacs_file(const std::string &path)
{
    std::ifstream input(path);
    if (!input)
    {
        throw DimacsParseError(path + ": cannot open DIMACS input");
    }
    return parse_dimacs(input, path);
}

} // namespace ppcdcl

#endif // PPCDCL_DIMACS_H
