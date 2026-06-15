-- A table mixing array and map keys cannot be marshaled to an Object; the
-- raised error is nonfatal (lands on stderr, startup continues).
mua.g.bad = { 1, 2, name = "x" }
print("UNREACHED")
