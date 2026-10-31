#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "redstone_solver - searches for Minecraft redstone circuits that satisfy a "
    "truth table.\n"
    "\n"
    "Usage: redstone_solver [--help]\n"
    "\n"
    "The solver and UI are not implemented yet; only the simulator core and "
    "its tests exist.\n";

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      return 0;
    }
    std::cerr << "unknown argument: " << arg << "\n\n" << kUsage;
    return 2;
  }
  std::cout << kUsage;
  return 0;
}
