#include <math.h>
#include <chrono>
#include <exception>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include "solver.hpp"
#include "utils.hpp"

// this microtest will print the time for each step and its components.

int main(int argc, char** argv) {

    std::srand(2);
    try {
        int port, party;
        party = std::atoi(argv[1]);
        port = std::atoi(argv[2]);
        int nvar = std::atoi(argv[3]);
        int ncls = std::atoi(argv[4]);
        int nltr = std::atoi(argv[5]);
        bool single_step_test = true;
        int number_of_steps = std::atoi(argv[6]);
        std::string hname = argv[7];
        /*
         * For the microtest, argv[6] should be set to 1.
         */
        single_step_test = number_of_steps== 1;

        std::cout<<"finish set up" << std::endl;
        emp::NetIO *io = new emp::NetIO(party == emp::ALICE ? nullptr : "127.0.0.1", port);
        emp::setup_semi_honest(io, party);

        /*
         * generate returns a formula of the requested size.
         * ncls -- number of clauses
         * nvar -- number of variables in the formula
         * nltr -- number of literals per clause
         */
        auto phi = generate(ncls, nvar, nltr, BICLAUSE, BILITERAL);
        std::cout << "finish generate\n";
        /*
         * generate creates a random formula. To solve a specific formula,
         * replace phi with a unique_ptr<Formula> built from an input string.
         * e.g. auto phi = make_unique<Formula>(nvar, "(1 2 3)(-1) (-2 3)");
         * the syntax of input string is the (\(-?[0-9]+\))+
         */

        Solver solver(nvar, phi);

        std::chrono::steady_clock sc;
        auto start = sc.now();
        auto model = solver.solve(number_of_steps + 1, single_step_test,hname);
        auto end = sc.now();
        auto time_span = static_cast<std::chrono::duration<double>>(end - start);
        std::cout << "total time: " << time_span.count() << std::endl;
        std::cout << nvar << " variables, " << nltr << " literals, " << ncls << " clauses.\n";
        std::cout << emp::CircuitExecution::circ_exec->num_and() << std::endl;
        delete io;
    }
    catch (char const *msg) {
            std::cout << "Main catch msg: " << msg << std::endl;
    }
    catch (const std::exception &e) {
            std::cout << "Main cat exception: " << e.what() << std::endl;
    }

}
