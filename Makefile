

PROTOBUF_ROOT=third_party/protobuf
PROTOC=$(PROTOBUF_ROOT)/src/protoc

MODULE_big = postgres_protobuf
EXTENSION = postgres_protobuf
DATA = postgres_protobuf--0.1.sql
DOCS = README.md
REGRESS = postgres_protobuf
OBJS=$(patsubst %.cpp, %.o, $(wildcard *.cpp))
BC_FILES=$(patsubst %.o, %.bc, $(OBJS))
DESC_SET_FILES=$(patsubst %.proto, %.pb, $(wildcard test_protos/*.proto))

PG_CPPFLAGS=-I$(PROTOBUF_ROOT)/src -Wno-deprecated -std=c++17 -Wno-register
PG_CXXFLAGS=-fPIC
PG_LDFLAGS=-Wl,--whole-archive $(PROTOBUF_ROOT)/src/.libs/libprotobuf.a -Wl,--no-whole-archive

ifdef DEBUG_PRINT
PG_CPPFLAGS+=-DDEBUG_PRINT
endif

ifndef PG_CONFIG
	PG_CONFIG := pg_config
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Add targets to `all` and `clean`
all: sql/postgres_protobuf.sql expected/postgres_protobuf.out $(DESC_SET_FILES)
clean: pb_clean

protoc: $(PROTOC)

# Hack to get protobuf headers and libraries before building any of our stuff
$(OBJS) $(BC_FILES): $(PROTOC)

# Instead of proper dependency tracking, it's easier to make all compilation units depend on all headers.
# Good enough for a small project.
$(OBJS) $(BC_FILES): $(wildcard *.hpp)

$(PROTOC):
	./build-protobuf-library.sh

sql/postgres_protobuf.sql: generate_test_cases.rb $(DESC_SET_FILES)
	env PROTOC=$(PROTOC) ./generate_test_cases.rb sql

expected/postgres_protobuf.out: generate_test_cases.rb $(DESC_SET_FILES)
	env PROTOC=$(PROTOC) ./generate_test_cases.rb expected-output

%.pb: %.proto $(PROTOC) 
	$(PROTOC) -I test_protos --descriptor_set_out=$@ $<

pb_clean:
	rm -f $(DESC_SET_FILES)

# Work around weird error mentioned here: https://github.com/rdkit/rdkit/issues/2192
COMPILE.cxx.bc = $(CLANG) -xc++ -Wno-ignored-attributes $(BITCODE_CPPFLAGS) $(CPPFLAGS) -emit-llvm -c
%.bc : %.cpp
	$(COMPILE.cxx.bc) -o $@ $<
	$(LLVM_BINPATH)/opt -module-summary -f $@ -o $@

.PHONY: all clean protoc erb_clean pb_clean
