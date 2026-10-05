#ifndef UTIL_H
#define UTIL_H

#include <vector>
#include <cassert>
#include <type_traits>

using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

static int COMPARE_N = 0;
static int IF_N = 0;

template <typename T>
inline const void assign_vector_in_place_range_bits(std::vector<T> &vec,
                                                    const std::vector<Bit> &idx_bits,
                                                    const T &data,
                                                    int l, int r,
                                                    int level,
                                                    const Bit &active)
{
    int n = r - l;

    if (n == 1)
    {
        vec[l] = If(active, data, vec[l]);
        return;
    }

    int mid = l + n / 2;
    Bit active_right = active & idx_bits[level];
    Bit active_left = active & !idx_bits[level];
    assign_vector_in_place_range_bits(vec, idx_bits, data, l, mid, level + 1, active_left);
    assign_vector_in_place_range_bits(vec, idx_bits, data, mid, r, level + 1, active_right);
}

/*
 * Sets vec[idx] = data at the secret index idx, only when flag is set. The vector is
 * padded to a power-of-two size during the call and restored to its original size
 * afterwards. Every entry is touched, so the access pattern does not depend on idx.
 */
template <typename T>
inline void assign_vector(std::vector<T> &vec, const Integer &idx, const T &data, Bit flag = true)
{
    int og_size = vec.size();
    // compute how many bits are needed to represent the vector size
    int bits_needed = 0;
    for (int i = 1; i < og_size; i <<= 1)
    {
        bits_needed++;
    }
    // pad vector size to the next power of 2
    if (std::is_same<T, Integer>::value)
    {
        vec.resize(1 << bits_needed, Integer(vec[0].size(), 0, PUBLIC));
    }
    else if (std::is_same<T, Bit>::value)
    {
        vec.resize(1 << bits_needed);
    }
    int n = 1 << bits_needed;
    Integer compact_idx = idx;
    compact_idx.resize(bits_needed, true);

    std::vector<Bit> idx_bits = compact_idx.bits;
    std::reverse(idx_bits.begin(), idx_bits.end());

    // Initially the whole vector is active; flag disables the write when it is false.
    Bit active = Bit(true, PUBLIC) & flag;
    assign_vector_in_place_range_bits(vec, idx_bits, data, 0, n, 0, active);
    vec.resize(og_size);
}

template <typename T>
inline const void read_vector_from_range_bits(const std::vector<T> &vec,
                                              const std::vector<Bit> &idx_bits,
                                              T &result,
                                              int l, int r,
                                              int level,
                                              const Bit &active)
{
    int n = r - l;

    if (n == 1)
    {
        // Leaf: the entry is copied into result only if it is on the active path.
        result = If(active, vec[l], result);
        return;
    }

    int mid = l + n / 2;
    Bit active_right = active & idx_bits[level];
    Bit active_left = active & !idx_bits[level];

    read_vector_from_range_bits(vec, idx_bits, result, l, mid, level + 1, active_left);
    read_vector_from_range_bits(vec, idx_bits, result, mid, r, level + 1, active_right);
}

/*
 * Reads vec[idx] into result for the secret index idx. The vector is padded to a
 * power-of-two size during the call and restored to its original size afterwards.
 * Every entry is touched, so the access pattern does not depend on idx.
 */
template <typename T>
inline void read_vector(std::vector<T> &vec, const Integer &idx, T &result)
{
    int og_size = vec.size();
    // compute how many bits are needed to represent the vector size
    int bits_needed = 0;
    for (int i = 1; i < og_size; i <<= 1)
    {
        bits_needed++;
    }

    // Pad the vector in place to a power-of-two size: Integer entries are filled with
    // zeros of the same width, Bit entries with false.
    if (std::is_same<T, Integer>::value)
    {
        vec.resize(1 << bits_needed, Integer(vec[0].size(), 0, PUBLIC));
    }
    else if (std::is_same<T, Bit>::value)
    {
        vec.resize(1 << bits_needed);
    }

    int n = 1 << bits_needed;

    Integer compact_idx = idx;
    compact_idx.resize(bits_needed, true);
    std::vector<Bit> idx_bits = compact_idx.bits;
    std::reverse(idx_bits.begin(), idx_bits.end());

    // Initially the whole vector is active.
    Bit active(true, PUBLIC);

    read_vector_from_range_bits(vec, idx_bits, result, 0, n, 0, active);
    vec.resize(og_size);
}

static inline int bits_required(int n)
{
    int count = 0;
    while (n > 0)
    {
        count++;
        n >>= 1;
    }
    return count;
}

static inline int actual_blk_size_to_pow_2(int n)
{
    return 1 << bits_required(n);
}

static inline string reveal_bool_string(const Integer input, const int cutoff = -1)
{
    // if cutoff is positive, print a space after every cutoff bits
    std::string res = "";
    int ctr = 0;
    for (const auto &bit : input.bits)
    {
        res += bit.reveal() ? "1" : "0";
        ctr++;
        if (cutoff > 0 && (ctr % cutoff == 0))
        {
            res += " ";
        }
    }
    return res;
}

static inline string reveal_int_32(Integer const input)
{
    Integer tmp = input;
    return std::to_string(tmp.resize(32).reveal<int32_t>());
}

static inline string reveal_int_details(Integer const input)
{
    std::string res = "";
    res += "size: " + std::to_string(input.size()) + "  ";
    res += "bool string: " + reveal_bool_string(input) + "  ";
    res += "int 32: " + reveal_int_32(input) + "  ";
    return res;
}

static inline int reveal_32(Integer const input, bool if_unsign = false)
{
    Integer tmp = input;
    return if_unsign ? tmp.resize(32,false).reveal<uint32_t>() : tmp.resize(32).reveal<int32_t>();
}

#endif // UTIL_H
