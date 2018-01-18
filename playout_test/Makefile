EXTRAINCLUDES =
EXTRACFLAGS  =
EXTRACPPFLAGS = -std=c++0x -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 $(EXTRAINCLUDES)
EXTRA_LIBS = -lrt -lpthread

# Look for sources in other directories
VPATH  = ./

MODULE = playout_test
SRCS = $(wildcard *.cpp)
CSRCS = $(wildcard *.c)

OBJS = $(SRCS:.cpp=.o) $(CSRCS:.c=.o)

include ./Makefile.defs
