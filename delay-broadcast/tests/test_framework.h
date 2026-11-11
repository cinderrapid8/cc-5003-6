#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dbs {

struct TestCase {
    std::string name;
    std::function<bool()> fn;
};

class TestRegistry {
public:
    static TestRegistry& instance();
    void add(std::string name, std::function<bool()> fn);
    int runAll();

private:
    std::vector<TestCase> cases_;
};

#define DBS_TEST(NAME)                                                     \
    static bool NAME##_impl();                                             \
    namespace {                                                            \
    struct NAME##_reg {                                                    \
        NAME##_reg() { ::dbs::TestRegistry::instance().add(#NAME, NAME##_impl); } \
    } NAME##_reg_instance;                                                 \
    }                                                                      \
    static bool NAME##_impl()

void check(bool condition, const std::string& message);
std::int64_t checksRun();
std::int64_t checksFailed();

}  // namespace dbs
