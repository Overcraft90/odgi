#include "subcommand.hpp"
#include "odgi.hpp"
#include "args.hxx"
#include <omp.h>
#include "algorithms/unroll.hpp"

namespace odgi {

using namespace odgi::subcommand;

int main_unroll(int argc, char** argv) {

    // trick argumentparser to do the right thing with the subcommand
    for (uint64_t i = 1; i < argc-1; ++i) {
        argv[i] = argv[i+1];
    }
    std::string prog_name = "odgi unroll";
    argv[0] = (char*)prog_name.c_str();
    --argc;
    
    args::ArgumentParser parser("unroll looping components of the graph");
    args::HelpFlag help(parser, "help", "display this help summary", {'h', "help"});
    args::ValueFlag<std::string> og_in_file(parser, "FILE", "load the graph from this file", {'i', "idx"});
    args::ValueFlag<std::string> og_out_file(parser, "FILE", "store the graph self index in this file", {'o', "out"});
    args::ValueFlag<uint64_t> nthreads(parser, "N", "number of threads to use for parallel operations", {'t', "threads"});
    args::Flag debug(parser, "debug", "print information about the process to stderr.", {'d', "debug"});

    try {
        parser.ParseCLI(argc, argv);
    } catch (args::Help) {
        std::cout << parser;
        return 0;
    } catch (args::ParseError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }
    if (argc==1) {
        std::cout << parser;
        return 1;
    }

    if (!og_in_file) {
        std::cerr << "[odgi::unroll] error: please specify an input file from where to load the graph via -i=[FILE], --idx=[FILE]." << std::endl;
        return 1;
    }

    if (!og_out_file) {
        std::cerr << "[odgi::unroll] error: please specify an output file to where to store the unrollped graph via -o=[FILE], --out=[FILE]." << std::endl;
        return 1;
    }

    graph_t graph;
    assert(argc > 0);
    std::string infile = args::get(og_in_file);
    if (infile.size()) {
        if (infile == "-") {
            graph.deserialize(std::cin);
        } else {
            ifstream f(infile.c_str());
            graph.deserialize(f);
            f.close();
        }
    }

    const uint64_t num_threads = nthreads ? args::get(nthreads) : 1;
    graph.set_number_of_threads(num_threads);

    graph_t output;
    algorithms::unroll(graph, output);
    
    std::string outfile = args::get(og_out_file);
    if (outfile.size()) {
        if (outfile == "-") {
            output.serialize(std::cout);
        } else {
            ofstream f(outfile.c_str());
            output.serialize(f);
            f.close();
        }
    }
    return 0;
}

static Subcommand odgi_unroll("unroll", "unroll the graph to prevent self-looping",
                              PIPELINE, 3, main_unroll);


}
