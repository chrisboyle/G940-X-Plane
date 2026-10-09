# GNU make; cross builds can set PLATFORM and CXX explicitly.
.DEFAULT_GOAL := all
BUILDDIR ?= build
BUILD_TYPE ?= release
SDK_DIR ?= SDK
HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
PLATFORM ?= mac
else ifeq ($(HOST_OS),Linux)
PLATFORM ?= linux
else
PLATFORM ?= windows
endif

PLUGINS := g940FF g940LEDs
SDK_ARCHIVE := XPSDK430.zip
SDK_URL := https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/$(SDK_ARCHIVE)
SDK_HEADER := $(SDK_DIR)/CHeaders/XPLM/XPLMDefs.h
SDK_CPPFLAGS := -I"$(SDK_DIR)/CHeaders/XPLM" -DXPLM200=1 -DXPLM210=1 -DXPLM300=1 -DXPLM301=1
ifeq ($(BUILD_TYPE),release)
CXXFLAGS ?= -O2
BUILD_CXXFLAGS := -g0 -DNDEBUG -UG940_DEBUG_FORCE
else ifeq ($(BUILD_TYPE),debug)
CXXFLAGS ?= -O0
BUILD_CXXFLAGS := -g -UNDEBUG
else
$(error Unsupported BUILD_TYPE '$(BUILD_TYPE)'; choose release or debug)
endif
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -fvisibility=hidden
empty :=
space := $(empty) $(empty)

ifeq ($(PLATFORM),mac)
ifeq ($(origin CXX),default)
CXX := /usr/bin/clang++
endif
MAC_ARCHS ?= x86_64 arm64
MAC_MIN_VERSION ?= 11.0
ARCH_FLAGS := $(foreach arch,$(MAC_ARCHS),-arch $(arch)) -mmacosx-version-min=$(MAC_MIN_VERSION)
ARCH_TAG := $(subst $(space),_,$(strip $(MAC_ARCHS)))
PLATFORM_CPPFLAGS := -DAPL=1 -DIBM=0 -DLIN=0
PLUGIN_FILE := mac.xpl
PLATFORM_LDFLAGS := -bundle -F"$(SDK_DIR)/Libraries/Mac"
STRIP_DEBUG := -Wl,-S
HID_LIBS := -framework IOKit -framework CoreFoundation
PLATFORM_LIBS := -framework XPLM $(HID_LIBS)
else ifeq ($(PLATFORM),linux)
ARCH_FLAGS := -m64 -fPIC
ARCH_TAG := x86_64
PLATFORM_CPPFLAGS := -DAPL=0 -DIBM=0 -DLIN=1
PLUGIN_FILE := lin.xpl
PLATFORM_LDFLAGS := -shared -Wl,--version-script=exports.txt
STRIP_DEBUG := -Wl,--strip-debug
PLATFORM_LIBS := -lm
else ifeq ($(PLATFORM),windows)
ARCH_FLAGS := -m64
ARCH_TAG := x86_64
PLATFORM_CPPFLAGS := -DAPL=0 -DIBM=1 -DLIN=0
PLUGIN_FILE := win.xpl
PLATFORM_LDFLAGS := -shared -static-libgcc -static-libstdc++
STRIP_DEBUG := -Wl,--strip-debug
HID_LIBS := -lhid -lsetupapi
# MinGW's POSIX thread runtime may otherwise add libwinpthread-1.dll to the
# plugin's dependencies. Zig uses Windows threads and has no such archive.
WINDOWS_PTHREAD := $(shell $(CXX) -print-file-name=libwinpthread.a 2>/dev/null)
ifneq ($(wildcard $(WINDOWS_PTHREAD)),)
HID_LIBS += -Wl,--whole-archive "$(WINDOWS_PTHREAD)" -Wl,--no-whole-archive
endif
PLATFORM_LIBS := "$(SDK_DIR)/Libraries/Win/XPLM_64.lib" $(HID_LIBS)
else
$(error Unsupported PLATFORM '$(PLATFORM)'; choose linux, windows, or mac)
endif

ifeq ($(BUILD_TYPE),release)
BUILD_LDFLAGS := $(STRIP_DEBUG)
endif

# Keep OS, architecture and release/debug objects separate.
OBJDIR := $(BUILDDIR)/obj/$(PLATFORM)/$(ARCH_TAG)/$(BUILD_TYPE)
COMMON_OBJECTS := $(OBJDIR)/g940Backend.o $(OBJDIR)/g940HID.o
OBJECTS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(PLUGINS))) $(COMMON_OBJECTS)
TARGETS := $(foreach plugin,$(PLUGINS),$(BUILDDIR)/$(plugin)/64/$(PLUGIN_FILE))
LINK_TARGETS := $(foreach plugin,$(PLUGINS),$(OBJDIR)/$(plugin)/$(PLUGIN_FILE))

.PHONY: all clean install sdk test probe FORCE
.SECONDARY: $(OBJECTS) $(LINK_TARGETS)
all: $(TARGETS)

# This official header is absent from the old handwritten SDK replacements.
sdk: $(SDK_HEADER)
$(SDK_ARCHIVE):
	curl --fail --location --retry 2 --output "$@.tmp" "$(SDK_URL)"
	unzip -tq "$@.tmp"
	mv "$@.tmp" "$@"

$(SDK_HEADER):
	$(MAKE) $(SDK_ARCHIVE)
	unzip -q -o "$(SDK_ARCHIVE)"
	@test -f "$@" || { echo "Set SDK_DIR to an extracted official SDK directory."; exit 1; }

$(OBJDIR)/%.o: %.cpp $(SDK_HEADER) Makefile
	mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(SDK_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CXXFLAGS) $(BUILD_CXXFLAGS) $(ARCH_FLAGS) -MMD -MP -c "$<" -o "$@"

$(OBJDIR)/%/$(PLUGIN_FILE): $(OBJDIR)/%.o $(COMMON_OBJECTS) Makefile exports.txt
	mkdir -p "$(dir $@)"
	$(CXX) $(ARCH_FLAGS) $(LDFLAGS) $(PLATFORM_LDFLAGS) $(BUILD_LDFLAGS) -o "$@" $(filter %.o,$^) $(PLATFORM_LIBS) $(LDLIBS)

# Copy from the selected architecture's linked artifact, even when switching
# back to previously built architectures whose objects are older than $@.
$(BUILDDIR)/%/64/$(PLUGIN_FILE): $(OBJDIR)/%/$(PLUGIN_FILE) FORCE
	mkdir -p "$(dir $@)"
	cmp -s "$<" "$@" || cp "$<" "$@"

FORCE:

# Prefer X-Plane's native hints; an explicit path works on every platform.
install: all
	python3 tools/install.py --build-dir "$(BUILDDIR)" --platform "$(PLATFORM)" $(if $(XP_INSTALL_PATH),--x-plane "$(XP_INSTALL_PATH)") $(if $(HINTFILE),--hint-file "$(HINTFILE)")

# Tests use assertions as their checks, independently of the plugin build mode.
TEST_CXXFLAGS := $(CXXFLAGS) $(BUILD_CXXFLAGS) -UNDEBUG
test: $(SDK_HEADER)
	mkdir -p "$(BUILDDIR)/tests"
	$(CXX) $(CPPFLAGS) $(SDK_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(TEST_CXXFLAGS) $(ARCH_FLAGS) tests/protocol.cpp -I. -o "$(BUILDDIR)/tests/protocol"
	"$(BUILDDIR)/tests/protocol"
	$(CXX) $(CPPFLAGS) $(SDK_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(TEST_CXXFLAGS) $(ARCH_FLAGS) tests/plugin_host.cpp -I. -o "$(BUILDDIR)/tests/force-feedback"
	"$(BUILDDIR)/tests/force-feedback"
	$(CXX) $(CPPFLAGS) $(SDK_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(TEST_CXXFLAGS) $(ARCH_FLAGS) -DTEST_LEDS tests/plugin_host.cpp -I. -o "$(BUILDDIR)/tests/leds"
	"$(BUILDDIR)/tests/leds"
	$(CXX) $(CPPFLAGS) $(SDK_CPPFLAGS) $(PLATFORM_CPPFLAGS) -ULIN -DLIN=0 $(TEST_CXXFLAGS) $(ARCH_FLAGS) tests/hid_backend.cpp g940Backend.cpp -I. -o "$(BUILDDIR)/tests/hid-backend"
	"$(BUILDDIR)/tests/hid-backend"
	python3 -m unittest discover -s tests -p 'test_*.py'

probe: $(SDK_HEADER)
	mkdir -p "$(BUILDDIR)/tools"
	$(CXX) $(CPPFLAGS) $(SDK_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CXXFLAGS) $(BUILD_CXXFLAGS) $(ARCH_FLAGS) tools/g940_probe.cpp g940HID.cpp -I. $(HID_LIBS) -o "$(BUILDDIR)/tools/g940_probe"
	"$(BUILDDIR)/tools/g940_probe"

clean:
	rm -rf "$(BUILDDIR)/obj" "$(BUILDDIR)/tests" "$(BUILDDIR)/tools" $(foreach plugin,$(PLUGINS),"$(BUILDDIR)/$(plugin)")

-include $(OBJECTS:.o=.d)
