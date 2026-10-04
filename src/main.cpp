#pragma once
#include <cassert>
#include <expected>
#include <lua.hpp>
#include <memory>
#include <numeric>
#include <optional>
#include <print>
#include <string_view>
#include <type_traits>
#include <vector>

class LuaTable
{
public:
    LuaTable() = default;
    LuaTable(lua_State *l, int stackIndex) : l_{l}, stackIndex_{stackIndex} {}

    template <typename T>
    T get(const char *key)
    {
        luaL_checktype(l_, stackIndex_, LUA_TTABLE);
        const int type = lua_getfield(l_, stackIndex_, key);
        if (type == LUA_TNIL)
        {
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

    template <typename T>
    std::optional<T> get_optional(const char *key)
    {
        if (!lua_istable(l_, stackIndex_))
        {
            return std::nullopt;
        }
        const int type = lua_getfield(l_, stackIndex_, key);
        if (type == LUA_TNIL)
        {
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

void get_from_lua(lua_State *l, int index, double &value)
{
    value = luaL_checknumber(l, index);
}
void get_from_lua(lua_State *l, int index, int &value)
{
    value = luaL_checkinteger(l, index);
}
void get_from_lua(lua_State *l, int index, bool &value)
{
    luaL_checktype(l, index, LUA_TBOOLEAN);
    value = lua_toboolean(l, index);
}
void get_from_lua(lua_State *l, int index, std::string_view &value)
{
    const char *c = luaL_checkstring(l, index);
    value = std::string_view(c);
}
void get_from_lua(lua_State *l, int index, LuaTable &value)
{
    value = LuaTable(l, index);
}
template <typename T>
void get_from_lua(lua_State *l, int index, std::vector<T> &value)
{
    luaL_checktype(l, index, LUA_TTABLE);
    const lua_Unsigned size = lua_rawlen(l, index);
    value = std::vector<T>(size);
    for (lua_Unsigned tableIndex = 0; tableIndex < size; ++tableIndex)
    {
        lua_geti(l, index, tableIndex + 1);
        const auto topOfStack = lua_gettop(l);
        get_from_lua(l, topOfStack, value[tableIndex]);
        lua_pop(l, 1);
    }
}
template <typename T>
void get_from_lua(lua_State *l, int index, std::optional<T> &value)
{
    if (lua_isnoneornil(l, index))
    {
        value = std::nullopt;
        return;
    }
    T t;
    get_from_lua(l, index, t);
    value = t;
}

template <class... Ts>
void get_tuple_from_lua(lua_State *l, std::tuple<Ts...> &t)
{
    int index = 1;
    std::apply([&](auto &...out) { (get_from_lua(l, index++, out), ...); }, t);
}

int push_to_lua(lua_State *l, double value)
{
    lua_pushnumber(l, value);
    return 1;
}

template <typename F>
struct function_args_tuple;
template <typename F, typename... Args>
struct function_args_tuple<F (*)(Args...)>
{
    using type = std::tuple<Args...>;
};
template <typename F>
using function_args_tuple_t = typename function_args_tuple<F>::type;

template <typename YieldData>
YieldData &yieldDataGlobal()
{
    static YieldData data{};
    return data;
}

template <auto F>
int call_with_args_from_lua_stack(lua_State *l)
{
    {
        function_args_tuple_t<decltype(F)> args{};
        get_tuple_from_lua(l, args);
        const auto result = std::apply(F, std::move(args));
        if (result.has_value())
        {
            if constexpr (std::is_void_v<decltype(result.value())>)
            {
                return 0;
            }
            else
            {
                return push_to_lua(l, result.value());
            }
        }
        const auto yieldData = result.error();
        yieldDataGlobal<std::remove_cvref_t<decltype(yieldData)>>() = yieldData;
    }
    lua_yieldk(l, 0, 0, [](lua_State *l, auto, auto) {
        return call_with_args_from_lua_stack<F>(l);
    });
    assert(false);
    return 0;
}

template <auto F>
static void push_function_to_lua(const char *name, lua_State *l)
{
    lua_pushcfunction(l, call_with_args_from_lua_stack<F>);
    lua_setglobal(l, name);
}

enum class RunStatus
{
    // The loaded script was completed successfully
    Completed,

    // A C++ function called from Lua returned `YieldData`,
    // causing the engine to yield, letting the caller decide how to proceed
    // `RunResult::yield` contains the returned `YieldData`.
    Yield,

    // The execution of the script errored.
    // Typically cases include type error, syntax error, or missing/incorrect arguments to
    // functions within Lua.
    // `RunResult::error` contains the error string.
    Error,
};

template <typename YieldData>
class LuaEngine
{
public:
    LuaEngine() : l_{luaL_newstate(), &lua_close}
    {
        luaL_openlibs(l_.get());
        thread_ = lua_newthread(l_.get());
    }

    template <auto F>
    void push_function(const char *name)
    {
        push_function_to_lua<F>(name, l_.get());
    }

    bool load_script(const char *script) { return luaL_loadstring(thread_, script) == LUA_OK; }

    struct RunResult
    {
        RunStatus status{};
        YieldData yield{};
        std::string error{};
    };
    RunResult run()
    {
        int nresults = 0;
        const int status = lua_resume(thread_, nullptr, 0, &nresults);
        switch (status)
        {
        case LUA_OK:
            return {.status = RunStatus::Completed};
        case LUA_YIELD:
            return {.status = RunStatus::Yield, .yield = yieldDataGlobal<YieldData>()};
        default:
            break;
        }
        return {.status = RunStatus::Error, .error = lua_tostring(thread_, -1)};
    }

private:
    std::unique_ptr<lua_State, decltype(&lua_close)> l_;
    lua_State *thread_;
};

int ticks = 0;
struct ExecutionStatus
{
    int ticksLeft{};
};

namespace lua_api
{
std::expected<double, ExecutionStatus> add(double a, double b, std::optional<double> c)
{
    return a + b + c.value_or(0);
}

std::expected<double, ExecutionStatus> sum(std::vector<double> range)
{
    return std::accumulate(range.begin(), range.end(), 0);
}

std::expected<void, ExecutionStatus> greet(std::string_view name, LuaTable table)
{
    const auto surname = table.get_optional<std::string_view>("surname").value_or("");
    std::println("Greetings, {} {}!", name, surname);
    return {};
}

std::expected<void, ExecutionStatus> waitTick(int tick)
{
    if (ticks < tick)
    {
        return std::unexpected<ExecutionStatus>({tick - ticks});
    }
    return {};
}
} // namespace lua_api

int main()
{
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

    LuaEngine<ExecutionStatus> engine{};
    engine.push_function<lua_api::add>("Add");
    engine.push_function<lua_api::sum>("Sum");
    engine.push_function<lua_api::greet>("Greet");
    engine.push_function<lua_api::waitTick>("WaitTick");
    engine.load_script(script);

    for (;;)
    {
        const auto runResult{engine.run()};
        switch (runResult.status)
        {
        case RunStatus::Completed:
            return 0;
        case RunStatus::Error:
            std::println("{}", runResult.error);
            return 1;
        case RunStatus::Yield:
            std::println("Execution yielded ticks left: {}", runResult.yield.ticksLeft);
            ++ticks;
            continue;
        }
    }
    return 0;
}
