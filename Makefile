BUILDDIR = build
BUILD_TYPE ?= release

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
	meson setup $(BUILDDIR) --buildtype=$(BUILD_TYPE)

.PHONY: build
build:
	meson compile -C $(BUILDDIR)
	
.PHONY: install
install:
	meson install -C $(BUILDDIR)

#
# Setup test-environment using zram
#
.PHONY: test-setup-zram
test-setup-zram:
	cd cijoe && cijoe workflows/setup_zram.yaml \
		--config configs/localhost-zram.toml \
		--monitor

#
# Setup test-environment using NVMe
#
.PHONY: test-setup-nvme
test-setup-nvme:
	cd cijoe && cijoe workflows/setup_nvme.yaml \
		--config configs/localhost-nvme.toml \
		--monitor

#
# Run test using ZRAM
#
.PHONY: test-using-zram
test-using-zram:
	cd cijoe && cijoe workflows/test.yaml \
		--config configs/localhost-zram.toml \
		--monitor

#
# Run test using NVMe
#
.PHONY: test-using-nvme
test-using-nvme:
	cd cijoe && cijoe workflows/test.yaml \
		--config configs/localhost-nvme.toml \
		--monitor

#
# Setup ZRAM and run tests using it
#
.PHONY: test
test: test-setup-zram test-using-zram