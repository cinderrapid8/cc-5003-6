#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace dbs {
namespace tst {

// Local assertion sink, deliberately separate from the DBS_TEST registry so
// the standalone ring-acceptance entry and the registered suite can drive the
// same probe code with independent counters.
struct ProbeReport {
    std::int64_t checks = 0;
    std::int64_t failures = 0;
    std::vector<std::string> messages;
    bool loud = true;  // negative (fault-injection) runs suppress expected logs

    void expect(bool condition, const std::string& message) {
        ++checks;
        if (!condition) {
            ++failures;
            messages.push_back(message);
            if (loud) {
                std::fprintf(stderr, "    [PROBE-FAIL] %s\n", message.c_str());
            }
        }
    }

    bool ok() const { return failures == 0; }
};

}  // namespace tst
}  // namespace dbs
