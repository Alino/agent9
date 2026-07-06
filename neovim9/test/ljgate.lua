-- G1 gate: LuaJIT interpreter on 9front
local ok = 0
local function check(name, cond)
  if cond then ok = ok + 1; print("PASS " .. name)
  else print("FAIL " .. name) end
end

-- fib
local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end
check("fib", fib(24) == 46368)

-- string.buffer (LuaJIT-only, needs FFI substrate)
local sb = require("string.buffer")
local buf = sb.new()
buf:put("hello "):put("9front")
check("string.buffer", buf:tostring() == "hello 9front")

-- bit ops
check("bit", bit.band(0xFF00, 0x0FF0) == 0x0F00 and bit.bxor(0xF0F0, 0xFFFF) == 0x0F0F)

-- pcall/error/xpcall
local st, err = pcall(function() error("boom", 0) end)
check("pcall", st == false and err == "boom")
local traced = false
xpcall(function() error("x") end, function() traced = true end)
check("xpcall", traced)

-- coroutines
local co = coroutine.create(function(a) local b = coroutine.yield(a + 1) return b * 2 end)
local _, v1 = coroutine.resume(co, 10)
local _, v2 = coroutine.resume(co, 5)
check("coroutine", v1 == 11 and v2 == 10)

-- gc
collectgarbage("collect")
check("gc", collectgarbage("count") > 0)

-- string/table stdlib
check("gsub", ("plan nine"):gsub("nine", "9") == "plan 9")
local t = {} for i = 1, 100 do t[i] = 101 - i end
table.sort(t)
check("sort", t[1] == 1 and t[100] == 100)

-- os/io basics
local f = io.open("/tmp/ljgate.out", "w")
f:write("42\n"); f:close()
local g = io.open("/tmp/ljgate.out", "r")
local n = g:read("*n"); g:close()
check("io+readnum", n == 42)
check("os.time", os.time() > 1700000000)
check("os.date", type(os.date("%Y")) == "string")

print(string.format("ljgate: %d/12 passed on %s", ok, jit and jit.os or "?"))
if ok == 12 then print("LJGATE OK") end
