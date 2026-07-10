BIN_DIR ?= bin
TARGET ?= $(BIN_DIR)/lunaar-switch
PROFILE ?= release
RUST_TARGET := target/$(PROFILE)/lunaar-switch

$(TARGET): | $(BIN_DIR)
	cargo build --profile $(PROFILE)
	cp $(RUST_TARGET) $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR)
	cargo clean

.PHONY: clean