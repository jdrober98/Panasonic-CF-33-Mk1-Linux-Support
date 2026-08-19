obj-m += kernel/

KDIR ?= /lib/modules/$(shell uname -r)/build
SRC := $(shell pwd)
BUILD := $(SRC)/../build

.PHONY: all clean

all:
	@mkdir -p $(BUILD)
	$(MAKE) -C $(KDIR) M=$(SRC) modules
	@find $(SRC) -name "*.ko" ! -path "$(BUILD)/*" -exec mv {} $(BUILD) \;
	@$(MAKE) -C $(KDIR) M=$(SRC) clean

clean:
	rm -rf $(BUILD)