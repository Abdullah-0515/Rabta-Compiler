# ============================================================================
#  Rabta Programming Language
#  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
#  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
# ============================================================================
#
#  make            build the compiler ./rabtac (Flex + Bison + LLVM C API)
#  make examples   compile every examples/*.rabta to a native binary in build/
#  make ir         write the LLVM IR of every example to build/*.ll
#  make test       build, then run the whole test suite (PASS/FAIL)
#  make clean      remove all generated files
#  make pdf        regenerate README.pdf from README.md (needs pandoc)
#
#  The LLVM installation is found with llvm-config; override it with e.g.
#      make LLVM_CONFIG=llvm-config-18

CC          := gcc
LLVM_CONFIG ?= llvm-config
BISON       ?= bison
FLEX        ?= flex

LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags 2>/dev/null)
LLVM_LIBS   := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis target native 2>/dev/null) \
               $(shell $(LLVM_CONFIG) --system-libs 2>/dev/null)

CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2 -g
CFLAGS  += $(LLVM_CFLAGS)
LDLIBS  := $(LLVM_LIBS)

TARGET  := rabtac
GEN     := rabta.tab.c rabta.tab.h lex.yy.c rabta.output
OBJS    := rabta.tab.o lex.yy.o ast.o semantic.o protocol.o \
           llvm_gen.o llvm_flow.o llvm_func.o llvm_actor.o toolchain.o

EXAMPLES := $(patsubst examples/%.rabta,build/%,$(wildcard examples/*.rabta))
IRFILES  := $(addsuffix .ll,$(EXAMPLES))

.PHONY: all clean test pdf examples ir check-llvm

all: check-llvm $(TARGET)

check-llvm:
	@command -v $(LLVM_CONFIG) >/dev/null 2>&1 || { \
	    echo "error: '$(LLVM_CONFIG)' not found. Install LLVM with:"; \
	    echo "    sudo apt install llvm llvm-dev clang"; \
	    echo "or pass LLVM_CONFIG=llvm-config-<version>."; exit 1; }

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Bison: -d writes rabta.tab.h; -Wall reports any conflict as a warning.
rabta.tab.c rabta.tab.h: rabta.y
	$(BISON) -d -Wall rabta.y

lex.yy.c: rabta.l rabta.tab.h
	$(FLEX) rabta.l

rabta.tab.o: rabta.tab.c ast.h semantic.h protocol.h llvm_gen.h toolchain.h
lex.yy.o: lex.yy.c rabta.tab.h ast.h
ast.o: ast.c ast.h
semantic.o: semantic.c semantic.h ast.h
protocol.o: protocol.c protocol.h semantic.h ast.h
llvm_gen.o: llvm_gen.c llvm_gen.h llvm_gen_priv.h ast.h
llvm_flow.o: llvm_flow.c llvm_gen_priv.h ast.h
llvm_func.o: llvm_func.c llvm_gen_priv.h semantic.h ast.h
llvm_actor.o: llvm_actor.c llvm_gen_priv.h ast.h
toolchain.o: toolchain.c toolchain.h

# ---- compiling Rabta programs ----

build:
	mkdir -p build

build/%: examples/%.rabta $(TARGET) | build
	./$(TARGET) $< -o $@

build/%.ll: examples/%.rabta $(TARGET) | build
	./$(TARGET) -S $< -o $@

examples: $(EXAMPLES)

ir: $(IRFILES)

test: all
	@sh tests/run_tests.sh

pdf: README.pdf

README.pdf: README.md
	pandoc README.md -o README.pdf --pdf-engine=pdflatex \
	    -V geometry:margin=2cm -V fontsize=10pt -V colorlinks=true \
	    --highlight-style=tango --toc

clean:
	rm -f $(TARGET) rabta $(OBJS) $(GEN)
	rm -rf build
