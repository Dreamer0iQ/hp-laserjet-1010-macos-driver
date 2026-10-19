VERSION = 1.0.0
PKG_ID  = io.github.dreamer0iq.hp-laserjet-1010

CC      = clang
CFLAGS  = -O2 -Wall -Wextra -Wno-deprecated-declarations -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
LDLIBS  = -lcups

BUILD   = build
FILTER  = $(BUILD)/rastertolj1010
PPDS    = $(BUILD)/hp1010fr.ppd $(BUILD)/hp1012fr.ppd
PKG     = $(BUILD)/HP-LaserJet-1010-macOS-$(VERSION).pkg

all: $(FILTER) $(PPDS)

$(FILTER): src/rastertolj1010.c src/deltaplus.c src/deltaplus.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/rastertolj1010.c src/deltaplus.c $(LDLIBS)
	codesign -s - -f $@

$(PPDS): ppd/hp1010fr.drv | $(BUILD)
	ppdc -l en -d $(BUILD) ppd/hp1010fr.drv

$(BUILD):
	mkdir -p $@

check: all
	cupstestppd -I filters -W translations $(PPDS)

# End-to-end without a printer: PDF -> cgpdftoraster -> filter -> decode to PNG.
test: all
	mkdir -p $(BUILD)/test
	python3 tools/testpage.py $(BUILD)/test/testpage.pdf
	sed "s|/Library/Printers/HP1010FastRaster/Filters|$(CURDIR)/$(BUILD)|" $(BUILD)/hp1010fr.ppd > $(BUILD)/test/local.ppd
	cupsfilter -p $(BUILD)/test/local.ppd -m printer/foo -e $(BUILD)/test/testpage.pdf \
		> $(BUILD)/test/testpage.fr 2> $(BUILD)/test/cupsfilter.log
	python3 tools/frdecode.py $(BUILD)/test/testpage.fr $(BUILD)/test/decoded

pkg: $(PKG)

$(PKG): all install.sh uninstall.sh scripts/postinstall tools/mkpkg.sh
	rm -rf $(BUILD)/pkgroot $(BUILD)/pkgscripts
	install -d -m 755 $(BUILD)/pkgroot/Filters $(BUILD)/pkgroot/PPDs $(BUILD)/pkgscripts
	install -m 755 $(FILTER) $(BUILD)/pkgroot/Filters/
	install -m 755 uninstall.sh $(BUILD)/pkgroot/
	for m in 1010 1012; do \
		gzip -9nc $(BUILD)/hp$${m}fr.ppd > "$(BUILD)/pkgroot/PPDs/HP LaserJet $$m FastRaster.gz"; \
	done
	chmod 644 $(BUILD)/pkgroot/PPDs/*.gz
	install -m 755 scripts/postinstall $(BUILD)/pkgscripts/postinstall
	sh tools/mkpkg.sh $(BUILD)/pkgroot $(BUILD)/pkgscripts $(PKG_ID) $(VERSION) \
		/Library/Printers/HP1010FastRaster $@

install: all
	sudo ./install.sh

uninstall:
	sudo ./uninstall.sh

clean:
	rm -rf $(BUILD)

.PHONY: all check test pkg install uninstall clean
