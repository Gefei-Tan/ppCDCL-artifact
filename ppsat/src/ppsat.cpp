#include <math.h>
#include "formula.hpp"
#include <memory>
#include <chrono>
#include <exception>
#include <cstdlib>
#include <ctime>
#include "clause.hpp"
#include "literal.hpp"
#include "model.hpp"
#include "utils.hpp"
#include "solver.hpp"



int main(int argc, char** argv) {

    try {

        int port, party;

        parse_party_and_port(argv, &party, &port);
        emp::NetIO *io = new emp::NetIO(party == emp::ALICE ? nullptr : "127.0.0.1", port);
        emp::setup_semi_honest(io, party);

        int number_of_steps = std::atoi(argv[3]);
        int nvar = std::atoi(argv[4]);
        auto phi = std::make_unique<Formula>(nvar, argv[5]);
        std::cout << "input formula: \n";
        phi->print(true);
        Solver solver(nvar, phi);
        auto model = solver.solve(number_of_steps, false);
        std::cout << model->toString() << std::endl;
        delete io;
    }

    catch (char const *msg) {
        std::cout << "Main catch msg: " << msg << std::endl;
    }
    catch (const std::exception &e) {
        std::cout << "Main cat exception: " << e.what() << std::endl;
    }


}
