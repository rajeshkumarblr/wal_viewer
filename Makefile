BUILD_DIR = build
EXEC = wal_viewer_gui

.PHONY: all build clean run stop

all: build

$(BUILD_DIR)/Makefile: CMakeLists.txt
	cmake -S . -B $(BUILD_DIR)

build: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

run: build
	./$(BUILD_DIR)/$(EXEC)

stop:
	pkill -f $(EXEC) || true
