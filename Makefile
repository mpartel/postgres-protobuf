
# If you change the major or minor version, remember to edit
# `postgres_protobuf.control` too.
# To release a new version, change these numbers and
# run ./build-and-test.sh to update the test cases.
EXT_VERSION_MAJOR = 0
EXT_VERSION_MINOR = 3
EXT_VERSION_PATCHLEVEL = 2
EXT_VERSION = $(EXT_VERSION_MAJOR).$(EXT_VERSION_MINOR).$(EXT_VERSION_PATCHLEVEL)

PROTOBUF_ROOT=third_party/protobuf
PROTOC=$(PROTOBUF_ROOT)/src/protoc

MODULE_big = postgres_protobuf
EXTENSION = postgres_protobuf
DATA = postgres_protobuf--0.1.sql postgres_protobuf--0.1--0.2.sql
DOCS = README.md
REGRESS = postgres_protobuf
OBJS=$(patsubst %.cpp, %.o, $(wildcard *.cpp))
BC_FILES=$(patsubst %.o, %.bc, $(OBJS))
DESC_SET_FILES=$(patsubst %.proto, %.pb, $(wildcard test_protos/*.proto))

PG_CPPFLAGS=-I$(PROTOBUF_ROOT)/src -Wno-deprecated -std=c++17 -Wno-register -DEXT_VERSION_MAJOR=$(EXT_VERSION_MAJOR) -DEXT_VERSION_MINOR=$(EXT_VERSION_MINOR) -DEXT_VERSION_PATCHLEVEL=$(EXT_VERSION_PATCHLEVEL)
PG_CXXFLAGS=-fPIC
PG_LDFLAGS=-Wl,--whole-archive $(PROTOBUF_ROOT)/src/.libs/libprotobuf.a -Wl,--no-whole-archive -lz -lstdc++

ifdef DEBUG_PRINT
PG_CPPFLAGS+=-DDEBUG_PRINT
endif

ifndef PG_CONFIG
	PG_CONFIG := pg_config
endif

PLATFORM := "$(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)"

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Add targets to `all` and `clean`
all: sql/postgres_protobuf.sql expected/postgres_protobuf.out $(DESC_SET_FILES)
clean: postgres_protobuf_clean

protoc: $(PROTOC)

# Hack to get protobuf headers and libraries before building any of our stuff
$(OBJS) $(BC_FILES): $(PROTOC)

# Instead of proper dependency tracking, it's easier to make all compilation units depend on all headers.
# Good enough for a small project.
$(OBJS) $(BC_FILES): $(wildcard *.hpp)

# Changes to this makefile should also trigger a recompile
$(OBJS) $(BC_FILES): Makefile

$(PROTOC):
	./build-protobuf-library.sh

sql/postgres_protobuf.sql: generate_test_cases.rb $(DESC_SET_FILES) Makefile
	env PROTOC=$(PROTOC) ./generate_test_cases.rb sql

expected/postgres_protobuf.out: generate_test_cases.rb $(DESC_SET_FILES) Makefile
	env PROTOC=$(PROTOC) ./generate_test_cases.rb expected-output

%.pb: %.proto $(PROTOC) 
	$(PROTOC) -I test_protos --descriptor_set_out=$@ $<

postgres_protobuf_clean:
	rm -Rf dist
	rm -f $(DESC_SET_FILES)

# Work around weird error mentioned here: https://github.com/rdkit/rdkit/issues/2192
COMPILE.cxx.bc = $(CLANG) -xc++ -Wno-ignored-attributes $(BITCODE_CPPFLAGS) $(CPPFLAGS) -emit-llvm -c
%.bc : %.cpp
	$(COMPILE.cxx.bc) -o $@ $<
	$(LLVM_BINPATH)/opt -module-summary -f $@ -o $@

DIST_TAR_BASENAME = postgres-protobuf-v$(EXT_VERSION)-$(PLATFORM)-for-pg$(MAJORVERSION)
DIST_DIR = dist/$(DIST_TAR_BASENAME)
dist: all
	rm -Rf dist
	mkdir -p $(DIST_DIR)/lib $(DIST_DIR)/lib/bitcode $(DIST_DIR)/extension $(DIST_DIR)/doc
	cp postgres_protobuf.so $(DIST_DIR)/lib/
	cp postgres_protobuf.control $(DIST_DIR)/extension/
	cp postgres_protobuf--*.sql $(DIST_DIR)/extension/
	cp README.md $(DIST_DIR)/doc/postgres-protobuf-README.md
	cp LICENSE.txt $(DIST_DIR)/doc/postgres-protobuf-LICENSE.txt
	cp *.bc $(DIST_DIR)/lib/bitcode/
	cd $(DIST_DIR)/lib/bitcode && $(LLVM_BINPATH)/llvm-lto -thinlto -thinlto-action=thinlink -o postgres_protobuf.index.bc *.bc
	tar -C dist -cvzf dist/$(DIST_TAR_BASENAME).tar.gz $(DIST_TAR_BASENAME)

.PHONY: all clean protoc postgres_protobuf_clean dist
