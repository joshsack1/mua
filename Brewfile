# Build toolchain and QA tools for mua development (macOS).
# Homebrew may require an administrator shell on some machines.
brew "luajit"
brew "libuv"
brew "cmake"
brew "ninja"
brew "clang-format"
brew "llvm" # keg-only; clang-tidy lives at /opt/homebrew/opt/llvm/bin/clang-tidy
brew "stylua"
brew "luacheck"
brew "luarocks"
# libcurl: mua links the macOS system libcurl via Homebrew's SDK pkg-config shim.
# Do NOT `brew install curl` — the keg-only formula would shadow it confusingly.
