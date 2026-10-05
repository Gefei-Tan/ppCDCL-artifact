
#pragma once

#include "emp-sh2pc/emp-sh2pc.h"
#include <memory>
#include <string>

#include "literal.hpp"

using emp::Bit;
using emp::Integer;
using emp::PUBLIC;
using std::string;
using std::unique_ptr;

/**
 * Store a list of literals in bitmap format.
 */
class Model
{
private:
    int nvar;
    Bit *pos_vars;
    Bit *neg_vars;

public:
    /** Construct an empty bitmap. */
    Model(int _nvar);
    /** Construct from bitmap. */
    Model(Bit *_pos_var, Bit *_neg_var, int _nvar);
    /** Construct from a string. Only for test purpose. */
    Model(int nvar, string text);

    /** Construct an empty bitmap. */
    unique_ptr<Model> default_value() const;

    /** Insert a literal to model when real is true. */
    void update_index(int index, bool is_pos, Bit real);

    unique_ptr<Model> copy() const;

    void print(bool test) const;
    string toString() const;
    /** Check if model has designated literal. */
    Bit hasIndex(int index, bool is_pos) const;

    /** if use_rhs then return rhs, else return this. Note rhs and this must have the same nvar. */
    unique_ptr<Model> select(Bit b, unique_ptr<Model> const &rhs) const;

    /* Return the union of two models. */
    unique_ptr<Model> operator|(const Literal &rhs) const;

    /** Negate all literals. (Effectively make no change when model is empty.) */
    unique_ptr<Model> flip() const;

    ~Model();
};
