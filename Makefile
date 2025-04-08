CXXFLAGS = -O3 -Wall -pedantic

main: clean
	mkdir -p build
	mpicxx $(CXXFLAGS) src/main.cpp -o build/main

no_sleep: clean
	mkdir -p build
	mpicxx -DNO_SLEEPING $(CXXFLAGS) src/main.cpp -o build/main

clean:
	rm -rf build