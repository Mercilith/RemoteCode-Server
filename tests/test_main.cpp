#include <iostream>

#include "../src/greeting.h"

int main() {
    if (greeting() != "Hello, World!") {
        std::cerr << "greeting() returned unexpected value: " << greeting() << std::endl;
        return 1;
    }
    return 0;
}
