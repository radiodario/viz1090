#
# When building a package or installing otherwise in the system, make
# sure that the variable PREFIX is defined, e.g. make PREFIX=/usr/local
#

# -MMD -MP: write .d files so changing a header rebuilds everything that
# includes it (otherwise objects keep a stale class layout and crash)
CXXFLAGS=-O2 -std=c++11 -g -MMD -MP
LIBS= -lm -lcurl -lSDL2 -lSDL2_ttf -lSDL2_gfx -lSDL2_image -lpthread -g
CXX=g++

all: viz1090

%.o: %.c %.cpp
	$(CXX) $(CXXFLAGS) $(EXTRACFLAGS) -c $<

viz1090: viz1090.o AppData.o AircraftList.o Aircraft.o Label.o AircraftLabel.o anet.o interactive.o mode_ac.o mode_s.o net_io.o Input.o View.o Map.o parula.o monokai.o TransitBoard.o WeatherData.o ViewWeather.o RadarData.o AdsbApi.o
	$(CXX) -o viz1090 viz1090.o AppData.o AircraftList.o Aircraft.o Label.o AircraftLabel.o anet.o interactive.o mode_ac.o mode_s.o net_io.o Input.o View.o Map.o parula.o monokai.o TransitBoard.o WeatherData.o ViewWeather.o RadarData.o AdsbApi.o $(LIBS) $(LDFLAGS)

-include $(wildcard *.d)

clean:
	rm -f \
		airportdata.bin \
		airportnames \
		mapdata/* \
		mapdata.bin \
		mapnames \
		*.o \
		*.d \
		viz1090
