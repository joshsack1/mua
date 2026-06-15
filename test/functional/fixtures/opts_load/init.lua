-- Exercises the mua.o set + read-back round-trip for all three option types.
mua.o.step_cap = 7
mua.o.model = "demo/model"
mua.o.system_prompt = "You are demo"

print("STEPCAP=" .. mua.o.step_cap)
print("MODEL=" .. mua.o.model)
print("SP1=" .. mua.o.system_prompt:match("^%a+"))
