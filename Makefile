builddir = build

.PHONY: all
all: clean configure build run

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

.PHONY: run
run:
#	sudo chown safl:safl /dev/sda1
#	sudo mount -o nodatacow /dev/sda1
	./$(builddir)/xal /dev/sda1