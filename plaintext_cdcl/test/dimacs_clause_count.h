#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>

// The plaintext solver keeps an empty sentinel clause at phi[0]. Return the
// number of clauses declared by the DIMACS input, excluding the sentinel.
template <typename ClauseMap>
int dimacs_clause_count(const ClauseMap &phi)
{
    if (phi.find(0) == phi.end())
    {
        throw std::runtime_error("plaintext clause map is missing its phi[0] sentinel");
    }
    if (phi.size() - 1 > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::overflow_error("DIMACS clause count does not fit in int");
    }
    return static_cast<int>(phi.size() - 1);
}
