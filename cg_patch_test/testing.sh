#!/bin/sh
../inst/bin/valgrind --tool=callgrind --callgrind-out-file=callout.bsortcpp ./bsort
../inst/bin/valgrind --tool=callgrind --callgrind-out-file=callout.bsortcppOC --collect-openclose=yes ./bsort
../inst/bin/valgrind --tool=callgrind --callgrind-out-file=callout.bsortpy ./bsort.py
../inst/bin/valgrind --tool=callgrind --callgrind-out-file=callout.bsortpyOC --collect-openclose=yes ./bsort.py
../inst/bin/valgrind --tool=callgrind --callgrind-out-file=callout.fileIOjava java FileIO
../inst/bin/valgrind --tool=callgrind --callgrind-out-file=callout.fileIOjavaOC --collect-openclose=yes java FileIO
