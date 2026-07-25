#include "test_framework.h"
#include <iostream>

TEST(Debug, SimpleTest) {
    ASSERT_TRUE(true);
}

int main() {
    std::cerr << "Registry size: " << dice::test::registry().size() << std::endl;
    std::cout << "Running tests..." << std::endl;
    return dice::test::runAll();
}
