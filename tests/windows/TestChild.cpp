#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
  if (argc < 1) return 2;
  // Both fake children exit immediately; no real daemon or user state is used.
  const auto name = std::filesystem::path(argv[0]).filename();
  std::ofstream(name.string() + ".started") << "started\n";
}
