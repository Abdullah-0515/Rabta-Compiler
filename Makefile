# ============================================================================
#  Rabta Programming Language
#  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
#  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
# ============================================================================
#
#  make         build the interpreter ./rabta
#  make test    build, then run every example and negative test (PASS/FAIL)
#  make clean   remove all generated files
#  make pdf     regenerate README.pdf from README.md (needs pandoc)

CC      := gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2 -g
LDLIBS  := -lpthread
BISON   ?= bison
FLEX    ?= flex

TARGET  := rabta
GEN     := rabta.tab.c rabta.tab.h lex.yy.c rabta.output
OBJS    := rabta.tab.o lex.yy.o ast.o semantic.o protocol.o interpreter.o runtime.o

.PHONY: all clean test pdf

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Bison: -d writes rabta.tab.h; -Wall reports any conflict as a warning.
rabta.tab.c rabta.tab.h: rabta.y
	$(BISON) -d -Wall rabta.y

lex.yy.c: rabta.l rabta.tab.h
	$(FLEX) rabta.l

rabta.tab.o: rabta.tab.c ast.h semantic.h protocol.h interpreter.h
lex.yy.o: lex.yy.c rabta.tab.h ast.h
ast.o: ast.c ast.h
semantic.o: semantic.c semantic.h ast.h
protocol.o: protocol.c protocol.h semantic.h ast.h
interpreter.o: interpreter.c interpreter.h runtime.h semantic.h ast.h
runtime.o: runtime.c runtime.h

test: $(TARGET)
	@sh tests/run_tests.sh

pdf: README.pdf

README.pdf: README.md
	pandoc README.md -o README.pdf --pdf-engine=pdflatex \
	    -V geometry:margin=2cm -V fontsize=10pt -V colorlinks=true \
	    --highlight-style=tango --toc

clean:
	rm -f $(TARGET) $(OBJS) $(GEN)
