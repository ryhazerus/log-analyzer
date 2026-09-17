#include <csignal>
#include <cstdio>
#include <cstring>

#include "harness.hpp"

namespace testing {
std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
}  // namespace testing

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int passed = 0, failed = 0;
  for (const auto& c : testing::cases()) {
    if (filter && std::strstr(c.name, filter) == nullptr) continue;
    try {
      c.fn();
      std::printf("  ok   %s\n", c.name);
      ++passed;
    } catch (const std::exception& e) {
      std::printf("  FAIL %s\n       %s\n", c.name, e.what());
      ++failed;
    }
  }
  std::printf("%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
