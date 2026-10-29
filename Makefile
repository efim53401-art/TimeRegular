CC ?= gcc
CFLAGS ?= -O2
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)

CFLAGS += -Wall -Wextra $(GTK_CFLAGS)
LIBS := $(GTK_LIBS) -lm

APP := timeregular
VERSION := 1.0.0
ARCH := $(shell dpkg --print-architecture)
BUILD := build
DEBROOT := $(BUILD)/$(APP)_$(VERSION)_$(ARCH)
DEB := $(BUILD)/$(APP)_$(VERSION)_$(ARCH).deb

.PHONY: all run deb install clean

all: $(BUILD)/$(APP)

$(BUILD)/$(APP): src/main.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

run: all
	./$(BUILD)/$(APP)

deb: all
	rm -rf $(DEBROOT)
	install -d $(DEBROOT)/DEBIAN \
	  $(DEBROOT)/usr/bin \
	  $(DEBROOT)/usr/share/$(APP) \
	  $(DEBROOT)/usr/share/applications \
	  $(DEBROOT)/usr/share/icons/hicolor/scalable/apps
	install -m 0755 $(BUILD)/$(APP) $(DEBROOT)/usr/bin/$(APP)
	install -m 0644 src/style.css $(DEBROOT)/usr/share/$(APP)/style.css
	install -m 0644 packaging/$(APP).desktop $(DEBROOT)/usr/share/applications/$(APP).desktop
	install -m 0644 src/$(APP).svg $(DEBROOT)/usr/share/icons/hicolor/scalable/apps/$(APP).svg
	install -m 0644 packaging/control $(DEBROOT)/DEBIAN/control
	install -m 0755 packaging/postinst $(DEBROOT)/DEBIAN/postinst
	dpkg-deb --build --root-owner-group $(DEBROOT) $(DEB)
	@echo "Built $(DEB)"

install: deb
	sudo dpkg -i $(DEB) || sudo apt-get -f install -y

clean:
	rm -rf $(BUILD)
