#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace imap2pst::test {

// Absolute path of tests/fixtures, injected by CMake.
inline std::string fixturePath(const std::string& name) {
    return std::string(IMAP2PST_FIXTURE_DIR) + "/" + name;
}

inline std::string readFixture(const std::string& name) {
    const std::string path = fixturePath(name);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open fixture " + path);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

}  // namespace imap2pst::test
