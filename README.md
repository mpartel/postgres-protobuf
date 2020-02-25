# Protocol Buffer extension for PostgreSQL

![Tests on PG11](https://github.com/mpartel/postgres-protobuf/workflows/Tests%20on%20PG11/badge.svg)
![Tests on PG12](https://github.com/mpartel/postgres-protobuf/workflows/Tests%20on%20PG12/badge.svg)

Features:

- Converting [protobuf](https://developers.google.com/protocol-buffers/) columns to and from JSON.
- Selecting parts of a protobuf.

Examples:

```sql
SELECT protobuf_query('MyProto:some_submessage.some_map[some_key]', my_proto_column) FROM ...;

-- or for multiple results
SELECT protobuf_query_multi('MyProto:some_repeated_field[*].some_map[*].some_field', my_proto_column) FROM ...;
```

## Why put protobufs in a database?

Protobufs in a database is in many ways similar to JSON in a database:
- less code needed to convert data structures into database rows and back
- less alter tables required as data fields are added and removed

The main advantage of protobufs over JSON is a compact and efficient representation.
The main advantage of JSON is human-readability without extra steps.

### What about storing protobufs as JSON?

Protobufs have a well-defined JSON representation supported by most implementations.
Using that for storage (as [`json` or `jsonb`](https://www.postgresql.org/docs/current/datatype-json.html))
is a valid strategy if efficiency is not a major concern.

Note that JSON protobufs store fields by name instead of by number,
which means that field renames will break backwards- and forwards-compatibility.

## Installation from source

Requires Postgres 11 or newer.

Prerequisites for installing from source:

- Postgres server headers (if you use the Postgres APT repo, install `postgresql-server-dev-$VERSION`)
- A (mostly) C++17-capable compiler (Ubuntu 18.04's default GCC 7.4.0 works)

To install:

```bash
make
sudo make install
```

Then in Postgres:

```sql
CREATE EXTENSION postgres_protobuf;
```

## Installation from binary release

Prebuilt binaries are [here](https://github.com/mpartel/postgres-protobuf/releases).

Currently they are built with and tested on Ubuntu 18.04 only.
You can build your own binary package from source with `make dist`.

On Ubuntu, install the contents of the binary package like this:
- copy the contents of `lib/` to `/usr/lib/postgresql/11/lib/`
- copy the contents of `extension/` to `/usr/share/postgresql/11/extension/`

Other distros may have those Postgres directories elsewhere.

## Usage

First, you need to tell the extension about your protobuf schema.
Run the protobuf compiler `protoc` on your `.proto` files with
`--descriptor_set_out=path/to/schema.pb` and `--include_imports`,
then insert that file to the database with

```sql
INSERT INTO protobuf_file_descriptor_sets (name, file_descriptor_set)
VALUES ('default', contents_of_file)
```

where `contents_of_file` is a Postgres [byte array](https://www.postgresql.org/docs/current/datatype-binary.html).
Commit the transaction if you're in one.

Now you can query protobufs described by your schema like this:

```sql
SELECT protobuf_query('path.to.Message:path.to.field', protobuf_as_byte_array) AS result;
```

## Reference

The following functions are defined:

- `protobuf_query(query, protobuf)` returns the first matching field in the protobuf, or NULL if missing or proto3 default.
- `protobuf_query_multi(query, protobuf)` returns all matching fields in the protobuf. Missing or proto3 default values are not returned.
- `protobuf_to_json_text(protobuf_type, protobuf)` converts the protobuf to a JSON string, assuming it's of the given type.
- `protobuf_from_json_text(protobuf_type, json_str)` parses a protobuf from a JSON string, assuming it's of the given type.
- `protobuf_extension_version()` returns the extension version `X.Y.Z` as a number `X*10000+Y*100+Z`.

*Queries* take the form `[<descriptor_set>:]<message_name>:<path>`
where

- `<descriptor_set>` (optional) is the name of the descriptor set you inserted into `protobuf_file_descriptor_sets`. Defaults to `default`.
- `<message_name>` is a fully qualified name of a protobuf message.
- `<path>` may be composed of
    - *field selectors* like `submessage.field`. Fields may be specified by name or by number.
    - *index selectors* like `field[123]`, which select the Nth element of a repeated field.
    - *map value selectors* like `field[123]` or `field[abc]`, which select the given map key (both numbers and strings work).
    - *universal selectors* written `field[*]`, which select all elements of a repeated field or map.
    - *universal map key selectors* written `field|keys`, which select all keys of a map.

## Caveats

While this extension should be good to go for exploratory queries and
other non-demanding workloads, there are some limitations to consider
before relying on it too heavily in a design.

### Security

This extension is fairly new and written in C++, so some caution is warranted.
It's may be unwise to give it untrusted queries or protobuf data that you haven't
parsed and reserialized first. Conversion to/from JSON should be safer since it
thinly wraps the well-tested protobuf library, but see the note about memory management below.

### Performance

Queries need to load and scan through the entire protobuf column. Storing and
querying large protobufs can be significantly slower than splitting the data into columns
that can be queried individually.

In the current version, protobuf schemas are deserialized and cached only for the duration of a single transaction.
If you intend to run many SELECTs on protobufs, wrap them in a transaction for better performance.

In the current version, there is no way to use the query functions as index expressions,
because the query functions depend on your protobuf schema, which may change over time.
In other words, you can't create an index (nor a `UNIQUE` constraint) on the contents of a protobuf column.
A future version may allow such indices for queries that are written entirely in terms of field numbers.

### Memory management

This extension allocates most things on the default C++ heap,
because the protobuf library does not support custom allocators.
This memory might not be properly accounted for by Postgres's
memory management and monitoring systems.

While care has been taken to avoid memory leaks and to tolerate memory exhaustion at any point,
this has not been rigorously tested, and I am unsure whether the protobuf library always cleans up
correctly after a `bad_alloc` exception.

In the current version, protobuf [map](https://developers.google.com/protocol-buffers/docs/proto3#maps)
values are buffered before being scanned by queries. This means that huge protobufs whose bulk is under a
highly nested map may take a lot of memory. Note that an attacker can easily construct such a
protobuf if the schema has recursion involving a map. This caveat applies only to queries,
not to JSON conversion.

Other than the above case, memory use is linear, or roughly
`O(|descriptor sets| + |largest protobuf queried| + |result set|)`.

### Compatibility

Protobuf versions 2 and 3 should both work,
but [groups](https://developers.google.com/protocol-buffers/docs/proto#groups)
(which have been deprecated for a long time)
are currently not supported and will cause queries to fail if encountered.

Note that proto3 does not store default values for fields.
The current implementation returns no result for missing values.
This means that e.g. a numeric field whose value is 0 will not show up in results.
A future version may change this default.

This extension has so far only been tested on AMD64.

### Advanced operations

The current version does not provide functions for modifying the contents of protobufs,
and there are no concrete plans to add such functonality.
There are also no concrete plans to significantly extend the query language.

If you do need these features occasionally, consider converting to JSON and back,
since Postgres has [a wide range of JSON operations](https://www.postgresql.org/docs/current/functions-json.html).

## Comparison with pg_protobuf

There is an older project [pg_protobuf](https://github.com/afiskon/pg_protobuf),
which appears to have been an experiment that is no longer actively maintained.
It has less features and conveniences, e.g. fields must be referred to by number,
but its code is much simpler, with no dependency on the Protobuf library or C++.

## License

[MIT](LICENSE.txt)
