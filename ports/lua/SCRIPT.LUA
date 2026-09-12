print("PHIPIA LUA INPUT READY")
local input = assert(io.read("*l"), "stdin ended before the test input")
assert(input == "phipia", "unexpected stdin value")

local sum = 0
for value = 1, 100 do
  sum = sum + value
end
assert(sum == 5050)

local output = assert(io.open("RESULT.TXT", "w"))
local sine = math.sin(0.5)
assert(sine > 0.4794 and sine < 0.4795)
assert(output:write(string.format("input=%s\nsum=%d\nmath=ok\n",
  input, sum)))
assert(output:close())

print("PHIPIA LUA PASS input=" .. input .. " sum=" .. sum)
