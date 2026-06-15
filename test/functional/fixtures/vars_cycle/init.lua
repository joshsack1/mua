-- A cyclic table presents unbounded apparent depth; the marshaling walk trips
-- its depth cap and raises -- nonfatal, the cap doubling as the cycle guard.
local cyclic = {}
cyclic.self = cyclic
mua.g.cyc = cyclic
print("UNREACHED")
