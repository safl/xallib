BUILDDIR = build
BUILD_TYPE ?= release
DEBUG_MESSAGES ?= false

.PHONY: all
all: clean configure build install
	@echo ""
	@echo "To run tests; Have a look at the Makefile helpers"
	@echo "================================================="

.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

.PHONY: configure
configure:
	meson setup $(BUILDDIR) --buildtype=$(BUILD_TYPE) -Ddebug_messages=$(DEBUG_MESSAGES)

.PHONY: build
build:
	meson compile -C $(BUILDDIR)
	
.PHONY: install
install:
	meson install -C $(BUILDDIR)

.PHONY: test-using-nvme
test-using-nvme:
	cd cijoe && cijoe workflows/prep_and_test.yaml \
		--config configs/localhost-nvme.toml \
		--monitor

.PHONY: test-using-zram
test-using-zram:
	cd cijoe && cijoe workflows/prep_and_test.yaml \
		--config configs/localhost-zram.toml \
		--monitor

.PHONY: test
test: test-using-zram