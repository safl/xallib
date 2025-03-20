builddir = build
BUILD_TYPE ?= release

.PHONY: all
all: clean configure build install test

.PHONY: clean
clean:
	rm -rf $(builddir)

.PHONY: configure
configure:
	meson setup $(builddir) --buildtype=$(BUILD_TYPE)

.PHONY: build
build:
	meson compile -C $(builddir)
	
.PHONY: install
install:
	meson install -C $(builddir)

.PHONY: test
test:
	cd cijoe && cijoe workflows/test.yaml \
		--config configs/cijoe-config.toml \
		--monitor
