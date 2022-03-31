# Makefile
TARGETS = main
MODULES += lib
HEADERS += ctypes

all: bin

# append common rules (have to do it here)
include Makefile.common