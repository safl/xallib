builddir = build

.PHONY: all
all: clean configure build test

.PHONY: clean
clean:
	rm -rf $(builddir)

.PHONY: configure
configure:
	meson setup $(builddir) --reconfigure

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
