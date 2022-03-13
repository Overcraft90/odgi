#include "subcommand.hpp"
#include "odgi.hpp"
#include "position.hpp"
#include "args.hxx"
#include "split.hpp"
#include <omp.h>
#include <regex>
#include "utils.hpp"
#include "atomic_bitvector.hpp"
#include "src/algorithms/subgraph/extract.hpp"

namespace odgi {

    using namespace odgi::subcommand;


    int main_extract(int argc, char **argv) {

        // trick argumentparser to do the right thing with the subcommand
        for (uint64_t i = 1; i < argc - 1; ++i) {
            argv[i] = argv[i + 1];
        }
        const std::string prog_name = "odgi extract";
        argv[0] = (char *) prog_name.c_str();
        --argc;

        args::ArgumentParser parser("Extract subgraphs or parts of a graph defined by query criteria.");
        args::Group mandatory_opts(parser, "[ MANDATORY OPTIONS ]");
        args::ValueFlag<std::string> og_in_file(mandatory_opts, "FILE", "Load the succinct variation graph in ODGI format from this *FILE*. The file name usually ends with *.og*. It also accepts GFAv1, but the on-the-fly conversion to the ODGI format requires additional time!", {'i', "idx"});
        args::Group graph_files_io_opts(parser, "[ Graph Files IO ]");
        args::ValueFlag<std::string> og_out_file(graph_files_io_opts, "FILE", "Store all subgraphs in this FILE. The file name usually ends with *.og*.",
                                                 {'o', "out"});
        args::Group extract_opts(parser, "[ Extract Options ]");
        args::Flag _split_subgraphs(extract_opts, "split_subgraphs",
                                    "Instead of writing the target subgraphs into a single graph, "
                                    "write one subgraph per given target to a separate file named path:start-end.og "
                                    "(0-based coordinates).", {'s', "split-subgraphs"});
        args::Flag _inverse(extract_opts, "inverse",
                               "Extract the parts of the graph that do not meet the query criteria.",
                               {'I', "inverse"});
        args::ValueFlag<uint64_t> _target_node(extract_opts, "ID", "A single node ID from which to begin our traversal.",
                                               {'n', "node"});
        args::ValueFlag<std::string> _node_list(extract_opts, "FILE", "A file with one node id per line. The node specified will be extracted from the input graph.", {'l', "node-list"});
        args::ValueFlag<uint64_t> _context_steps(extract_opts, "N",
                                                "The number of steps (nodes) away from our initial subgraph that we should collect.",
                                                {'c', "context-steps"});
        args::ValueFlag<uint64_t> _context_bases(extract_opts, "N",
                                   "The number of bases away from our initial subgraph that we should collect.",
                                   {'L', "context-bases"});
        args::ValueFlag<std::string> _path_range(extract_opts, "STRING",
                                                 "Find the node(s) in the specified path range TARGET=path[:pos1[-pos2]] "
                                                 "(0-based coordinates).", {'r', "path-range"});
        args::ValueFlag<std::string> _path_bed_file(extract_opts, "FILE",
                                                    "Find the node(s) in the path range(s) specified in the given BED FILE.",
                                                    {'b', "bed-file"});
        args::ValueFlag<std::string> _pangenomic_range(extract_opts, "STRING",
                                                       "Find the node(s) in the specified pangenomic range pos1-pos2 (0-based coordinates). "
                                                       "The nucleotide positions refer to the pangenome’s sequence (i.e., "
                                                       "the sequence obtained arranging all the graph’s node from left to right)."
                                                       , {'q', "pangenomic-range"});
        args::Flag _full_range(extract_opts, "full_range",
                               "Collects all nodes in the sorted order of the graph in the min and max positions touched by the given path ranges. "
                               "This ensures that all the paths of the subgraph are not split by node, but that the nodes are laced together again. "
                               "Comparable to **-R, --lace-paths=FILE**, but specifically for all paths in the resulting subgraph. "
                               "Be careful to use it with very complex graphs.",
                               {'E', "full-range"});
        args::ValueFlag<std::string> _path_names_file(extract_opts, "FILE",
                                                      "List of paths to consider in the extraction. The FILE must "
                                                      "contain one path name per line and a subset of all paths can be specified.",
                                                      {'p', "paths-to-extract"});
        args::ValueFlag<std::string> _lace_paths_file(extract_opts, "FILE",
                                                       "List of paths to fully retain in the extracted graph. Must "
                                                       "contain one path name per line and a subset of all paths can be specified.",
                                                      {'R', "lace-paths"});
        args::Group threading_opts(parser, "[ Threading ]");
        args::ValueFlag<uint64_t> nthreads(threading_opts, "N", "Number of threads to use for parallel operations.",
                                           {'t', "threads"});
        args::Group processing_info_opts(parser, "[ Processing Information ]");
        args::Flag _show_progress(processing_info_opts, "progress",
                                  "Print information about the operations and the progress to stderr.",
                                  {'P', "progress"});
        args::Group program_info_opts(parser, "[ Program Information ]");
        args::HelpFlag help(program_info_opts, "help", "Print a help message for odgi extract.", {'h', "help"});

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
        if (argc == 1) {
            std::cout << parser;
            return 1;
        }

        if (!og_in_file) {
            std::cerr
                    << "[odgi::extract] error: please specify an input file from where to load the graph via -i=[FILE], --idx=[FILE]."
                    << std::endl;
            return 1;
        }

        if (_context_steps && _context_bases) {
            std::cerr << "[odgi::extract] error: please specify the expanding context either in steps (with -c/--context-steps) or"
                         "in bases (-L/--context-bases), not both." << std::endl;
            return 1;
        }

        if (_split_subgraphs) {
            if (og_out_file) {
                std::cerr << "[odgi::extract] error: please do not specify an output file (with -o/--out) when "
                             "one subgraph per given target is requested (with -s/--split-subgraphs)." << std::endl;
                return 1;
            }

            if (_target_node) {
                std::cerr << "[odgi::extract] error: please do not specify a single node (with -n/--node) when "
                             "one subgraph per given target is requested (with -s/--split-subgraphs)." << std::endl;
                return 1;
            }

            if (_node_list) {
                std::cerr << "[odgi::extract] error: please do not specify a node list (with -l/--list) when "
                             "one subgraph per given target is requested (with -s/--split-subgraphs)." << std::endl;
                return 1;
            }

            if (_inverse) {
                std::cerr << "[odgi::extract] error: please do not specify an inverse query (with -I/--inverse) when "
                             "one subgraph per given target is requested (with -s/--split-subgraphs)." << std::endl;
                return 1;
            }

            if (_pangenomic_range) {
                std::cerr << "[odgi::extract] error: please do not specify a pangenomic range (with -q/--pangenomic-range) when "
                             "one subgraph per given target is requested (with -s/--split-subgraphs)." << std::endl;
                return 1;
            }
        } else {
            if (!og_out_file) {
                std::cerr << "[odgi::extract] error: please specify an output file to where to store the subgraph via "
                             "-o=[FILE], --out=[FILE]." << std::endl;
                return 1;
            }
        }

		const uint64_t num_threads = args::get(nthreads) ? args::get(nthreads) : 1;

		graph_t graph;
        assert(argc > 0);
        {
            const std::string infile = args::get(og_in_file);
            if (!infile.empty()) {
                if (infile == "-") {
                    graph.deserialize(std::cin);
                } else {
					utils::handle_gfa_odgi_input(infile, "extract", args::get(_show_progress), num_threads, graph);
                }
            }
        }

        const uint64_t shift = graph.min_node_id();
        if (graph.max_node_id() - shift >= graph.get_node_count()){
            std::cerr << "[odgi::extract] error: the node IDs are not compacted. Please run 'odgi sort' using -O, --optimize to optimize the graph." << std::endl;
            exit(1);
        }

        // Prepare all paths for parallelize the next step (actually, not all paths are always present in the subgraph)
        std::vector<path_handle_t> paths;
        if (args::get(_path_names_file).empty()) {
            paths.reserve(graph.get_path_count());
            graph.for_each_path_handle([&](const path_handle_t path) {
                paths.push_back(path);
            });
        } else {
            std::ifstream path_names_in(args::get(_path_names_file));

            uint64_t num_of_paths_in_file = 0;

            std::vector<bool> path_already_seen;
            path_already_seen.resize(graph.get_path_count(), false);

            std::string line;
            while (std::getline(path_names_in, line)) {
                if (!line.empty()) {
                    if (graph.has_path(line)) {
                        const path_handle_t path = graph.get_path_handle(line);
                        const uint64_t path_rank = as_integer(path) - 1;
                        if (!path_already_seen[path_rank]) {
                            path_already_seen[path_rank] = true;
                            paths.push_back(path);
                        } else {
                            std::cerr << "[odgi::extract] error: in the path list there are duplicated path names."
                                      << std::endl;
                            exit(1);
                        }
                    }

                    ++num_of_paths_in_file;
                }
            }

            path_names_in.close();

            std::cerr << "[odgi::extract] found " << paths.size() << "/" << num_of_paths_in_file
                      << " paths to consider." << std::endl;

            if (paths.empty()) {
                std::cerr << "[odgi::extract] error: no path to consider." << std::endl;
                exit(1);
            }
        }

        std::vector<path_handle_t> lace_paths;
        if (!args::get(_lace_paths_file).empty()) {
            ska::flat_hash_set<path_handle_t> lace_paths_set;
            std::ifstream path_names_in(args::get(_lace_paths_file));
            uint64_t num_of_paths_in_file = 0;
            std::string line;
            while (std::getline(path_names_in, line)) {
                if (!line.empty()
                    && graph.has_path(line)) {
                    auto path = graph.get_path_handle(line);
                    if (!lace_paths_set.count(path)) {
                        lace_paths.push_back(path);
                        lace_paths_set.insert(path);
                    }
                }
            }
            path_names_in.close();
            if (lace_paths.empty()) {
                std::cerr << "[odgi::extract] error: no path to fully retain." << std::endl;
                exit(1);
            }
        }

        std::vector<odgi::path_range_t> path_ranges;

        // handle targets from BED
        if (_path_bed_file && !args::get(_path_bed_file).empty()) {
            std::ifstream bed_in(args::get(_path_bed_file));
            std::string line;
            while (std::getline(bed_in, line)) {
                add_bed_range(path_ranges, graph, line);
            }
        }

        // handle targets from command line
        if (_path_range) {
            Region region;
            parse_region(args::get(_path_range), region);

            if (!graph.has_path(region.seq)) {
                std::cerr
                        << "[odgi::extract] error: path " << region.seq << " not found in the input graph."
                        << std::endl;
                exit(1);
            }

            // no coordinates given, we do whole thing (0,-1)
            if (region.start < 0 && region.end < 0) {
                add_bed_range(path_ranges, graph, region.seq);
            } else {
                add_bed_range(path_ranges, graph, region.seq + "\t" + std::to_string(region.start) + "\t" + std::to_string(region.end));
            }
        }

        std::pair<uint64_t, uint64_t> pangenomic_range = {0, 0};
        if (_pangenomic_range && !args::get(_pangenomic_range).empty()) {
            const std::string pangenomic_range_str = args::get(_pangenomic_range);

            const std::regex regex("-");
            const std::vector<std::string> splitted(
                    std::sregex_token_iterator(pangenomic_range_str.begin(), pangenomic_range_str.end(), regex, -1),
                    std::sregex_token_iterator()
                    );

            if (splitted.size() != 2) {
                std::cerr
                        << "[odgi::extract] error: please specify a valid pangenomic range: start-end."
                        << std::endl;
                return 1;
            }

            if (!utils::is_number(splitted[0]) || !utils::is_number(splitted[1]) || stoull(splitted[0]) > stoull(splitted[1])) {
                std::cerr
                << "[odgi::extract] error: please specify valid numbers for the pangenomic range."
                << std::endl;
                return 1;
            }

            pangenomic_range.first = stoull(splitted[0]);
            pangenomic_range.second = stoull(splitted[1]);
        }

        if (_split_subgraphs && path_ranges.empty()) {
            std::cerr << "[odgi::extract] error: please specify at least one target when "
                         "one subgraph per given target is requested (with -s/--split-subgraphs)." << std::endl;
            exit(1);
        }

        const bool show_progress = args::get(_show_progress);
        const uint64_t context_steps = _context_steps ? args::get(_context_steps) : 0;
        const uint64_t context_bases = _context_bases ? args::get(_context_bases) : 0;

        omp_set_num_threads((int) num_threads);

        auto prep_graph = [](graph_t &source, const std::vector<path_handle_t>& source_paths,
                             const std::vector<path_handle_t>& lace_paths, graph_t &subgraph,
                             const uint64_t context_steps, const uint64_t context_bases, const bool full_range, const bool inverse,
                             const uint64_t num_threads, const bool show_progress) {
            if (context_steps > 0 || context_bases > 0) {
                if (show_progress) {
                    std::cerr << "[odgi::extract] expansion and adding connecting edges" << std::endl;
                }

                if (context_steps > 0) {
                    algorithms::expand_subgraph_by_steps(source, subgraph, context_steps, false);
                } else {
                    algorithms::expand_subgraph_by_length(source, subgraph, context_bases, false);
                }
            }

            if (full_range) {
                // Take the start and end node of this and fill things in
                algorithms::extract_id_range(source, subgraph.min_node_id(), subgraph.max_node_id() , subgraph, show_progress
                                                                                 ? "[odgi::extract] collecting all nodes in the path range"
                                                                                 : "");
            }

            if (inverse) {
                unordered_set<nid_t> node_ids_to_ignore;
                subgraph.for_each_handle([&](const handle_t &h) {
                    node_ids_to_ignore.insert(subgraph.get_id(h));
                });

                std::unique_ptr<algorithms::progress_meter::ProgressMeter> progress;
                if (show_progress) {
                    progress = std::make_unique<algorithms::progress_meter::ProgressMeter>(
                            source.get_node_count() - node_ids_to_ignore.size(), "[odgi::extract] inverting the query criteria");
                }

                subgraph.clear();

                source.for_each_handle([&](const handle_t &h) {
                    nid_t id = source.get_id(h);
                    if (node_ids_to_ignore.count(id) <= 0) {
                        subgraph.create_handle(source.get_sequence(source.get_handle(id)), id);

                        if (show_progress) {
                            progress->increment(1);
                        }
                    }
                });

                if (show_progress) {
                    progress->finish();
                }
            }

            // rewrite lace paths so that skipped regions are represented as new nodes that we then add to our subgraph
            if (!lace_paths.empty()) {
                if (show_progress) {
                    std::cerr << "[odgi::extract] adding " << lace_paths.size() << " lace paths" << std::endl;
                }

                algorithms::embed_lace_paths(source, subgraph, lace_paths);
            }

            // Connect the collected handles
            algorithms::add_connecting_edges_to_subgraph(source, subgraph, show_progress
                                                                           ? "[odgi::extract] adding connecting edges"
                                                                           : "");

            // Add subpaths covering the collected handles
            algorithms::add_subpaths_to_subgraph(source, source_paths, subgraph, num_threads,
                                                 show_progress ? "[odgi::extract] adding subpaths" : "");


            std::vector<path_handle_t> subpaths;
            subpaths.reserve(subgraph.get_path_count());
            subgraph.for_each_path_handle([&](const path_handle_t& path) {
                subpaths.push_back(path);
            });

            std::unique_ptr<algorithms::progress_meter::ProgressMeter> progress_checking;
            if (show_progress) {
                progress_checking = std::make_unique<algorithms::progress_meter::ProgressMeter>(
                        subpaths.size(), "[odgi::extract] checking missing edges");
            }

            ska::flat_hash_set<std::pair<handle_t, handle_t>> edges_to_create;

#pragma omp parallel for schedule(dynamic, 1) num_threads(num_threads)
            for (auto path: subpaths) {
                handle_t last;
                const step_handle_t begin_step = subgraph.path_begin(path);
                subgraph.for_each_step_in_path(path, [&](const step_handle_t &step) {
                    handle_t h = subgraph.get_handle_of_step(step);
                    if (step != begin_step && !subgraph.has_edge(last, h)) {
#pragma omp critical (edges_to_create)
                        edges_to_create.insert({last, h});
                    }
                    last = h;
                });

                if (show_progress) {
                    progress_checking->increment(1);
                }
            }

            if (show_progress) {
                progress_checking->finish();
            }

            subpaths.clear();

            // force embed the paths
            for (auto edge: edges_to_create) {
                subgraph.create_edge(edge.first, edge.second);
            }

            if (show_progress && edges_to_create.size() > 0) {
                std::cerr << "[odgi::extract] fixed " << edges_to_create.size() << " edge(s)" << std::endl;
            }

            // This should not be necessary, if the extraction works correctly
            // subgraph.remove_orphan_edges();
        };

        auto check_and_create_handle = [&](const graph_t &source, graph_t &subgraph, const nid_t node_id) {
            if (graph.has_node(node_id)) {
                if (!subgraph.has_node(node_id)){
                    const handle_t cur_handle = graph.get_handle(node_id);
                    subgraph.create_handle(
                            source.get_sequence(source.get_is_reverse(cur_handle) ? source.flip(cur_handle) : cur_handle),
                            node_id);
                }
            } else {
                std::cerr << "[odgi::extract] warning, cannot find node " << node_id << std::endl;
            }
        };

        if (_split_subgraphs) {
            for (auto &path_range : path_ranges) {
                graph_t subgraph;

                const path_handle_t path_handle = path_range.begin.path;

                if (show_progress) {
                    std::cerr << "[odgi::extract] extracting path range " << graph.get_path_name(path_range.begin.path) << ":" << path_range.begin.offset
                              << "-"
                              << path_range.end.offset << std::endl;
                }

                algorithms::extract_path_range(graph, path_handle, path_range.begin.offset, path_range.end.offset, subgraph);

                prep_graph(graph, paths, lace_paths, subgraph, context_steps, context_bases, _full_range, false, num_threads, show_progress);

                const string filename = graph.get_path_name(path_range.begin.path) + ":" + to_string(path_range.begin.offset) + "-" + to_string(path_range.end.offset) + ".og";

                if (show_progress) {
                    std::cerr << "[odgi::extract] writing " << filename << std::endl;
                }

                ofstream f(filename);
                subgraph.serialize(f);
                f.close();
            }
        } else {
            std::unique_ptr<algorithms::progress_meter::ProgressMeter> progress;
            if (show_progress) {
                progress = std::make_unique<algorithms::progress_meter::ProgressMeter>(
                    path_ranges.size(), "[odgi::extract] extracting path ranges");
            }

            graph_t subgraph;
            {
                atomicbitvector::atomic_bv_t keep_bv(graph.get_node_count()+1);
                // collect path ranges by path
#pragma omp parallel for schedule(dynamic,1)
                for (auto &path_range : path_ranges) {
                    const path_handle_t path_handle = path_range.begin.path;
                    if (show_progress) {
                        progress->increment(1);
                    }
                    algorithms::for_handle_in_path_range(
                        graph, path_handle, path_range.begin.offset, path_range.end.offset,
                        [&](const handle_t& handle) {
                            keep_bv.set(graph.get_id(handle) - shift);
                        });
                }
                if (_pangenomic_range) {
                    uint64_t len = 0;
                    graph.for_each_handle([&](const handle_t &h) {
                        const uint64_t hl = graph.get_length(h);

                        if (len <= pangenomic_range.second && pangenomic_range.first <= len + hl) {
                            keep_bv.set(graph.get_id(h) - shift);
                        }

                        len += hl;
                    });
                }
                for (auto id_shifted : keep_bv) {
                    const handle_t h = graph.get_handle(id_shifted + shift);
                    subgraph.create_handle(graph.get_sequence(h),
                                           id_shifted + shift);
                }
            }

            if (show_progress) {
                progress->finish();
            }

            if (args::get(_target_node)) {
                check_and_create_handle(graph, subgraph, args::get(_target_node));
            }

            if (!args::get(_node_list).empty()) {
                ifstream nodes(args::get(_node_list));
                std::string next;
                while (std::getline(nodes, next)) {
                    check_and_create_handle(graph, subgraph, std::stol(next));
                }
            }

            prep_graph(graph, paths, lace_paths, subgraph, context_steps, context_bases, _full_range, _inverse, num_threads, show_progress);

            {
                const std::string outfile = args::get(og_out_file);
                if (!outfile.empty()) {
                    if (outfile == "-") {
                        subgraph.serialize(std::cout);
                    } else {
                        ofstream f(outfile.c_str());
                        subgraph.serialize(f);
                        f.close();
                    }
                }
            }
        }

        return 0;
    }

    static Subcommand odgi_extract("extract", "Extract subgraphs or parts of a graph defined by query criteria.",
                                   PIPELINE, 3, main_extract);

}
