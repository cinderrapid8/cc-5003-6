#include "test_framework.h"

#include <cstdio>
#include <atomic>

namespace dbs {

namespace {
std::atomic<std::int64_t> g_checks{0};
std::atomic<std::int64_t> g_failures{0};
thread_local std::string t_caseName;
}  // namespace

void check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "    [FAIL] %s: %s\n", t_caseName.c_str(), message.c_str());
    }
}

std::int64_t checksRun() { return g_checks.load(); }
std::int64_t checksFailed() { return g_failures.load(); }

TestRegistry& TestRegistry::instance() {
    static TestRegistry registry;
    return registry;
}

void TestRegistry::add(std::string name, std::function<bool()> fn) {
    cases_.push_back({std::move(name), std::move(fn)});
}

int TestRegistry::runAll() {
    int failedCases = 0;
    std::printf("== delay-broadcast engine test suite ==\n");
    for (const auto& tc : cases_) {
        t_caseName = tc.name;
        std::printf("[test] %s\n", tc.name.c_str());
        const std::int64_t beforeFail = g_failures.load();
        bool ok = false;
        try {
            ok = tc.fn();
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "    [FAIL] exception: %s\n", ex.what());
            ok = false;
        }
        if (!ok || g_failures.load() > beforeFail) ++failedCases;
    }
    std::printf("\n%lld checks, %lld failures, %d/%zu cases failed\n",
                static_cast<long long>(g_checks.load()),
                static_cast<long long>(g_failures.load()),
                failedCases, cases_.size());
    return failedCases == 0 ? 0 : 1;
}

}  // namespace dbs
