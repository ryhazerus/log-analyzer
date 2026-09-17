#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "checkpoint.hpp"
#include "harness.hpp"

using namespace podlogs;

namespace {
std::string temp_path(const char* name) {
  return (std::filesystem::temp_directory_path() / (std::string("podlogs-") + name + "-" + std::to_string(getpid()) + ".json")).string();
}
}  // namespace

TEST_CASE(checkpoint_roundtrip_and_monotonic_advance) {
  const std::string path = temp_path("cp");
  std::filesystem::remove(path);
  {
    Checkpoint cp(path);
    CHECK(!cp.load());
    cp.advance("abc", 100, "orders");
    cp.advance("abc", 50, "orders");  // older: ignored
    cp.advance("def", 7, "web");
    CHECK(cp.dirty());
    CHECK(cp.save());
    CHECK(!cp.dirty());
  }
  {
    Checkpoint cp(path);
    CHECK(cp.load());
    CHECK_EQ(cp.size(), size_t{2});
    CHECK_EQ(*cp.get("abc"), 100LL);
    CHECK_EQ(*cp.get("def"), 7LL);
    CHECK(!cp.get("nope"));
    cp.retain({"abc"});
    CHECK_EQ(cp.size(), size_t{1});
    cp.remove("abc");
    CHECK_EQ(cp.size(), size_t{0});
  }
  CHECK(!std::filesystem::exists(path + ".tmp"));
  std::filesystem::remove(path);
}

TEST_CASE(checkpoint_ignores_corrupt_file) {
  const std::string path = temp_path("corrupt");
  {
    std::ofstream out(path);
    out << "{ not json";
  }
  Checkpoint cp(path);
  CHECK(!cp.load());
  CHECK_EQ(cp.size(), size_t{0});
  std::filesystem::remove(path);
}
