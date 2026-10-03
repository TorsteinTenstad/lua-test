#include <lua.hpp>

#include <cstdio>
#include <expected>
#include <print>
#include <string>
#include <type_traits>
#include <variant>

void pop_from_lua(lua_State *l, int index, double &value) {
  value = luaL_checknumber(l, index);
}
void pop_from_lua(lua_State *l, int index, int &value) {
  value = luaL_checkinteger(l, index);
}
void pop_from_lua(lua_State *l, int index, std::string &value) {
  const char *cString = luaL_checkstring(l, index);
  value = std::string(cString);
}
int push_to_lua(lua_State *l, double value) {
  lua_pushnumber(l, value);
  return 1;
}

template <class... Ts>
void pop_tuple_from_lua(lua_State *l, std::tuple<Ts &...> t) {

  int index = 1;
  std::apply([&](auto &...out) { (pop_from_lua(l, index++, out), ...); }, t);
}

enum class ExecutionStatus {};
int ticks = 0;

struct Add {
  static constexpr auto name{"Add"};
  double a;
  double b;

  std::expected<double, ExecutionStatus> execute() { return a + b; }
  auto tuple() { return std::tie(a, b); }
};

struct Greet {
  static constexpr auto name{"Greet"};
  std::string person;

  std::expected<void, ExecutionStatus> execute() {
    std::println("Greetings, {}!", person);
    return {};
  }
  auto tuple() { return std::tie(person); }
};

struct WaitTick {
  static constexpr auto name{"WaitTick"};
  int tick{};

  std::expected<void, ExecutionStatus> execute() {
    if (ticks < tick) {
      return std::unexpected<ExecutionStatus>({});
    }
    return {};
  }
  auto tuple() { return std::tie(tick); }
};

using ScriptFunction = std::variant<Add, Greet, WaitTick>;

ScriptFunction currentFunction;

using ExecuteResult = std::expected<int, ExecutionStatus>;

static ExecuteResult execute(lua_State *l, ScriptFunction scriptFunction) {
  return std::visit(
      [=](auto &&f) -> ExecuteResult {
        std::println("Calling {} with args {}",
                     std::remove_cvref_t<decltype(f)>::name, f.tuple());
        const auto returnValue = f.execute();
        if (returnValue.has_value()) {
          if constexpr (std::is_void_v<decltype(returnValue.value())>) {
            return 0;
          } else {
            return push_to_lua(l, returnValue.value());
          }
        }
        return std::unexpected(returnValue.error());
      },
      scriptFunction);
}

static int resume(lua_State *L, int, lua_KContext) {
  ExecuteResult result = execute(L, currentFunction);
  if (!result.has_value()) {
    return lua_yieldk(L, 0, 0, resume);
  }
  return result.value();
}

static int try_execute(lua_State *L, ScriptFunction scriptFunction) {
  currentFunction = scriptFunction;
  return resume(L, 0, 0);
}

template <typename F> static int function_available_to_lua(lua_State *l) {
  F f;
  pop_tuple_from_lua(l, f.tuple());
  return try_execute(l, f);
}
template <typename F> static void push_function_to_lua(lua_State *l) {
  lua_pushcfunction(l, function_available_to_lua<F>);
  lua_setglobal(l, F::name);
}

template <class... Ts, class F>
void invoke_for_each_type(std::type_identity<std::variant<Ts...>>, F &&f) {
  (f.template operator()<Ts>(), ...);
}

int main() {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  invoke_for_each_type(std::type_identity<ScriptFunction>{},
                       [&]<class T>() { push_function_to_lua<T>(L); });

  // Script runs as a coroutine so cpp_wait_tick can yield.
  const char *script = R"(
        print("Calling into C++ from Lua...")

        local sum = Add(3, 4)
        print("Add(3, 4) =", sum)

        Greet("world")

        print("waiting until tick 8...")
        WaitTick(8)
    )";

  lua_State *co = lua_newthread(L);
  if (luaL_loadstring(co, script) != LUA_OK) {
    const char *err = lua_tostring(co, -1);
    fprintf(stderr, "Lua error: %s\n", err ? err : "unknown error");
    lua_close(L);
    return 1;
  }

  int nresults = 0;
  int status = lua_resume(co, nullptr, 0, &nresults);

  // Host loop: each yield returns control here. Advance the tick and
  // resume; the C continuation retries until ticks reaches the target.
  while (status == LUA_YIELD) {
    ticks++;
    printf("host: coroutine yielded, now at tick %d\n", ticks);
    status = lua_resume(co, nullptr, 0, &nresults);
  }

  if (status != LUA_OK) {
    const char *err = lua_tostring(co, -1);
    fprintf(stderr, "Lua error: %s\n", err ? err : "unknown error");
    lua_close(L);
    return 1;
  }

  lua_close(L);
  return 0;
}
