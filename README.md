# This is terrafs - a fuse base file system for FlightGear scenery

Mount terrasync scenery as a file system.

## Usage
    terrafs -oserver=http://flightgear.sourceforge.net/scenery /path/to/my/scenery
    fgfs --fg-scenery=/path/to/my/scenery
    fusermount -u /path/to/my/scenery

## Build
Prerequisites
You need libcurl and fuse installed along with their devel packages (header files)
There is no make script (yet) but it's easy:
    g++ -std=c++0x -c ../src/terrafs.cpp
    g++ -o terrafs terrafs.o -lfuse -lcurl

