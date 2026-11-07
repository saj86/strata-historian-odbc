# Dev workflow on macOS or Linux. Tools go under build/tools; nothing is installed system-wide.
#   make            build the driver and tests
#   make test       run all portable tests
#   make install    install the driver and fixture DSNs under PREFIX
#   make query      open an interactive SQL prompt on the fixture DSN
#   make package-windows   build the unsigned Windows test zip (needs no Windows machine)
# This is not the Windows release path: use scripts/Build-And-Test.ps1 on Windows for the MSI.

VERSION   ?= 0.1.0
PREFIX    ?= $(HOME)/.local/strata-odbc
ROOT      := $(CURDIR)
TOOLS     := $(ROOT)/build/tools
VENV      := $(TOOLS)/venv
CMAKE     := $(VENV)/bin/cmake
ODBC      := $(TOOLS)/unixodbc
BUILD     := $(ROOT)/build/dev
UNAME     := $(shell uname)
EXT       := $(if $(filter Darwin,$(UNAME)),dylib,so)
DRIVER    := Strata Historian ODBC Driver
UO_VER    := 2.3.14
UO_SHA    := 4e2814de3e01fc30b0b9f75e83bb5aba91ab0384ee951286504bb70205524771
UO_URL    := https://www.unixodbc.org/unixODBC-2.3.14.tar.gz
PKG       := strata-odbc-$(VERSION)-win-x64-UNSIGNED-TEST
export PATH := $(VENV)/bin:$(ODBC)/bin:$(PATH)

.PHONY: all tools build test install uninstall query package-windows clean
all: build

$(VENV)/.done:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip -q install --disable-pip-version-check ziglang cmake ninja pefile
	touch $@

$(ODBC)/.done:
	mkdir -p $(TOOLS)/uo-src $(TOOLS)/uo-build
	curl -fsSL -o $(TOOLS)/unixODBC.tar.gz $(UO_URL)
	echo "$(UO_SHA)  $(TOOLS)/unixODBC.tar.gz" | shasum -a 256 -c -
	tar xzf $(TOOLS)/unixODBC.tar.gz -C $(TOOLS)/uo-src --strip-components=1
	cd $(TOOLS)/uo-build && ../uo-src/configure --prefix=$(ODBC) --disable-gui --enable-drivers=no --enable-driverc=no --with-pic >/dev/null
	$(MAKE) -C $(TOOLS)/uo-build -j8 >/dev/null 2>&1
	$(MAKE) -C $(TOOLS)/uo-build install >/dev/null 2>&1
	touch $@

tools: $(VENV)/.done $(ODBC)/.done

build: tools
	$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_MAKE_PROGRAM=$(VENV)/bin/ninja -DCMAKE_BUILD_TYPE=Release -DODBC_PREFIX=$(ODBC) >/dev/null
	$(CMAKE) --build $(BUILD)

test: tools
	ODBC_PREFIX=$(ODBC) CMAKE=$(CMAKE) NINJA=$(VENV)/bin/ninja BUILD=$(BUILD) scripts/run-dev-tests.sh
	python3 -B -m unittest discover -s fabric/tests

install: build
	mkdir -p $(PREFIX)/lib $(PREFIX)/etc $(PREFIX)/bin
	cp $(BUILD)/strataodbc.$(EXT) $(PREFIX)/lib/
	printf '[$(DRIVER)]\nDriver=$(PREFIX)/lib/strataodbc.$(EXT)\nUsageCount=1\n' > $(PREFIX)/etc/odbcinst.ini
	printf '[StrataFixture]\nDriver=$(DRIVER)\nBackend=fixture\n\n[StrataFixturePreview]\nDriver=$(DRIVER)\nBackend=fixture\nNavigatorPreview=1\n' > $(PREFIX)/etc/odbc.ini
	printf '#!/bin/sh\nexport ODBCSYSINI="$(PREFIX)/etc" ODBCINI="$(PREFIX)/etc/odbc.ini"\nexec "$(ODBC)/bin/iusql" "$${1:-StrataFixture}" -v\n' > $(PREFIX)/bin/strata-sql
	chmod +x $(PREFIX)/bin/strata-sql
	@echo "Installed to $(PREFIX). Try: $(PREFIX)/bin/strata-sql"

uninstall:
	rm -f $(PREFIX)/lib/strataodbc.$(EXT) $(PREFIX)/etc/odbcinst.ini $(PREFIX)/etc/odbc.ini $(PREFIX)/bin/strata-sql
	-rmdir $(PREFIX)/lib $(PREFIX)/etc $(PREFIX)/bin $(PREFIX)

query:
	@$(PREFIX)/bin/strata-sql

package-windows: $(VENV)/.done
	ZIGCXX="$(VENV)/bin/python -m ziglang c++" OUT=$(ROOT)/build/cross-windows scripts/cross-check-windows.sh >/dev/null
	rm -rf dist/$(PKG) && mkdir -p dist/$(PKG)/bin dist/$(PKG)/scripts dist/$(PKG)/fixtures dist/$(PKG)/docs dist/$(PKG)/powerquery
	cp build/cross-windows/strataodbc.dll build/cross-windows/core_tests.exe build/cross-windows/odbc_smoke.exe dist/$(PKG)/bin/
	cp scripts/Install-Portable.ps1 scripts/Test-Package.ps1 scripts/Provision-StrataDsn.ps1 scripts/Test-StrataNode.ps1 dist/$(PKG)/scripts/
	cp fixtures/expected_pressure_window.csv dist/$(PKG)/fixtures/
	cp docs/powerbi-acceptance.md dist/$(PKG)/docs/
	cp powerquery/examples.pq dist/$(PKG)/powerquery/
	cp packaging/README-TEST.txt dist/$(PKG)/README-TEST.txt
	cd dist/$(PKG) && shasum -a 256 bin/* > SHA256SUMS.txt
	cd dist && rm -f $(PKG).zip && zip -qr $(PKG).zip $(PKG)
	@ls -l dist/$(PKG).zip

clean:
	rm -rf build/dev build/cross-windows dist
