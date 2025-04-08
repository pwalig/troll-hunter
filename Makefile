CXXFLAGS = -O3 -Wall -pedantic

build:
	mkdir -p build
	mpicxx $(CXXFLAGS) src/main.cpp -o build/main

clean:
	rm -rf build