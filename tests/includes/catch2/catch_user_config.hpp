#pragma once

// Enable colorized output in all environments
#define CATCH_CONFIG_COLOUR_WINDOWS

// Disable exceptions for slightly faster tests (optional)
#define CATCH_CONFIG_DISABLE_EXCEPTIONS

// Reduce startup time by disabling unnecessary reporters
// #define CATCH_CONFIG_DISABLE_MATCHERS
// #define CATCH_CONFIG_DISABLE_BENCHMARKING

// Enable more strict floating-point comparisons
#define CATCH_CONFIG_FAST_COMPILE
