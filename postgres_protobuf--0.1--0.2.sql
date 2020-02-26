-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION postgres_protobuf" to load this file. \quit

CREATE FUNCTION protobuf_query_array(
    IN TEXT,  -- Query
    IN BYTEA  -- Binary protobuf
)
    RETURNS TEXT[]
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT STABLE;
