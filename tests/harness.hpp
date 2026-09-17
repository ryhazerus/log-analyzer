#pragma once

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Tiny test harness: no external dependency, keeps the Docker builds hermetic.
namespace testing {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Case {
  const char* name;
  void (*fn)();
};

std::vector<Case>& cases();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

template <class A, class B>
std::string describe(const A& a, const B& b) {
  std::ostringstream o;
  o << "\n    left:  " << a << "\n    right: " << b;
  return o.str();
}

}  // namespace testing

#define TEST_CASE(name)                                             \
  static void name();                                               \
  static ::testing::Registrar name##_registrar(#name, &name);       \
  static void name()

#define CHECK(cond)                                                                                        \
  do {                                                                                                     \
    if (!(cond)) throw ::testing::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ") failed"); \
  } while (0)

#define CHECK_EQ(a, b)                                                                                      \
  do {                                                                                                      \
    const auto check_a_ = (a);                                                                              \
    const auto check_b_ = (b);                                                                              \
    if (!(check_a_ == check_b_))                                                                            \
      throw ::testing::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK_EQ(" #a ", " #b ") failed" + \
                               ::testing::describe(check_a_, check_b_));                                    \
  } while (0)

#define CHECK_THROWS(expr)                                                                                  \
  do {                                                                                                      \
    bool threw_ = false;                                                                                    \
    try {                                                                                                   \
      (void)(expr);                                                                                         \
    } catch (...) {                                                                                         \
      threw_ = true;                                                                                        \
    }                                                                                                       \
    if (!threw_) throw ::testing::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": expected " #expr " to throw"); \
  } while (0)
