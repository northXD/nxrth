#include "mcp/mcp_server.h"

#include <cstring>

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
        return nxrth::mcp::run_self_test();
    return nxrth::mcp::run_stdio_server();
}
