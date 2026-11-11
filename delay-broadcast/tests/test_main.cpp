#include "test_framework.h"

int main() {
    return dbs::TestRegistry::instance().runAll();
}
