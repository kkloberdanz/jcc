-- The Lua benchmark the plan's performance numbers refer to: recursive
-- calls, table fill and sum, string splitting and sorting. Each part is
-- sized so the whole run takes a few seconds on a kcc-built Lua; the
-- point is the ratio against the same Lua built by cc -O0 and -O2.
-- Run with tests/bench-lua.sh, which builds Lua and times this.
local clock = os.clock

local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end

local total = 0

local function bench(name, f)
    local t0 = clock()
    local r = f()
    local t = clock() - t0
    total = total + t
    io.write(string.format("%-8s %6.2f s  (%s)\n", name, t, tostring(r)))
end

bench("fib", function() return fib(35) end)

bench("table", function()
    local t = {}
    for i = 1, 10000000 do t[i] = i * 2 end
    local s = 0
    for i = 1, #t do s = s + t[i] end
    return s
end)

bench("split", function()
    local parts = {}
    for i = 1, 20000 do parts[i] = "word" .. i end
    local line = table.concat(parts, ",")
    local n = 0
    for _ = 1, 100 do
        for w in string.gmatch(line, "[^,]+") do n = n + #w end
    end
    return n
end)

bench("sort", function()
    local t = {}
    local x = 12345
    for i = 1, 2000000 do
        x = (x * 1103515245 + 12345) % 2147483648
        t[i] = x
    end
    table.sort(t)
    return t[1] + t[#t]
end)

io.write(string.format("total    %6.2f s\n", total))
