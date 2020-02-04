-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION postgres_protobuf" to load this file. \quit

CREATE FUNCTION protobuf_extension_version()
    RETURNS BIGINT
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT STABLE;

-- Query functions are STABLE, not IMMUTABLE, because they reads
-- `postgres_protobuf_descriptors`. This means that the functions
-- can't be used in index expressions.

CREATE FUNCTION protobuf_query(
    IN TEXT,  -- Query
    IN BYTEA  -- Binary protobuf
)
    RETURNS TEXT
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT STABLE;

CREATE FUNCTION protobuf_query_multi(
    IN TEXT,  -- Query
    IN BYTEA  -- Binary protobuf
)
    RETURNS SETOF TEXT
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT STABLE;

CREATE FUNCTION protobuf_to_json_text(
    IN TEXT,  -- protobuf type
    IN BYTEA  -- Binary protobuf
)
    RETURNS TEXT
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT STABLE;

CREATE FUNCTION protobuf_from_json_text(
    IN TEXT,  -- protobuf type
    IN TEXT   -- JSON text
)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT STABLE;

CREATE TABLE protobuf_file_descriptor_sets (
    name TEXT NOT NULL,
    file_descriptor_set BYTEA NOT NULL,
    PRIMARY KEY (name)
);
SELECT pg_catalog.pg_extension_config_dump ('protobuf_file_descriptor_sets', '');
