#pragma once

#include "emp-sh2pc/emp-sh2pc.h"
#include <iostream>
#include <memory>
#include <vector>
#include <time.h> /* time */

#include "clause.hpp"
#include "formula.hpp"
#include "heuristics.hpp"
#include "literal.hpp"
#include "model.hpp"
#include "private_stack.hpp"
#include "state.hpp"
#include "utils.hpp"

using emp::Bit;
using emp::Integer;
using std::unique_ptr;
using std::vector;

/**
 * SAT solver.
 * Initialized with a formula, invoke "solve" to check if there's a satisfying assignment of variables.
 */
class Solver
{
private:
	int ncls;
	int nvar;
	Heuristics h;

public:
	State current;
	unique_ptr<Formula> input_phi;
	unique_ptr<Formula> current_phi;
	PrivateStack<State> decisions;

	/** Construct from maximum number of variables and a formula */
	Solver(int _nvar, unique_ptr<Formula> const &_phi);

	/** Search unit clause in the current formula. If found, update the literal in current state. */
	Bit UnitSearch();

	/** Simplify the formula according to the current literal. */
	void propagation();

	/** Check the state:
	 * * Return 0: when current formula is empty.
	 * * Return 1: when current formula conflicts with current literal.
	 * * Return 2: otherwise.
	 */
	Integer check() const;

	/** Main routine of solver.
	 * It iterates at most "steps" iterations.
	 * "giantstep_test" designates whether halt after 1 iteration.
	 */
	unique_ptr<Model> solve(int steps = 100, bool giantstep_test = false, std::string heuristic_name = "wrand");

	void print(bool test);
};
