# Hash YouTube Downloader
#
# Linux:                 make linux
# Windows (static):      make windows
# Both platforms:        make all-platforms
# Package Windows EXEs:  make package-windows
# Run tests:             make test
# Remove build outputs:  make clean
#
# All generated files stay under:
#   build/linux/
#   build/windows/
#
# Windows requires MXE's x86_64-w64-mingw32.static target,
# including static Qt 5, curl, and SQLite libraries.

.DEFAULT_GOAL := all

# ==================================================
# Configuration
# ==================================================

PLATFORM ?= linux

MXE_ROOT   ?= $(HOME)/Code/mxe
MXE_TARGET ?= x86_64-w64-mingw32.static

MXE_PREFIX  := $(MXE_ROOT)/usr/$(MXE_TARGET)
MXE_BINDIR  := $(MXE_ROOT)/usr/bin
MXE_CXX     := $(MXE_BINDIR)/$(MXE_TARGET)-g++
MXE_PKGCONF := $(MXE_BINDIR)/$(MXE_TARGET)-pkg-config
MXE_OBJDUMP := $(MXE_BINDIR)/$(MXE_TARGET)-objdump

LINUX_DIR   := build/linux
WINDOWS_DIR := build/windows

WIN_BIN_DIR  := $(WINDOWS_DIR)/bin
WIN_DIST_DIR := $(WINDOWS_DIR)/package

WIN_QT_LIB_DIR     := $(MXE_PREFIX)/qt5/lib
WIN_QT_PLUGIN_DIR  := $(MXE_PREFIX)/qt5/plugins/platforms

WIN_QT_PLUGIN := $(firstword \
    $(wildcard $(WIN_QT_PLUGIN_DIR)/libqwindows.a) \
    $(wildcard $(WIN_QT_PLUGIN_DIR)/qwindows.a))

# Qt 5 static platform-plugin support libraries.
WIN_QT_SUPPORT_LIBS := \
    $(WIN_QT_LIB_DIR)/libQt5EventDispatcherSupport.a \
    $(WIN_QT_LIB_DIR)/libQt5FontDatabaseSupport.a \
    $(WIN_QT_LIB_DIR)/libQt5ThemeSupport.a \
    $(WIN_QT_LIB_DIR)/libQt5AccessibilitySupport.a \
    $(WIN_QT_LIB_DIR)/libQt5WindowsUIAutomationSupport.a \
    $(WIN_QT_LIB_DIR)/libQt5PlatformCompositorSupport.a

# Native Windows libraries used by the Qt platform plugin.
WIN_QT_SYSTEM_LIBS := \
    -ldwmapi \
    -limm32 \
    -lwtsapi32 \
    -luxtheme \
    -lwinspool \
    -lshlwapi \
    -lnetapi32

# ==================================================
# Platform configuration
# ==================================================

ifeq ($(filter $(PLATFORM),linux windows),)
    $(error Unsupported PLATFORM '$(PLATFORM)'. Use linux or windows.)
endif

ifeq ($(PLATFORM),windows)

    BUILD_DIR := $(WINDOWS_DIR)
    CXX := $(MXE_CXX)
    PKG_CONFIG := $(MXE_PKGCONF)
    EXEEXT := .exe

    THREAD_FLAGS :=
    GUI_EXTRA_FLAGS := -mwindows

    # Static libraries and static C/C++ runtime.
    CORE_PKG_LIBS := $(shell $(PKG_CONFIG) --static --libs libcurl sqlite3 2>/dev/null)

    # Re-scan static archives to resolve references between Qt libraries.
    QT_LIBS := \
        -Wl,--start-group \
        $(WIN_QT_PLUGIN) \
        $(WIN_QT_SUPPORT_LIBS) \
        $(WIN_QT_SYSTEM_LIBS) \
        $(shell $(PKG_CONFIG) --static --libs Qt5Widgets 2>/dev/null) \
        -Wl,--end-group

    STATIC_LINK_FLAGS := -static -static-libgcc -static-libstdc++
    QT_STATIC_CPPFLAGS := -DQT_STATICPLUGIN

else

    BUILD_DIR := $(LINUX_DIR)
    CXX := g++
    PKG_CONFIG := pkg-config
    EXEEXT :=

    THREAD_FLAGS := -pthread
    GUI_EXTRA_FLAGS := -fPIC

    CORE_PKG_LIBS := $(shell $(PKG_CONFIG) --libs libcurl sqlite3 2>/dev/null)
    QT_LIBS := $(shell $(PKG_CONFIG) --libs Qt5Widgets 2>/dev/null)

    STATIC_LINK_FLAGS :=
    QT_STATIC_CPPFLAGS :=

endif

BIN_DIR  := $(BUILD_DIR)/bin
OBJ_DIR  := $(BUILD_DIR)/obj
TEST_DIR := $(BUILD_DIR)/tests

TARGET := $(BIN_DIR)/yt-downloader$(EXEEXT)
GUI_TARGET := $(BIN_DIR)/yt-downloader-gui$(EXEEXT)

TEST_TARGET := $(TEST_DIR)/test-utils$(EXEEXT)
SELECTOR_TEST_TARGET := $(TEST_DIR)/test-format-selector$(EXEEXT)

# ==================================================
# Sources and compiler flags
# ==================================================

CORE_SOURCES := $(shell find src/core -type f -name '*.cpp' | sort)

CLIENT_SOURCES := $(filter-out src/client/gui.cpp,$(wildcard src/client/*.cpp))

CORE_OBJECTS := $(patsubst src/core/%.cpp,$(OBJ_DIR)/core/%.o,$(CORE_SOURCES))
CLIENT_OBJECTS := $(patsubst src/client/%.cpp,$(OBJ_DIR)/client/%.o,$(CLIENT_SOURCES))

DEPFILES := $(CORE_OBJECTS:.o=.d) $(CLIENT_OBJECTS:.o=.d)

CORE_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl sqlite3 2>/dev/null)
QT_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt5Widgets 2>/dev/null)

CPPFLAGS += -Iinclude $(CORE_PKG_CFLAGS)
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic

LDLIBS += $(CORE_PKG_LIBS) $(THREAD_FLAGS)

# ==================================================
# Public targets
# ==================================================

.PHONY: all cli gui linux windows all-platforms package-windows \
        check-core-deps check-qt-deps test clean clean-all install

all: check-core-deps check-qt-deps $(TARGET) $(GUI_TARGET)

cli: check-core-deps $(TARGET)

gui: check-core-deps check-qt-deps $(TARGET) $(GUI_TARGET)

linux:
	$(MAKE) --no-print-directory PLATFORM=linux all

windows:
	@test -x "$(MXE_CXX)" || { \
		echo "ERROR: MXE static compiler not found: $(MXE_CXX)" >&2; \
		exit 1; \
	}
	@test -x "$(MXE_PKGCONF)" || { \
		echo "ERROR: MXE pkg-config not found: $(MXE_PKGCONF)" >&2; \
		exit 1; \
	}
	PATH="$(MXE_BINDIR):$$PATH" \
		$(MAKE) --no-print-directory PLATFORM=windows all

all-platforms:
	$(MAKE) --no-print-directory PLATFORM=linux all
	$(MAKE) --no-print-directory windows

# ==================================================
# Dependency checks
# ==================================================

check-core-deps:
	@$(PKG_CONFIG) --exists libcurl sqlite3 || { \
		echo "ERROR: libcurl or SQLite development files are missing for $(PLATFORM)." >&2; \
		exit 1; \
	}

ifeq ($(PLATFORM),windows)

check-qt-deps:
	@$(PKG_CONFIG) --exists Qt5Widgets || { \
		echo "ERROR: Qt5Widgets was not found for $(MXE_TARGET)." >&2; \
		exit 1; \
	}
	@test -n "$(WIN_QT_PLUGIN)" || { \
		echo "ERROR: Static Qt Windows platform plugin was not found." >&2; \
		echo "Expected libqwindows.a under $(WIN_QT_PLUGIN_DIR)" >&2; \
		exit 1; \
	}
	@set -eu; \
	for LIB in $(WIN_QT_SUPPORT_LIBS); do \
		if [ ! -f "$$LIB" ]; then \
			echo "ERROR: Required Qt static support library is missing:" >&2; \
			echo "  $$LIB" >&2; \
			exit 1; \
		fi; \
	done

else

check-qt-deps:
	@$(PKG_CONFIG) --exists Qt5Widgets || { \
		echo "ERROR: Qt5Widgets was not found for Linux." >&2; \
		exit 1; \
	}

endif

# ==================================================
# Compile object files
# ==================================================

$(OBJ_DIR)/core/%.o: src/core/%.cpp | check-core-deps
	mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) \
		-MMD -MP -c "$<" -o "$@"

$(OBJ_DIR)/client/%.o: src/client/%.cpp | check-core-deps
	mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) \
		-MMD -MP -c "$<" -o "$@"

# ==================================================
# Link CLI and GUI
# ==================================================

$(TARGET): $(CORE_OBJECTS) $(CLIENT_OBJECTS) | check-core-deps
	mkdir -p "$(dir $@)"
	$(CXX) $(LDFLAGS) $(STATIC_LINK_FLAGS) \
		-o "$@" $^ $(LDLIBS)

$(GUI_TARGET): src/client/gui.cpp | check-qt-deps
	mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(QT_CFLAGS) $(CXXFLAGS) \
		$(QT_STATIC_CPPFLAGS) $(GUI_EXTRA_FLAGS) $(LDFLAGS) \
		$(STATIC_LINK_FLAGS) -o "$@" "$<" $(QT_LIBS)

# ==================================================
# Tests
# ==================================================

$(TEST_TARGET): tests/test_utils.cpp \
                src/core/utils.cpp \
                src/core/cookie.cpp \
                src/core/client/http.cpp | check-core-deps
	mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) \
		$(STATIC_LINK_FLAGS) -o "$@" $^ $(LDLIBS)

$(SELECTOR_TEST_TARGET): tests/test_format_selector.cpp \
                         src/core/format/selector.cpp \
                         src/core/utils.cpp | check-core-deps
	mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) \
		$(STATIC_LINK_FLAGS) -o "$@" $^ $(LDLIBS)

ifeq ($(PLATFORM),windows)

test: $(TEST_TARGET) $(SELECTOR_TEST_TARGET)
	@echo "Windows test executables built."
	@echo "Run them on Windows to execute the tests."

else

test: $(TEST_TARGET) $(SELECTOR_TEST_TARGET)
	./$(TEST_TARGET)
	./$(SELECTOR_TEST_TARGET)

endif

# ==================================================
# Windows distribution
# ==================================================
#
# The package contains only the two executables.
# Windows system DLLs remain normal Windows dependencies.

package-windows: windows
	@set -eu; \
	DIST="$(WIN_DIST_DIR)"; \
	rm -rf "$$DIST"; \
	mkdir -p "$$DIST"; \
	cp -f "$(WIN_BIN_DIR)/yt-downloader.exe" "$$DIST/yt-downloader.exe"; \
	cp -f "$(WIN_BIN_DIR)/yt-downloader-gui.exe" "$$DIST/yt-downloader-gui.exe"; \
	echo; \
	echo "Windows executables packaged in $$DIST"; \
	echo "Files:"; \
	find "$$DIST" -maxdepth 1 -type f -print | sort; \
	echo; \
	echo "Imported DLLs (Windows system DLLs are expected):"; \
	for EXE in "$$DIST/yt-downloader.exe" "$$DIST/yt-downloader-gui.exe"; do \
		echo "  $$(basename "$$EXE"):"; \
		"$(MXE_OBJDUMP)" -p "$$EXE" | \
			sed -n 's/^[[:space:]]*DLL Name: /    /p'; \
	done

# ==================================================
# Install Linux CLI
# ==================================================

ifeq ($(PLATFORM),linux)

install: $(TARGET)
	install -Dm755 "$(TARGET)" "$(DESTDIR)/usr/local/bin/yt-downloader"

else

install:
	@echo "Use the Linux build for the install target." >&2
	@exit 1

endif

# ==================================================
# Clean
# ==================================================

clean:
	rm -rf build dist bin

clean-all: clean

# Generated header dependencies.
-include $(DEPFILES)