-- Setting an unknown option raises from mua.api; the user-init pcall reports
-- it on stderr and startup continues (nvim-style nonfatal config error).
mua.o.bogus = 1
