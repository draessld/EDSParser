#include "common.hpp"
#include "build_info.hpp"

#include <iostream>

namespace edsparser {

const char* build_commit() { return EDSPARSER_GIT_COMMIT; }

const char* build_commit_date() { return EDSPARSER_GIT_DATE; }

bool build_is_dirty() { return EDSPARSER_GIT_DIRTY != 0; }

void print_version(const std::string& tool_name) {
    std::cout << tool_name << " " << VERSION << "\n";
    std::cout << "COMMIT=" << build_commit() << "\n";
    std::cout << "COMMIT_DATE=" << build_commit_date() << "\n";
    std::cout << "DIRTY=" << (build_is_dirty() ? 1 : 0) << "\n";
}

} // namespace edsparser
