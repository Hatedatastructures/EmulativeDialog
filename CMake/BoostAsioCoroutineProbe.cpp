#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/version.hpp>

#include <cstdio>
#include <exception>

namespace {

boost::asio::awaitable<int> RunProbe() {
  const auto Executor = co_await boost::asio::this_coro::executor;
  static_assert(requires { Executor.context(); });
  co_return 42;
}

} // namespace

int main() {
  if (BOOST_VERSION != 109200) {
    std::fprintf(stderr, "unexpected Boost version: %d\n", BOOST_VERSION);
    return 2;
  }

  boost::asio::io_context Context;
  int Result = 0;
  std::exception_ptr Failure;

  boost::asio::co_spawn(
      Context, RunProbe(),
      [&Result, &Failure](std::exception_ptr Exception, int Value) {
        Failure = std::move(Exception);
        Result = Value;
      });
  Context.run();

  if (Failure) {
    try {
      std::rethrow_exception(Failure);
    } catch (const std::exception& Exception) {
      std::fprintf(stderr, "coroutine probe failed: %s\n", Exception.what());
    } catch (...) {
      std::fprintf(stderr, "coroutine probe failed with unknown exception\n");
    }
    return 3;
  }
  if (Result != 42) {
    std::fprintf(stderr, "unexpected coroutine result: %d\n", Result);
    return 4;
  }

  std::puts("BoostAsioCxx20 coroutine result=42");
  return 0;
}
