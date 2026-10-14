CXX ?= c++
PKG_CONFIG ?= pkg-config
BUILD_DIR := build
BIN_DIR := bin
TARGET := $(BIN_DIR)/yt-downloader
GUI_TARGET := $(BUILD_DIR)/yt-downloader-gui
CORE_SOURCES := $(filter-out src/core/downloader.cpp src/core/format_parser.cpp src/core/format_selector.cpp src/core/http_client.cpp src/core/youtube_client.cpp, $(shell find src/core -type f -name '*.cpp' | sort))
CLIENT_SOURCES := $(filter-out src/client/gui.cpp,$(wildcard src/client/*.cpp))
CORE_OBJECTS := $(patsubst src/core/%.cpp,$(BUILD_DIR)/core/%.o,$(CORE_SOURCES))
CLIENT_OBJECTS := $(patsubst src/client/%.cpp,$(BUILD_DIR)/client/%.o,$(CLIENT_SOURCES))
TEST_TARGET := $(BUILD_DIR)/test-utils
SELECTOR_TEST_TARGET := $(BUILD_DIR)/test-format-selector
CPPFLAGS += -Iinclude $(shell $(PKG_CONFIG) --cflags libcurl sqlite3 2>/dev/null)
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl sqlite3 2>/dev/null)
QT_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt5Widgets 2>/dev/null)
QT_LIBS := $(shell $(PKG_CONFIG) --libs Qt5Widgets 2>/dev/null)

.PHONY: all test clean install gui
all: $(TARGET)

gui: $(GUI_TARGET)

$(TARGET): $(CORE_OBJECTS) $(CLIENT_OBJECTS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(GUI_TARGET): src/client/gui.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(QT_CFLAGS) $(CXXFLAGS) $< -o $@ $(QT_LIBS)

$(BUILD_DIR)/core/%.o: src/core/%.cpp | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/client/%.o: src/client/%.cpp | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(BIN_DIR):
	mkdir -p $@

$(TEST_TARGET): tests/test_utils.cpp src/core/utils.cpp src/core/cookie.cpp src/core/client/http.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(SELECTOR_TEST_TARGET): tests/test_format_selector.cpp src/core/format/selector.cpp src/core/utils.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

test: $(TEST_TARGET) $(SELECTOR_TEST_TARGET)
	./$(TEST_TARGET)
	./$(SELECTOR_TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

install: $(TARGET)
	install -Dm755 $(TARGET) "$(DESTDIR)/usr/local/bin/yt-downloader"
