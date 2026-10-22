#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "px/core/Log.hpp"

int main(int argc, char** argv) {
    // Keep test output readable; individual tests capture logs when needed.
    px::log::setMinLevel(px::log::Level::Warn);
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
