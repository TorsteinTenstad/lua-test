#include <cassert>
#include <cstdio>
#include <expected>
#include <lua.hpp>
#include <numeric>
#include <optional>
#include <print>
#include <string_view>
#include <vector>

class LuaTable {
public:
  LuaTable() = default;
  LuaTable(lua_State *l, int stackIndex) : l_{l}, stackIndex_{stackIndex} {}
  template <typename T> T get(const char *key) {
    luaL_checktype(l_, stackIndex_, LUA_TTABLE);
    const int type = lua_getfield(l_, stackIndex_, key);
    if (type == LUA_TNIL) {
      luaL_argerror(l_, stackIndex_, "table is missing field");
      assert(false);
      return {};
    }
    T value;
    const auto topOfStack = lua_gettop(l_);
    get_from_lua(l_, topOfStack, value);
    lua_pop(l_, 1);
    return value;
  }
  template <typename T> std::optional<T> get_optional(const char *key) {
    if (!lua_istable(l_, stackIndex_)) {
      return std::nullopt;
    }
    const int type = lua_getfield(l_, stackIndex_, key);
    if (type == LUA_TNIL) {
      return std::nullopt;
    }
    T value;
    const auto topOfStack = lua_gettop(l_);
    get_from_lua(l_, topOfStack, value);
    lua_pop(l_, 1);
    return value;
  }

private:
  lua_State *l_{};
  int stackIndex_{};
};

void get_from_lua(lua_State *l, int index, double &value) {
  value = luaL_checknumber(l, index);
}
void get_from_lua(lua_State *l, int index, int &value) {
  value = luaL_checkinteger(l, index);
}
void get_from_lua(lua_State *l, int index, bool &value) {
  luaL_checktype(l, index, LUA_TBOOLEAN);
  value = lua_toboolean(l, index);
}
void get_from_lua(lua_State *l, int index, std::string_view &value) {
  const char *c = luaL_checkstring(l, index);
  value = std::string_view(c);
}

void get_from_lua(lua_State *l, int index, LuaTable &value) {
  value = LuaTable(l, index);
}

template <typename T>
void get_from_lua(lua_State *l, int index, std::vector<T> &value) {
  luaL_checktype(l, index, LUA_TTABLE);
  const lua_Unsigned size = lua_rawlen(l, index);
  value = std::vector<T>(size);
  for (lua_Unsigned tableIndex = 0; tableIndex < size; ++tableIndex) {
    lua_geti(l, index, tableIndex + 1);
    const auto topOfStack = lua_gettop(l);
    get_from_lua(l, topOfStack, value[tableIndex]);
    lua_pop(l, 1);
  }
}

template <typename T>
void get_from_lua(lua_State *l, int index, std::optional<T> &value) {
  if (lua_isnoneornil(l, index)) {
    value = std::nullopt;
    return;
  }
  T t;
  get_from_lua(l, index, t);
  value = t;
}

int push_to_lua(lua_State *l, double value) {
  lua_pushnumber(l, value);
  return 1;
}

template <class... Ts>
void get_tuple_from_lua(lua_State *l, std::tuple<Ts...> &t) {

  int index = 1;
  std::apply([&](auto &...out) { (get_from_lua(l, index++, out), ...); }, t);
}

enum class ExecutionStatus {};
using ExecuteResult = std::expected<int, ExecutionStatus>;
int ticks = 0;

template <typename F> struct function_args_tuple;
template <typename F, typename... Args>
struct function_args_tuple<F (*)(Args...)> {
  using type = std::tuple<Args...>;
};
template <typename F>
using function_args_tuple_t = typename function_args_tuple<F>::type;

template <auto F> int lua_f(lua_State *l) {
  {
    function_args_tuple_t<decltype(F)> args{};
    get_tuple_from_lua(l, args);
    const auto result = std::apply(F, std::move(args));
    if (result.has_value()) {
      if constexpr (std::is_void_v<decltype(result.value())>) {
        return 0;
      } else {
        return push_to_lua(l, result.value());
      }
    }
  }
  lua_yieldk(l, 0, 0, [](lua_State *l, auto, auto) { return lua_f<F>(l); });
  assert(false);
  return 0;
}

template <auto F>
static void push_function_to_lua(const char *name, lua_State *l) {
  lua_pushcfunction(l, lua_f<F>);
  lua_setglobal(l, name);
}

namespace lua_api {

std::expected<double, ExecutionStatus> add(double a, double b,
                                           std::optional<double> c) {
  return a + b + c.value_or(0);
}

std::expected<double, ExecutionStatus> sum(std::vector<double> range) {
  return std::accumulate(range.begin(), range.end(), 0);
}

std::expected<void, ExecutionStatus> greet(std::string_view name,
                                           LuaTable table) {
  const auto surname =
      table.get_optional<std::string_view>("surname").value_or("");
  std::println("Greetings, {} {}!", name, surname);
  return {};
}

std::expected<void, ExecutionStatus> waitTick(int tick) {
  if (ticks < tick) {
    return std::unexpected<ExecutionStatus>({});
  }
  return {};
}

} // namespace lua_api

int main() {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  push_function_to_lua<lua_api::add>("Add", L);
  push_function_to_lua<lua_api::sum>("Sum", L);
  push_function_to_lua<lua_api::greet>("Greet", L);
  push_function_to_lua<lua_api::waitTick>("WaitTick", L);

  const char *script = R"(
        print("Calling into C++ from Lua...")

        local sum = Add(3, 4)
        print("Add(3, 4) =", sum)

        local range_sum = Sum({1, 2, 3, 4})
        print("Sum({1, 2, 3, 4}) =", range_sum)
        
        Greet("world", {surname = "of joi"})
        Greet("world", {})
        
        print("waiting until tick 8...")
        WaitTick(8)

        local sum2 = Add(65, 45, -10)
        print("Add(65, 45, -10) =", sum2)
    )";

  lua_State *co = lua_newthread(L);
  if (luaL_loadstring(co, script) != LUA_OK) {
    const char *err = lua_tostring(co, -1);
    fprintf(stderr, "Lua error: %s\n", err ? err : "unknown error");
    lua_close(L);
    return 1;
  }

  int nresults = 0;
  int status = LUA_YIELD;
  while (status == LUA_YIELD) {
    ticks++;
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
