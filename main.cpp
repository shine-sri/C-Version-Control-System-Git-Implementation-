#include <iostream>
#include <string>
#include <vector>
#include "libwyag.h"

/**
 * @brief WYAG (Write Yourself a Git) C++ Engine
 * Acts as the main command line dispatcher routing sys args 
 * to the underlying Git-equivalent sub-commands.
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: wyag <command> [args]\n";
        return 1;
    }

    std::string command = argv[1];

    // Pack remaining shell arguments into a C++ vector for easy parsing downstream
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    try {
        // Route to the appropriate command algorithm mapped in libwyag.cpp
        if (command == "add")               cmd_add(args);
        else if (command == "cat-file")     cmd_cat_file(args);
        else if (command == "check-ignore") cmd_check_ignore(args);
        else if (command == "checkout")     cmd_checkout(args);
        else if (command == "commit")       cmd_commit(args);
        else if (command == "hash-object")  cmd_hash_object(args);
        else if (command == "init")         cmd_init(args);
        else if (command == "log")          cmd_log(args);
        else if (command == "ls-files")     cmd_ls_files(args);
        else if (command == "ls-tree")      cmd_ls_tree(args);
        else if (command == "rev-parse")    cmd_rev_parse(args);
        else if (command == "rm")           cmd_rm(args);
        else if (command == "show-ref")     cmd_show_ref(args);
        else if (command == "status")       cmd_status(args);
        else if (command == "tag")          cmd_tag(args);
        else {
            std::cerr << "wyag: '" << command << "' is not a wyag command.\n";
            return 1;
        }
    } catch (const std::exception& e) {
        // Global exception handling ensures neat CLI exits without dumping core 
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}