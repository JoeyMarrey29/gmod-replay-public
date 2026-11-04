#include "source/GameContent.h"

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error("Usage: gmod-replay-content-tests <application-root> <demo-hint>");
        }
        const auto exact = gmod::source::content::mount_content(argv[1],"rp_paralake_city_v6",argv[2]);
        if (exact.bsp.filename() != L"rp_paralake_city_v6.bsp") {
            throw std::runtime_error("Recorded Paralake v6 was not selected exactly.");
        }
        const auto missing = gmod::source::content::mount_content(
            argv[1],"rp_paralake_city_v999999",argv[2]);
        if (!missing.bsp.empty()) {
            throw std::runtime_error("Unavailable recorded map was silently replaced by another version.");
        }
        std::cout << "PASS: recorded map selected exactly; unavailable versions are not substituted\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
