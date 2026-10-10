# `make install` builds a Release Studio against the SDK the engine's
# `make install` put in PREFIX, and installs chibi-dev beside its chibi.
PREFIX ?= $(HOME)/.local
INSTALL_BUILD_DIR ?= build-install
JOBS ?= 8
# Use installed upstream LLVM when available, as the engine's build does.
NATIVE_TOOLCHAIN_FLAGS ?= $(if $(wildcard /opt/homebrew/opt/llvm/bin/clang),-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++)
SDK_CONFIG = $(abspath $(PREFIX))/lib/cmake/Chibi

.PHONY: install
install:
	@test -f "$(SDK_CONFIG)/ChibiConfig.cmake" || { printf '%s\n' 'No Chibi SDK in $(abspath $(PREFIX)): run make install in the engine checkout first' >&2; exit 2; }
	cmake -S . -B "$(INSTALL_BUILD_DIR)" -G Ninja $(NATIVE_TOOLCHAIN_FLAGS) -DCMAKE_BUILD_TYPE=Release -DChibi_DIR="$(SDK_CONFIG)"
	cmake --build "$(INSTALL_BUILD_DIR)" --target chibi-studio --parallel $(JOBS)
	cmake --install "$(INSTALL_BUILD_DIR)" --prefix "$(abspath $(PREFIX))"
