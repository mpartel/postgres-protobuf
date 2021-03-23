#!/usr/bin/env ruby
# Generates a test input and output file

require 'fileutils'
require 'shellwords'

class TestGen

  attr_reader :pg_proto, :pg_proto_raw

  def initialize(mode)
    @mode = mode
    @breadcrumb = []
  end

  def generate(&block)
    case @mode
    when 'sql'
      FileUtils.mkdir_p('sql')
      @file = File.open('sql/postgres_protobuf.sql', "wb")
    when 'expected-output'
      FileUtils.mkdir_p('expected')
      @file = File.open('expected/postgres_protobuf.out', "wb")
    else
      raise "invalid mode: #{@mode}"
    end

    generate_preamble
    
    begin
      instance_eval(&block)
    ensure
      @file.close
      @file = nil
    end
  end

  def section(name, &block)
    @breadcrumb.push(name)
    begin
      emit("--", nil)
      emit("-- #{@breadcrumb.join(' > ')}", nil)
      emit("--", nil)
      block.call
    ensure
      @breadcrumb.pop
    end
  end

  def test_sql(sql, expected_results)
    emit(sql, format_expected_results(expected_results))
  end

  def with_proto(text_format, proto_file='main_descriptor_set.proto', proto_name='pgpb.test.ExampleMessage', &block)
    raise "Must be call in generate() block" if @file == nil
    pg_proto_bytes = textformat_to_binary(text_format, proto_file, proto_name)
    @pg_proto = pg_binary(pg_proto_bytes)
    @pg_proto_raw = pg_binary_raw(pg_proto_bytes)
    begin
      block.call
    ensure
      @pg_proto = nil
      @pg_proto_raw = nil
    end
  end

  def test_query(query, expected_results)
    raise "Must be called in with_proto() block" if @pg_proto == nil
    raise "expected_results must be an array" unless expected_results.is_a?(Array)

    sql = <<EOS
SELECT protobuf_query_multi(
  #{pg_quote(query)},
  #{@pg_proto}
) AS result;
EOS
    sql.strip!
    test_sql(sql, expected_results)
  end

  def textformat_to_binary(text_format, proto_file='main_descriptor_set.proto', proto_name='pgpb.test.ExampleMessage')
    cmd = Shellwords.join([PROTOC, '--encode=' + proto_name, '-Itest_protos', proto_file])
    bytes = IO.popen(cmd, "rb+", encoding: "ASCII-8BIT") do |pipe|
      pipe.write text_format
      pipe.close_write
      pipe.read
    end

    raise "protoc command failed" unless $?.success?
    bytes
  end

  private

  def generate_preamble
    test_sql("--", nil)
    test_sql("-- DO NOT EDIT - This file is autogenerated by #{File.basename($0)}", nil)
    test_sql("--", nil)
    test_sql('\\pset format unaligned', nil)
    test_sql('CREATE EXTENSION postgres_protobuf;', nil)
    generate_version_check
    test_sql("INSERT INTO protobuf_file_descriptor_sets (name, file_descriptor_set) VALUES ('default', #{descriptor_set_data_hex("main_descriptor_set")});", nil)
    test_sql("INSERT INTO protobuf_file_descriptor_sets (name, file_descriptor_set) VALUES ('other', #{descriptor_set_data_hex("other_descriptor_set")});", nil)
  end

  def generate_version_check
    makefile = File.read('Makefile')
    makefile =~ /EXT_VERSION_MAJOR\s*=\s*(\d+)/ or raise "EXT_VERSION_MAJOR not found in makefile"
    major = $1.to_i
    makefile =~ /EXT_VERSION_MINOR\s*=\s*(\d+)/ or raise "EXT_VERSION_MINOR not found in makefile"
    minor = $1.to_i
    makefile =~ /EXT_VERSION_PATCHLEVEL\s*=\s*(\d+)/ or raise "EXT_VERSION_PATCHLEVEL not found in makefile"
    patchlevel = $1.to_i
    version = major * 10000 + minor * 100 + patchlevel
    test_sql('SELECT protobuf_extension_version() AS result;', [version.to_s])
  end

  def format_expected_results(expected_results)
    if expected_results != nil
      rows = expected_results.join("\n")
      result = "result\n"
      result << "#{rows}\n" unless rows.empty?
      result << "(#{expected_results.size} #{if expected_results.size == 1 then 'row' else 'rows' end})\n"
      result
    end
  end

  def emit(sql, expected)
    @file.puts(sql.strip)
    if @mode == 'expected-output' && expected != nil
      @file.puts(expected)
    end
  end

  def descriptor_set_data_hex(name)
    pg_binary(File.read("test_protos/#{name}.pb"))
  end

  def pg_binary(data)
    pg_quote(pg_binary_raw(data)) + '::BYTEA'
  end

  def pg_binary_raw(data)
    '\\x' + data.unpack('H*')[0]
  end

  def pg_quote(s)
    # (not sure this is correct, but should be good enough for this test)
    "'" + s.gsub("'", "''") + "'"
  end
end

mode = ARGV[0]

if ARGV.include?('-h') || ARGV.include?('--help')
  raise "Usage: #{$0} sql|expected-output"
end

PROTOC = ENV['PROTOC']
raise "env var PROTOC required" if PROTOC == nil || PROTOC.empty?

TestGen.new(mode).generate do
  section "Scalar values" do
    with_proto('scalars { int32_field: 123 }') do
      test_query('pgpb.test.ExampleMessage:1.3', ['123'])
      test_query('pgpb.test.ExampleMessage:scalars.int32_field', ['123'])
    end

    with_proto('scalars { int64_field: 9223372036854775807 }') do
      test_query('pgpb.test.ExampleMessage:scalars.int64_field', ['9223372036854775807'])
    end
    with_proto('scalars { sint64_field: 9223372036854775807 }') do
      test_query('pgpb.test.ExampleMessage:scalars.sint64_field', ['9223372036854775807'])
    end
    with_proto('scalars { sfixed64_field: 9223372036854775807 }') do
      test_query('pgpb.test.ExampleMessage:scalars.sfixed64_field', ['9223372036854775807'])
    end

    with_proto('scalars { int64_field: -9223372036854775808 }') do
      test_query('pgpb.test.ExampleMessage:scalars.int64_field', ['-9223372036854775808'])
    end
    with_proto('scalars { sint64_field: -9223372036854775808 }') do
      test_query('pgpb.test.ExampleMessage:scalars.sint64_field', ['-9223372036854775808'])
    end
    with_proto('scalars { sfixed64_field: -9223372036854775808 }') do
      test_query('pgpb.test.ExampleMessage:scalars.sfixed64_field', ['-9223372036854775808'])
    end

    with_proto('scalars { uint64_field: 18446744073709551615 }') do
      test_query('pgpb.test.ExampleMessage:scalars.uint64_field', ['18446744073709551615'])
    end
    with_proto('scalars { fixed64_field: 18446744073709551615 }') do
      test_query('pgpb.test.ExampleMessage:scalars.fixed64_field', ['18446744073709551615'])
    end

    with_proto('scalars { float_field: 123.456 }') do
      test_query('pgpb.test.ExampleMessage:scalars.float_field', ['123.456001'])
    end
    with_proto('scalars { double_field: 123.456 }') do
      test_query('pgpb.test.ExampleMessage:scalars.double_field', ['123.456000'])
    end

    with_proto('scalars { bool_field: true }') do
      test_query('pgpb.test.ExampleMessage:scalars.bool_field', ['true'])
    end
    with_proto('scalars { bool_field: false }') do
      # Proto3 doesn't store the false value
      test_query('pgpb.test.ExampleMessage:scalars.bool_field', [])
    end

    with_proto('scalars { string_field: "xyz" }') do
      test_query('pgpb.test.ExampleMessage:scalars.string_field', ['xyz'])
    end
    with_proto('scalars { bytes_field: "xyz" }') do
      test_query('pgpb.test.ExampleMessage:scalars.bytes_field', ['\x78797A'])
    end
  end

  section "Empty results" do
    with_proto('') do
      test_query('pgpb.test.ExampleMessage:scalars.int32_field', [])
    end
    with_proto('scalars { string_field: "xyz" }') do
      test_query('pgpb.test.ExampleMessage:scalars.int32_field', [])
    end
  end

  section "Indexing into repeated fields" do
    with_proto('repeated_int32: 123, repeated_int32: 456') do
      test_query('pgpb.test.ExampleMessage:repeated_int32[0]', ['123'])
      test_query('pgpb.test.ExampleMessage:repeated_int32[1]', ['456'])
      test_query('pgpb.test.ExampleMessage:repeated_int32[2]', [])
      test_query('pgpb.test.ExampleMessage:repeated_int32[*]', ['123', '456'])
    end

    with_proto('repeated_string: "aaa", repeated_string: "bbb", repeated_string: "ccc"') do
      test_query('pgpb.test.ExampleMessage:repeated_string[0]', ['aaa'])
      test_query('pgpb.test.ExampleMessage:repeated_string[1]', ['bbb'])
      test_query('pgpb.test.ExampleMessage:repeated_string[2]', ['ccc'])
      test_query('pgpb.test.ExampleMessage:repeated_string[3]', [])
      test_query('pgpb.test.ExampleMessage:repeated_string[*]', ['aaa', 'bbb', 'ccc'])
    end

    with_proto('repeated_inner: { inner_repeated: "abc", inner_repeated: "def" }, repeated_inner { inner_repeated: "aaa", inner_repeated: "bbb" }') do
      test_query('pgpb.test.ExampleMessage:repeated_inner[0].inner_repeated[*]', ['abc', 'def'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[1].inner_repeated[*]', ['aaa', 'bbb'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[2].inner_repeated[*]', [])
      test_query('pgpb.test.ExampleMessage:repeated_inner[0].inner_repeated[0]', ['abc'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[1].inner_repeated[1]', ['bbb'])

      test_query('pgpb.test.ExampleMessage:repeated_inner[*]', ['{"innerRepeated":["abc","def"]}', '{"innerRepeated":["aaa","bbb"]}'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[*].inner_repeated[*]', ['abc', 'def', 'aaa', 'bbb'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[*].inner_repeated[0]', ['abc', 'aaa'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[*].inner_repeated[1]', ['def', 'bbb'])

      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[*].inner_repeated[0]', #{pg_proto}) AS result;", ['abc'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[*].inner_repeated[1]', #{pg_proto}) AS result;", ['def'])
      test_sql("SELECT COALESCE(protobuf_query('pgpb.test.ExampleMessage:repeated_inner[*].inner_repeated[2]', #{pg_proto}), 'none') AS result;", ['none'])
    end

    with_proto('repeated_inner: { repeated_inner: { inner_repeated: "abc", inner_repeated: "def" }, repeated_inner: { inner_repeated: "aaa" }, repeated_inner: {}, repeated_inner: { inner_repeated: "bbb" } }') do
      test_query('pgpb.test.ExampleMessage:repeated_inner[*].repeated_inner[*]', ['{"innerRepeated":["abc","def"]}', '{"innerRepeated":["aaa"]}', '{}', '{"innerRepeated":["bbb"]}'])
      test_query('pgpb.test.ExampleMessage:repeated_inner[*].repeated_inner[*].inner_repeated[*]', ['abc', 'def', 'aaa', 'bbb'])

      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[*].repeated_inner[*].inner_repeated[0]', #{pg_proto}) AS result;", ['abc'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[*].repeated_inner[*].inner_repeated[1]', #{pg_proto}) AS result;", ['def'])
      test_sql("SELECT COALESCE(protobuf_query('pgpb.test.ExampleMessage:repeated_inner[*].repeated_inner[*].inner_repeated[2]', #{pg_proto}), 'none') AS result;", ['none'])

      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[0].inner_repeated[0]', #{pg_proto}) AS result;", ['abc'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[0].inner_repeated[1]', #{pg_proto}) AS result;", ['def'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[1].inner_repeated[0]', #{pg_proto}) AS result;", ['aaa'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[3].inner_repeated[0]', #{pg_proto}) AS result;", ['bbb'])
    end

    with_proto('repeated_inner: { repeated_inner: { inner_str: "abc" }, repeated_inner: { inner_str: "def" } }, repeated_inner: { repeated_inner: { inner_str: "aaa" }, repeated_inner: { inner_str: "bbb" } }') do
      test_query('pgpb.test.ExampleMessage:repeated_inner[*].repeated_inner[*].inner_str', ['abc', 'def', 'aaa', 'bbb'])

      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[0].inner_str', #{pg_proto}) AS result;", ['abc'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[1].inner_str', #{pg_proto}) AS result;", ['def'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[1].repeated_inner[0].inner_str', #{pg_proto}) AS result;", ['aaa'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[1].repeated_inner[1].inner_str', #{pg_proto}) AS result;", ['bbb'])
    end

    with_proto('repeated_inner: { inner_str: "lvl1", repeated_inner: { inner_str: "lvl2" } }') do
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].inner_str', #{pg_proto}) AS result;", ['lvl1'])
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_inner[0].repeated_inner[0].inner_str', #{pg_proto}) AS result;", ['lvl2'])
    end
  end

  section "Indexing into maps" do
    with_proto('map_str2str: { key: "a", value: "AAA" }, map_str2str { key: "bb", value: "BBB" }') do
      test_query('pgpb.test.ExampleMessage:map_str2str[a]', ['AAA'])
      test_query('pgpb.test.ExampleMessage:map_str2str[bb]', ['BBB'])
      test_query('pgpb.test.ExampleMessage:map_str2str[c]', [])
      test_query('pgpb.test.ExampleMessage:map_str2str[*]', ['AAA', 'BBB'])
    end

    with_proto('map_int2str: { key: 123, value: "AAA" }, map_int2str { key: 456, value: "BBB" }') do
      test_query('pgpb.test.ExampleMessage:map_int2str[123]', ['AAA'])
      test_query('pgpb.test.ExampleMessage:map_int2str[456]', ['BBB'])
      test_query('pgpb.test.ExampleMessage:map_int2str[789]', [])
      test_query('pgpb.test.ExampleMessage:map_int2str[*]', ['AAA', 'BBB'])
    end

    with_proto('map_int2int: { key: 123, value: 100 }, map_int2int { key: 456, value: 200 }') do
      test_query('pgpb.test.ExampleMessage:map_int2int[123]', ['100'])
      test_query('pgpb.test.ExampleMessage:map_int2int[456]', ['200'])
      test_query('pgpb.test.ExampleMessage:map_int2int[789]', [])
      test_query('pgpb.test.ExampleMessage:map_int2int[*]', ['100', '200'])
    end

    with_proto('map_str2inner: { key: "x", value: { inner_str: "A" } }, map_str2inner { key: "y", value: { inner_str: "B" } }') do
      test_query('pgpb.test.ExampleMessage:map_str2inner[x].inner_str', ['A'])
      test_query('pgpb.test.ExampleMessage:map_str2inner[y].inner_str', ['B'])
      test_query('pgpb.test.ExampleMessage:map_str2inner[z].inner_str', [])
      test_query('pgpb.test.ExampleMessage:map_str2inner[*].inner_str', ['A', 'B'])
    end
  end

  section "Selecting all map keys" do
    with_proto('map_int2str: { key: 123, value: "AAA" }, map_int2str { key: 456, value: "BBB" }') do
      test_query('pgpb.test.ExampleMessage:map_int2str|keys', ['123', '456'])
    end
  end

  section "Queries from explicitly specified file descriptor sets" do
    with_proto('scalars { int32_field: 123 }') do
      test_query('default:pgpb.test.ExampleMessage:scalars.int32_field', ['123'])
    end
    with_proto('int32_field: 123', proto_file='other_descriptor_set.proto', proto_name='pgpb.test.other.MessageInOtherDescSet') do
      test_query('other:pgpb.test.other.MessageInOtherDescSet:int32_field', ['123'])
    end
  end

  section "Single result queries" do 
    with_proto('repeated_int32: 123, repeated_int32: 456') do
      # Should return just the first result
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:repeated_int32[*]', #{pg_proto}) AS result;", ['123'])
    end
  end

  section "Enums" do
    with_proto('an_enum: EnumValue2') do
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:an_enum', #{pg_proto}) AS result;", ['EnumValue2'])
    end
  end

  section "Empty query" do
    # protobuf_to_json() is equivalent to a query that specifies just the proto name
    with_proto('scalars { int32_field: 123 }') do
      test_sql("SELECT protobuf_query('pgpb.test.ExampleMessage:', #{pg_proto}) AS result;", ['{"scalars":{"int32Field":123}}'])
    end
  end

  section "Array queries" do
    with_proto('repeated_int32: 123, repeated_int32: 456') do
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:repeated_int32[*]', #{pg_proto}) AS result;", ['{123,456}'])
    end
    with_proto('map_int2str: { key: 123, value: "AAA" }, map_int2str { key: 456, value: "BBB" }') do
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:map_int2str[*]', #{pg_proto}) AS result;", ['{AAA,BBB}'])
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:map_int2str|keys', #{pg_proto}) AS result;", ['{123,456}'])
    end
    with_proto('scalars { int32_field: 123 }') do
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:', #{pg_proto}) AS result;", ['{"{\"scalars\":{\"int32Field\":123}}"}'])
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:scalars', #{pg_proto}) AS result;", ['{"{\"int32Field\":123}"}'])
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:scalars.int32_field', #{pg_proto}) AS result;", ['{123}'])
      test_sql("SELECT protobuf_query_array('pgpb.test.ExampleMessage:scalars.string_field', #{pg_proto}) AS result;", ['{}'])
    end
  end

  section "Converting to JSON" do
    with_proto('scalars { int32_field: 123 }') do
      test_sql("SELECT protobuf_to_json_text('pgpb.test.ExampleMessage', #{pg_proto}) AS result;", ['{"scalars":{"int32Field":123}}'])
    end
    with_proto('int32_field: 123', proto_file='other_descriptor_set.proto', proto_name='pgpb.test.other.MessageInOtherDescSet') do
      test_sql("SELECT protobuf_to_json_text('other:pgpb.test.other.MessageInOtherDescSet', #{pg_proto}) AS result;", ['{"int32Field":123}'])
    end
  end

  section "Converting from JSON" do
    with_proto('scalars { int32_field: 123 }') do
      json = pg_quote('{"scalars":{"int32Field":123}}')
      test_sql("SELECT protobuf_from_json_text('pgpb.test.ExampleMessage', #{json}) AS result;", [pg_proto_raw])
    end
    with_proto('int32_field: 123', proto_file='other_descriptor_set.proto', proto_name='pgpb.test.other.MessageInOtherDescSet') do
      json = pg_quote('{"int32Field":123}')
      test_sql("SELECT protobuf_from_json_text('other:pgpb.test.other.MessageInOtherDescSet', #{json}) AS result;", [pg_proto_raw])
    end
  end
end
