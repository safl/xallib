BUILDDIR = build
BUILD_TYPE ?= release

.PHONY: all
all: clean configure build install test

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

.PHONY: test
test:
	cd cijoe && cijoe workflows/test.yaml \
		--config configs/cijoe-config.toml \
		--monitor