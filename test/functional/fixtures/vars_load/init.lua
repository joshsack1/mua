-- Round-trips scalars and nested tables through mua.g and the value-marshaling
-- bridge: each read rebuilds a fresh Lua value from the stored Object.
mua.g.count = 7
mua.g.name = "demo"
mua.g.nested = { a = { 1, 2, 3 }, flag = true }

mua.g.temp = 99
mua.g.temp = nil -- delete the key

print("COUNT=" .. mua.g.count)
print("NAME=" .. mua.g.name)
print("A2=" .. mua.g.nested.a[2])
print("LEN=" .. #mua.g.nested.a)
print("FLAG=" .. tostring(mua.g.nested.flag))
print("TEMP=" .. tostring(mua.g.temp))
print("UNSET=" .. tostring(mua.g.missing))
