# Builds WirelessBatteryCenter.exe with MinGW-w64 (MSYS2 MINGW64/UCRT64 shell, or any mingw g++ on PATH).
CXX      ?= g++
WINDRES  ?= windres
CXXFLAGS ?= -std=c++17 -Os -Wall -Wno-missing-field-initializers -ffunction-sections -fdata-sections \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
LDFLAGS  ?= -municode -mwindows -static -s -Wl,--gc-sections
LIBS     := -lhid -lsetupapi -lcfgmgr32 -lole32 -loleaut32 -lcomctl32 -lgdi32 -lruntimeobject -lws2_32 -lwinhttp

SRC := $(wildcard src/*.cpp) $(wildcard src/drivers/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC)) build/app_res.o

WirelessBatteryCenter.exe: $(OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

build/%.o: src/%.cpp $(wildcard src/*.h src/drivers/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -municode -c $< -o $@

# Every lang/*.txt is a built-in UI language: joined into one resource, each after an "@file <name>" line.
build/languages.txt: $(wildcard lang/*.txt)
	@mkdir -p build
	@printf '' > $@; for f in $^; do printf '@file %s\n' "$$(basename $$f .txt)" >> $@; cat "$$f" >> $@; printf '\n' >> $@; done

build/app_res.o: res/app.rc res/version-info.rcinc res/version.h res/app.manifest res/app.ico res/app_light.ico build/languages.txt
	@mkdir -p build
	$(WINDRES) -i $< -o $@ --include-dir res --include-dir build

clean:
	rm -rf build WirelessBatteryCenter.exe

test:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tests/astro_driver_test.cpp src/drivers/astro.cpp -o build/astro_driver_test.exe
	./build/astro_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/steelseries_mouse_test.cpp src/drivers/steelseries.cpp -o build/steelseries_mouse_test.exe
	./build/steelseries_mouse_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/rapoo_protocol_test.cpp -o build/rapoo_protocol_test.exe
	./build/rapoo_protocol_test.exe
	$(CXX) $(CXXFLAGS) tests/rapoo_driver_test.cpp src/drivers/rapoo.cpp -o build/rapoo_driver_test.exe
	./build/rapoo_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/ninjutso_driver_test.cpp src/drivers/ninjutso.cpp -o build/ninjutso_driver_test.exe
	./build/ninjutso_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/cherry_driver_test.cpp src/drivers/cherry.cpp -o build/cherry_driver_test.exe
	./build/cherry_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/ninjutso_cable_test.cpp src/drivers/ninjutso.cpp -o build/ninjutso_cable_test.exe
	./build/ninjutso_cable_test.exe
	$(CXX) $(CXXFLAGS) tests/machenike_driver_test.cpp src/drivers/machenike.cpp -o build/machenike_driver_test.exe
	./build/machenike_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/niceboy_driver_test.cpp src/drivers/niceboy.cpp -o build/niceboy_driver_test.exe
	./build/niceboy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/alienware_driver_test.cpp src/drivers/alienware.cpp -o build/alienware_driver_test.exe
	./build/alienware_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/mchose_modern_driver_test.cpp src/drivers/mchose_modern.cpp -o build/mchose_modern_driver_test.exe
	./build/mchose_modern_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/vaxee_driver_test.cpp src/drivers/vaxee.cpp -o build/vaxee_driver_test.exe
	./build/vaxee_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/finalmouse_driver_test.cpp src/drivers/finalmouse.cpp -o build/finalmouse_driver_test.exe
	./build/finalmouse_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/keychron_driver_test.cpp src/drivers/keychron.cpp -o build/keychron_driver_test.exe
	./build/keychron_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_haste_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_haste_driver_test.exe
	./build/hyperx_haste_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/galaxy_buds_protocol_test.cpp -o build/galaxy_buds_protocol_test.exe
	./build/galaxy_buds_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/logitech_g435_0adf_test.cpp -o build/logitech_g435_0adf_test.exe
	./build/logitech_g435_0adf_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Isrc tests/xenta_protocol_test.cpp src/drivers/xenta_protocol.cpp -o build/xenta_protocol_test.exe
	./build/xenta_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/labels_test.cpp src/labels.cpp -o build/labels_test.exe
	./build/labels_test.exe
	$(CXX) $(CXXFLAGS) tests/brands_test.cpp src/brands.cpp -o build/brands_test.exe
	./build/brands_test.exe
	$(CXX) $(CXXFLAGS) tests/device_profiles_test.cpp src/device_profiles.cpp -o build/device_profiles_test.exe
	./build/device_profiles_test.exe
	$(CXX) $(CXXFLAGS) tests/settings_migration_test.cpp src/settings.cpp src/device_profiles.cpp -o build/settings_migration_test.exe
	./build/settings_migration_test.exe

.PHONY: clean test
