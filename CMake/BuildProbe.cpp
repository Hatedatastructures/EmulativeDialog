#include <concepts>

template <typename Type>
concept BuildProbeIntegral = std::integral<Type>;

static_assert(__cplusplus >= 202002L, "BuildProbe requires C++20");
static_assert(BuildProbeIntegral<int>, "C++20 concepts must be available");

int main() {
  return 0;
}
