#include "querying.hpp"

#include "descriptor_db.hpp"
#include "postgres_protobuf_common.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Protobuf headers must be included before any Postgres headers because
// the latter pollute names like 'FATAL' used by macros in the former.
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/wire_format.h>
#include <google/protobuf/wire_format_lite.h>

extern "C" {
// Must be included before other Postgres headers
#include <postgres.h>
#include <common/shortest_dec.h>
}

namespace pb = ::google::protobuf;

namespace postgres_protobuf {
namespace querying {

namespace {

struct FieldInfo {
  int number;
  uint32 wire_type;
  union Value {
    uint32 as_uint32;
    uint64 as_uint64;
    int as_size;
  } value;

  bool ValueEquals(const FieldInfo& that) {
    if (this->wire_type != that.wire_type) {
      return false;
    }
    switch (wire_type) {
      case 0:
      case 1:
        return this->value.as_uint64 == that.value.as_uint64;
      case 2:
        return this->value.as_size == that.value.as_size;
      case 5:
        return this->value.as_uint32 == that.value.as_uint32;
      default:
        return false;
    }
  }

  std::string ToString() const {
    std::stringstream ss;
    ss << "{num=" << number << ",wt=" << wire_type;
    switch (wire_type) {
      case 0:
      case 1:
        ss << ",as_uint64=0x" << std::hex << value.as_uint64;
        break;
      case 2:
        ss << ",as_size=" << value.as_size;
        break;
      case 5:
        ss << ",as_uint32=0x" << std::hex << value.as_uint32;
        break;
        break;
      default:
        break;
    }
    ss << "}";
    return ss.str();
  }
};

enum class LengthDelimitedFieldTreatment {
  Skip,
  Buffer,
  AsString,
  AsBytes,
  AsSubmessage,
  AsPackedVarint,
  AsPacked32,
  AsPacked64
};

struct DescPtrs {
  pb::FieldDescriptor::Type ty;  // Type of thing being pointed at
  const pb::Descriptor* desc;    // Message descriptor when ty == TYPE_MESSAGE
  const pb::EnumDescriptor* enum_desc;  // Enum descriptor when ty == TYPE_ENUM
  bool is_repeated;
  bool is_map;
};

// Thrown as an exception for convenience.
// TODO: this goes against some best practices. Is this worth changing?
class LimitReached {};

class ProtobufTraverser;

class ProtobufVisitor {
 public:
  ProtobufVisitor() : next_(&ProtobufVisitor::noOp) {}
  virtual ~ProtobufVisitor() {}

  void SetNext(ProtobufVisitor* next) { next_ = next; }

  virtual void Pushed(ProtobufTraverser* traverser) {}

  virtual ProtobufVisitor* BeginField(int number, int wire_type) {
    return this;
  }

  virtual void ReadPrimitive(const FieldInfo& field) {}

  virtual std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) {
    return std::make_pair(LengthDelimitedFieldTreatment::Skip, this);
  }

  virtual void ReadString(std::string&& str) {}
  virtual void ReadBytes(std::string&& bytes) {}

  virtual void BufferedValue(std::string&& value) {}

  virtual ProtobufVisitor* BeginMessage() { return this; }

  virtual void EndField() {}

  virtual void Popped() {}

  static ProtobufVisitor noOp;

 protected:
  ProtobufVisitor* next_;

  LengthDelimitedFieldTreatment CompositeFieldTreatmentForType(
      pb::FieldDescriptor::Type ty) {
    switch (ty) {
      case pb::FieldDescriptor::Type::TYPE_MESSAGE:
        return LengthDelimitedFieldTreatment::AsSubmessage;
      case pb::FieldDescriptor::Type::TYPE_STRING:
        return LengthDelimitedFieldTreatment::AsString;
      case pb::FieldDescriptor::Type::TYPE_BYTES:
        return LengthDelimitedFieldTreatment::AsBytes;
      default:
        return LengthDelimitedFieldTreatment::Skip;
    }
  }

  LengthDelimitedFieldTreatment PackedCompositeFieldTreatmentForType(
      pb::FieldDescriptor::Type ty) {
    int wire_type = pb::internal::WireFormat::WireTypeForFieldType(ty);
    switch (wire_type) {
      case 0:
        return LengthDelimitedFieldTreatment::AsPackedVarint;
      case 1:
        return LengthDelimitedFieldTreatment::AsPacked64;
      case 5:
        return LengthDelimitedFieldTreatment::AsPacked32;
      default:
        return LengthDelimitedFieldTreatment::Skip;
    }
  }
};

ProtobufVisitor ProtobufVisitor::noOp;

class ProtobufTraverser {
 public:
  ProtobufTraverser() : visitor_(&ProtobufVisitor::noOp), depth_(0) {}

  void PushVisitor(ProtobufVisitor* v) {
    PGPROTO_DEBUG("PUSH %lx", intptr_t(v));
    assert(v != nullptr);
    visitor_stack_.push_back(StackElement(v, depth_));
    visitor_ = v;
    v->Pushed(this);
  }

  void PopVisitor() {
    PGPROTO_DEBUG("POP");
    if (!visitor_stack_.empty()) {
      ProtobufVisitor* popped = visitor_stack_.back().visitor;
      visitor_stack_.pop_back();
      popped->Popped();
    }
    if (!visitor_stack_.empty()) {
      visitor_ = visitor_stack_.back().visitor;
    } else {
      visitor_ = &ProtobufVisitor::noOp;
    }
  }

  void ScanField(const FieldInfo& field, pb::io::CodedInputStream* stream) {
    if (field.wire_type != 2) {
      visitor_->ReadPrimitive(field);
      return;
    }

    LengthDelimitedFieldTreatment treatment;
    ProtobufVisitor* new_visitor;
    std::tie(treatment, new_visitor) =
        visitor_->ReadLengthDelimitedField(field);
    PGPROTO_DEBUG("ReadLengthDelimitedField returned %d for visitor %lx",
                  static_cast<int>(treatment), intptr_t(visitor_));
    bool got_new_visitor = new_visitor != visitor_;
    if (got_new_visitor) {
      PushVisitor(new_visitor);
      IncrementDepthAndCallBeginField(field.number, field.wire_type);
    }

    switch (treatment) {
      case LengthDelimitedFieldTreatment::Skip: {
        stream->Skip(field.value.as_size);
        break;
      }
      case LengthDelimitedFieldTreatment::Buffer: {
        std::string s;
        if (!stream->ReadString(&s, field.value.as_size)) {
          throw BadProto("failed to fully read length-delimited field");
        }
        visitor_->BufferedValue(std::move(s));
        break;
      }
      case LengthDelimitedFieldTreatment::AsString: {
        std::string s;
        if (!stream->ReadString(&s, field.value.as_size)) {
          throw BadProto("failed to fully read string field");
        }
        visitor_->ReadString(std::move(s));
        break;
      }
      case LengthDelimitedFieldTreatment::AsBytes: {
        std::string s;
        if (!stream->ReadString(&s, field.value.as_size)) {
          throw BadProto("failed to fully read bytes field");
        }
        visitor_->ReadBytes(std::move(s));
        break;
      }
      case LengthDelimitedFieldTreatment::AsSubmessage: {
        auto depth_and_limit =
            stream->IncrementRecursionDepthAndPushLimit(field.value.as_size);
        if (depth_and_limit.second < 0) {
          throw RecursionDepthExceeded();
        }
        visitor_->BeginMessage();
        // TODO: avoid actual recursion in case our stack is small
        ScanMessage(stream);
        stream->DecrementRecursionDepthAndPopLimit(depth_and_limit.first);
        break;
      }
      case LengthDelimitedFieldTreatment::AsPackedVarint: {
        ReadPacked(stream, field.number, field.value.as_size, 0);
        break;
      }
      case LengthDelimitedFieldTreatment::AsPacked32: {
        ReadPacked(stream, field.number, field.value.as_size, 5);
        break;
      }
      case LengthDelimitedFieldTreatment::AsPacked64: {
        ReadPacked(stream, field.number, field.value.as_size, 1);
        break;
      }
    }

    if (got_new_visitor) {
      DecrementDepthAndEndFieldAndPopVisitors();
    }
  }

 private:
  struct StackElement {
    ProtobufVisitor* visitor;
    int depth_added;
    StackElement(ProtobufVisitor* visitor, int depth_added)
        : visitor(visitor), depth_added(depth_added) {}
  };
  std::vector<StackElement> visitor_stack_;

  ProtobufVisitor* visitor_;
  int depth_;

  void ScanMessage(pb::io::CodedInputStream* stream) {
    CallBeginMessage();

    while (true) {
      uint32 tag = stream->ReadTag();
      if (tag == 0) {
        if (!stream->ConsumedEntireMessage()) {
          throw BadProto("Unexpected tag=0");
        }
        return;
      }

      FieldInfo field;
      field.number = tag >> 3;
      field.wire_type = tag & 0x7;

      ReadFieldValueOrSize(stream, &field);

      IncrementDepthAndCallBeginField(field.number, field.wire_type);

      ScanField(field, stream);

      DecrementDepthAndEndFieldAndPopVisitors();
    }
  }

  void IncrementDepthAndCallBeginField(int field_number, int wire_type) {
    ++depth_;
    while (true) {
      PGPROTO_DEBUG("BeginField %d on visitor %lx", field_number,
                    intptr_t(visitor_));
      ProtobufVisitor* new_visitor =
          visitor_->BeginField(field_number, wire_type);
      if (new_visitor != visitor_) {
        PushVisitor(new_visitor);
      } else {
        break;
      }
    }
  }

  void CallBeginMessage() {
    while (true) {
      PGPROTO_DEBUG("BeginMessage on visitor %lx", intptr_t(visitor_));
      ProtobufVisitor* new_visitor = visitor_->BeginMessage();
      if (new_visitor != visitor_) {
        PushVisitor(new_visitor);
      } else {
        break;
      }
    }
  }

  void DecrementDepthAndEndFieldAndPopVisitors() {
    --depth_;
    visitor_->EndField();
    while (!visitor_stack_.empty() &&
           visitor_stack_.back().depth_added > depth_) {
      PopVisitor();
      visitor_->EndField();
    }
  }

  void ReadPacked(pb::io::CodedInputStream* stream, int number, int size,
                  int wire_type) {
    FieldInfo f;
    f.number = number;
    f.wire_type = wire_type;
    auto limit = stream->PushLimit(size);
    while (stream->BytesUntilLimit() > 0) {
      ReadFieldValueOrSize(stream, &f);
      IncrementDepthAndCallBeginField(f.number, f.wire_type);
      visitor_->ReadPrimitive(f);
      DecrementDepthAndEndFieldAndPopVisitors();
    }
    stream->PopLimit(limit);
  }

  void ReadFieldValueOrSize(pb::io::CodedInputStream* stream,
                            FieldInfo* field) {
    switch (field->wire_type) {
      case 0:  // varint
        if (!stream->ReadVarint64(&field->value.as_uint64)) {
          throw BadProto("failed to read varint field");
        }
        break;
      case 1:  // 64-bit
        if (!stream->ReadLittleEndian64(&field->value.as_uint64)) {
          throw BadProto("failed to read 64-bit field");
        }
        break;
      case 2:  // length-delimited
        if (!stream->ReadVarintSizeAsInt(&field->value.as_size)) {
          throw BadProto("failed to read size varint");
        }
        break;
      // We don't support wire types 3 and 4 (groups)
      case 5:  // 32-bit
        if (!stream->ReadLittleEndian32(&field->value.as_uint32)) {
          throw BadProto("failed to read 32-bit field");
        }
        break;
      default:
        throw BadProto(std::string("unrecognized wire_type ") +
                       std::to_string(field->wire_type));
    }
  }
};

class Emitter;
class PrimitiveEmitter;
class EnumEmitter;
class MessageEmitter;

class Emitter : public ProtobufVisitor {
 public:
  static std::unique_ptr<Emitter> Create(const DescPtrs& desc_ptrs,
                                         pb::util::TypeResolver* type_resolver,
                                         std::optional<uint64_t> limit);

  std::vector<std::string> rows;

 protected:
  Emitter(pb::FieldDescriptor::Type ty, std::optional<uint64_t> limit)
      : ty_(ty) {}

  const pb::FieldDescriptor::Type ty_;
  const std::string type_url_;
  const std::optional<uint64_t> limit_;

  template <typename T>
  void Emit(const T& value) {
    EmitStr(std::move(std::to_string(value)));
  }

  void EmitStr(std::string&& str) {
    PGPROTO_DEBUG("EmitStr(%s)", str.c_str());
    rows.push_back(str);
    if (limit_ && rows.size() >= *limit_) {
      PGPROTO_DEBUG("Result limit reached");
      throw LimitReached();
    }
  }
};

class PrimitiveEmitter : public Emitter {
 public:
  PrimitiveEmitter(pb::FieldDescriptor::Type ty, std::optional<uint64_t> limit)
      : Emitter(ty, limit) {
    PGPROTO_DEBUG("Created primitive emitter %d %lx", static_cast<int>(ty_),
                  intptr_t(this));
  }

  using T = pb::FieldDescriptor::Type;
  using WFL = pb::internal::WireFormatLite;

  std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) override {
    return std::make_pair(CompositeFieldTreatmentForType(ty_), this);
  }

  void ReadPrimitive(const FieldInfo& field) override {
#ifndef PROTOBUF_LITTLE_ENDIAN
    static_assert(false, "big-endian not yet supported");
#endif
    PGPROTO_DEBUG("Emit primitive %d (wt %d, ty %d)", field.number,
                  field.wire_type, ty_);
    switch (ty_) {
      case T::TYPE_DOUBLE:
        {
          char *cstr = double_to_shortest_decimal(WFL::DecodeDouble(field.value.as_uint64));
          EmitStr(std::move(std::string(cstr)));
        }
        break;
      case T::TYPE_FLOAT:
        {
          char *cstr = float_to_shortest_decimal(WFL::DecodeFloat(field.value.as_uint32));
          EmitStr(std::move(std::string(cstr)));
        }
        break;
      case T::TYPE_INT64:
      case T::TYPE_SFIXED64:
        Emit(static_cast<int64_t>(field.value.as_uint64));
        break;
      case T::TYPE_UINT64:
      case T::TYPE_FIXED64:
        Emit(field.value.as_uint64);
        break;
      case T::TYPE_INT32:
      case T::TYPE_SFIXED32:
        Emit(static_cast<int32_t>(field.value.as_uint32));
        break;
      case T::TYPE_FIXED32:
      case T::TYPE_UINT32:
        Emit(field.value.as_uint32);
        break;
      case T::TYPE_BOOL:
        EmitStr(std::string(field.value.as_uint64 != 0 ? "true" : "false"));
        break;
      case T::TYPE_SINT32:
        Emit(WFL::ZigZagDecode32(field.value.as_uint32));
        break;
      case T::TYPE_SINT64:
        Emit(WFL::ZigZagDecode64(field.value.as_uint64));
        break;
      default:
        throw BadProto(std::string("unrecognized primitive field type: ") +
                       std::to_string(ty_));
    }
  }

  void ReadString(std::string&& s) override { EmitStr(std::move(s)); }

  void ReadBytes(std::string&& s) override {
    std::stringstream ss;
    ss << "\\x";
    ss << std::hex << std::setfill('0') << std::uppercase;
    for (char c : s) {
      ss << std::setw(2) << static_cast<unsigned int>(c);
    }

    EmitStr(ss.str());
  }
};

class EnumEmitter : public Emitter {
 public:
  EnumEmitter(const pb::EnumDescriptor* ed, std::optional<uint64_t> limit)
      : Emitter(pb::FieldDescriptor::Type::TYPE_ENUM, limit), ed_(ed) {
    PGPROTO_DEBUG("Created enum emitter %s %lx", ed_->full_name().c_str(),
                  intptr_t(this));
  }

  void ReadPrimitive(const FieldInfo& field) override {
    uint64_t n = field.value.as_uint64;
    const pb::EnumValueDescriptor* vd = ed_->FindValueByNumber(n);
    if (vd != nullptr) {
      EmitStr(std::string(vd->name()));
    } else {
      return Emit(n);
    }
  }

 private:
  const pb::EnumDescriptor* ed_;
};

class MessageEmitter : public Emitter {
 public:
  MessageEmitter(pb::util::TypeResolver* type_resolver, std::string&& type_url,
                 std::optional<uint64_t> limit)
      : Emitter(pb::FieldDescriptor::Type::TYPE_MESSAGE, limit),
        type_resolver_(type_resolver),
        type_url_(type_url) {
    PGPROTO_DEBUG("Created message emitter %d %lx", static_cast<int>(ty_),
                  intptr_t(this));
  }

  std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) override {
    return std::make_pair(LengthDelimitedFieldTreatment::Buffer, this);
  }

  void BufferedValue(std::string&& s) override {
    std::string json;
    if (type_url_.empty()) {
      throw BadQuery("result type not known");  // Should not happen
    }
    PGPROTO_DEBUG("Converting %lu bytes to JSON: %s", s.size(),
                  type_url_.c_str());
    if (!pb::util::BinaryToJsonString(type_resolver_, type_url_, s, &json)
             .ok()) {
      throw BadProto("failed to convert submessage to JSON");
    }

    EmitStr(std::move(json));
  }

 private:
  pb::util::TypeResolver* const type_resolver_;
  const std::string type_url_;
};

std::unique_ptr<Emitter> Emitter::Create(const DescPtrs& desc_ptrs,
                                         pb::util::TypeResolver* type_resolver,
                                         std::optional<uint64_t> limit) {
  if (desc_ptrs.ty == pb::FieldDescriptor::Type::TYPE_MESSAGE) {
    assert(desc_ptrs.desc != nullptr);
    std::string type_url;
    type_url += "type.googleapis.com/";
    type_url += desc_ptrs.desc->full_name();
    return std::unique_ptr<Emitter>(
        new MessageEmitter(type_resolver, std::move(type_url), limit));
  } else if (desc_ptrs.ty == pb::FieldDescriptor::Type::TYPE_ENUM) {
    assert(desc_ptrs.enum_desc != nullptr);
    return std::unique_ptr<Emitter>(
        new EnumEmitter(desc_ptrs.enum_desc, limit));
  } else {
    return std::unique_ptr<Emitter>(new PrimitiveEmitter(desc_ptrs.ty, limit));
  }
}

class DescendIntoSubmessage : public ProtobufVisitor {
 public:
  DescendIntoSubmessage() {
    PGPROTO_DEBUG("Created descend-into-submessage %lx", intptr_t(this));
  }

  std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) override {
    return std::make_pair(LengthDelimitedFieldTreatment::AsSubmessage, this);
  }

  ProtobufVisitor* BeginMessage() override { return next_; }
};

class FieldSelector : public ProtobufVisitor {
 public:
  FieldSelector(int wanted_field, pb::FieldDescriptor::Type ty, bool is_packed)
      : traverser_(nullptr),
        wanted_field_(wanted_field),
        ty_(ty),
        is_packed_(is_packed),
        wanted_index_(),
        state_(State::Scanning),
        current_field_(0),
        current_index_(0) {
    PGPROTO_DEBUG("Created field selector %d %lx", wanted_field,
                  intptr_t(this));
  }

  void SetWantedIndex(int wanted_index) { wanted_index_ = wanted_index; }

  void Pushed(ProtobufTraverser* traverser) override { traverser_ = traverser; }

  ProtobufVisitor* BeginField(int number, int wire_type) override {
    current_field_ = number;
    if (wire_type == 2) {
      if (is_packed_) {
        state_ = State::EmittingPacked;
      } else {
        if (ShouldEmitCurrentIndex()) {
          if (ty_ == pb::FieldDescriptor::Type::TYPE_MESSAGE) {
            return next_;
          } else {
            state_ = State::EmittingOtherComposite;
          }
        }
      }
    } else if (ShouldEmitCurrentIndex()) {
      return next_;
    }
    return this;
  }

  std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) override {
    if (state_ == State::EmittingPacked) {
      return std::make_pair(PackedCompositeFieldTreatmentForType(ty_), this);
    } else if (ShouldEmitCurrentIndex()) {
      if (state_ == State::EmittingOtherComposite) {
        return std::make_pair(CompositeFieldTreatmentForType(ty_), next_);
      } else {
        return std::make_pair(CompositeFieldTreatmentForType(ty_), this);
      }
    } else {
      return std::make_pair(LengthDelimitedFieldTreatment::Skip, this);
    }
  }

  void EndField() override {
    if (current_field_ == wanted_field_) {
      ++current_index_;
    }
  }

  void Popped() override {
    state_ = State::Scanning;
    current_field_ = 0;
    current_index_ = 0;
  }

 private:
  ProtobufTraverser* traverser_;
  const int wanted_field_;
  const pb::FieldDescriptor::Type ty_;
  const bool is_packed_;
  std::optional<int> wanted_index_;

  enum class State {
    Scanning,
    EmittingPacked,
    EmittingOtherComposite,
  };
  State state_;
  int current_field_;
  int current_index_;

  bool ShouldEmitCurrentIndex() const {
    return current_field_ == wanted_field_ &&
           (!wanted_index_.has_value() || current_index_ == wanted_index_.value());
  }
};

class MapFilter : public ProtobufVisitor {
 public:
  MapFilter(const FieldInfo& wanted_key_field,
            const std::string& wanted_key_contents,
            pb::FieldDescriptor::Type value_type)
      : wanted_key_field_(wanted_key_field),
        wanted_key_contents_(wanted_key_contents),
        value_type_(value_type),
        scope_(Scope::Outermost) {
    PGPROTO_DEBUG("Created map filter %d %s %lx", wanted_key_field.wire_type,
                  wanted_key_contents.c_str(), intptr_t(this));
  }

  ProtobufVisitor* BeginField(int number, int wire_type) override {
    if (wire_type == 2 && scope_ == Scope::Outermost) {
      PGPROTO_DEBUG("Map in entry");
      scope_ = Scope::InEntry;
    } else if (scope_ == Scope::InEntry) {
      // There is (I think) no guarantee that keys come before values
      if (number == 1) {
        PGPROTO_DEBUG("Map in key");
        scope_ = Scope::InKey;
      } else if (number == 2) {
        PGPROTO_DEBUG("Map in value");
        scope_ = Scope::InValue;
      }
    }
    return this;
  }

  // TODO: unnecessary?
  ProtobufVisitor* BeginMessage() override {
    if (scope_ == Scope::Outermost) {
      scope_ = Scope::InEntry;
    }
    return this;
  }

  void ReadPrimitive(const FieldInfo& field) override {
    switch (scope_) {
      case Scope::InKey:
        buffered_key_field_ = field;
        break;
      case Scope::InValue:
        buffered_value_field_ = field;
        break;
      default:
        break;
    }
  }

  std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) override {
    switch (scope_) {
      case Scope::InEntry:
        return std::make_pair(LengthDelimitedFieldTreatment::AsSubmessage,
                              this);
      case Scope::InKey:
        buffered_key_field_ = field;
        return std::make_pair(LengthDelimitedFieldTreatment::Buffer, this);
      case Scope::InValue:
        buffered_value_field_ = field;
        return std::make_pair(LengthDelimitedFieldTreatment::Buffer, this);
      default:
        return std::make_pair(LengthDelimitedFieldTreatment::Skip, this);
    }
  }

  // TODO: instead of buffering the value, record its position in the stream and
  // reread it
  void BufferedValue(std::string&& value) override {
    switch (scope_) {
      case Scope::InKey:
        PGPROTO_DEBUG("Map buffered key (%lu bytes)", value.size());
        buffered_key_contents_ = value;
        break;
      case Scope::InValue:
        PGPROTO_DEBUG("Map buffered value (%lu bytes)", value.size());
        buffered_value_contents_ = value;
        break;
      default:
        break;
    }
  }

  void EndField() override {
    PGPROTO_DEBUG("Map field end");
    bool entry_ended = scope_ == Scope::InEntry;
    switch (scope_) {
      case Scope::InKey:
      case Scope::InValue:
        scope_ = Scope::InEntry;
        break;
      default:
        scope_ = Scope::Outermost;
        break;
    }

    if (entry_ended) {
      if (buffered_key_field_.ValueEquals(wanted_key_field_) &&
          buffered_key_contents_ == wanted_key_contents_) {
        PGPROTO_DEBUG("Map entry matched.");
        ForwardBufferedValue();
      } else {
        PGPROTO_DEBUG("Map entry did not match. Key: %s, wanted %s %s",
                      buffered_key_field_.ToString().c_str(),
                      wanted_key_field_.ToString().c_str(),
                      buffered_key_contents_ == wanted_key_contents_
                          ? "(contents matched)"
                          : "(contents did not match)");
      }

      Reset();
    }
  }

  void Popped() override { Reset(); }

 private:
  const FieldInfo wanted_key_field_;
  const std::string wanted_key_contents_;
  pb::FieldDescriptor::Type value_type_;

  enum class Scope {
    Outermost,
    InEntry,
    InKey,
    InValue,
  };
  Scope scope_;

  FieldInfo buffered_key_field_;
  std::string buffered_key_contents_;
  FieldInfo buffered_value_field_;
  std::string buffered_value_contents_;

  void ForwardBufferedValue() {
    pb::io::CodedInputStream substream(
        reinterpret_cast<uint8*>(buffered_value_contents_.data()),
        buffered_value_contents_.size());
    ProtobufTraverser subtraverser;
    subtraverser.PushVisitor(next_);
    subtraverser.ScanField(buffered_value_field_, &substream);
    subtraverser.PopVisitor();
  }

  void Reset() {
    scope_ = Scope::Outermost;
    std::memset(&buffered_key_field_, 0, sizeof(buffered_key_field_));
    std::memset(&buffered_value_field_, 0, sizeof(buffered_value_field_));
    buffered_key_contents_.clear();
    buffered_value_contents_.clear();
  }
};

class AllMapEntries : public ProtobufVisitor {
 public:
  AllMapEntries(bool want_keys, pb::FieldDescriptor::Type ty)
      : want_keys_(want_keys), ty_(ty), scope_(Scope::Outermost) {
    PGPROTO_DEBUG("Created all-map-entries %s %lx",
                  want_keys ? "(keys)" : "(values)", intptr_t(this));
  }

  ProtobufVisitor* BeginField(int number, int wire_type) override {
    switch (scope_) {
      case Scope::Outermost:
        scope_ = Scope::InEntry;
        return this;
      case Scope::InEntry:
        if (((number == 1 && want_keys_) || (number == 2 && !want_keys_))) {
          scope_ = Scope::InWantedField;
          return next_;
        } else {
          scope_ = Scope::InUnwantedOtherField;
          return this;
        }
      default:
        PGPROTO_DEBUG("AllMapEntries: unexpected BeginField (num=%d, wt=%d)",
                      number, wire_type);
        return this;
    }
  }

  std::pair<LengthDelimitedFieldTreatment, ProtobufVisitor*>
  ReadLengthDelimitedField(const FieldInfo& field) override {
    switch (scope_) {
      case Scope::InEntry:
        return std::make_pair(LengthDelimitedFieldTreatment::AsSubmessage,
                              this);
      case Scope::InWantedField:
        return std::make_pair(CompositeFieldTreatmentForType(ty_), next_);
      default:
        return std::make_pair(LengthDelimitedFieldTreatment::Skip, this);
    }
  }

  void EndField() override {
    switch (scope_) {
      case Scope::InWantedField:
      case Scope::InUnwantedOtherField:
        scope_ = Scope::InEntry;
        break;
      default:
        scope_ = Scope::Outermost;
        break;
    }
  }

 private:
  const bool want_keys_;
  const pb::FieldDescriptor::Type ty_;

  enum class Scope {
    Outermost,
    InEntry,
    InWantedField,
    InUnwantedOtherField,
  };
  Scope scope_;
};

}  // namespace

class QueryImpl {
 public:
  QueryImpl(const descriptor_db::DescDb& desc_db, const std::string& query,
            std::optional<uint64_t> limit);
  QueryImpl(const QueryImpl&) = delete;
  void operator=(const QueryImpl&) = delete;

  std::vector<std::string> Run(const std::uint8_t* proto_data,
                               size_t proto_len);

 private:
  std::vector<std::unique_ptr<ProtobufVisitor>> visitors_;
  Emitter* emitter_;
  pb::util::TypeResolver* type_resolver_;

  void CompileQuery(const descriptor_db::DescDb& desc_db,
                    const std::string& query, std::optional<uint64_t> limit);

  static const descriptor_db::DescSet& GetDescSet(
      const descriptor_db::DescDb& desc_db, const std::string& query,
      std::string::size_type* query_start);

  static const pb::Descriptor* GetDesc(const descriptor_db::DescSet& desc_set,
                                       const std::string& query,
                                       std::string::size_type* query_start);

  void CompileQueryPart(const std::string& part, DescPtrs* desc_ptrs);

  static void ParseNumericMapKey(const std::string& s,
                                 pb::FieldDescriptor::Type ty,
                                 FieldInfo::Value* v);
};

Query::Query(const std::string& query, std::optional<uint64_t> limit) {
  std::shared_ptr<descriptor_db::DescDb> desc_db =
      descriptor_db::DescDb::GetOrCreateCached();
  impl_ = new QueryImpl(*desc_db, query, limit);
}

Query::~Query() { delete impl_; }

std::vector<std::string> Query::Run(const std::uint8_t* proto_data,
                                    size_t proto_len) {
  return impl_->Run(proto_data, proto_len);
}

QueryImpl::QueryImpl(const descriptor_db::DescDb& desc_db,
                     const std::string& query, std::optional<uint64_t> limit) {
  CompileQuery(desc_db, query, limit);
  assert(!visitors_.empty());  // There should be at least an Emitter
}

std::vector<std::string> QueryImpl::Run(const std::uint8_t* proto_data,
                                        size_t proto_len) {
  pb::io::CodedInputStream stream(proto_data, proto_len);

  ProtobufTraverser traverser;
  try {
    traverser.PushVisitor(visitors_[0].get());
    FieldInfo fake_root_field;
    fake_root_field.number = 0;
    fake_root_field.wire_type = 2;
    fake_root_field.value.as_size = proto_len;
    traverser.ScanField(fake_root_field, &stream);
    traverser.PopVisitor();
  } catch (const LimitReached&) {
    // early exit
  }

  std::vector<std::string> result = std::move(emitter_->rows);
  emitter_->rows = std::vector<std::string>();
  return result;
}

void QueryImpl::CompileQuery(const descriptor_db::DescDb& desc_db,
                             const std::string& query,
                             std::optional<uint64_t> limit) {
  visitors_.clear();
  emitter_ = nullptr;
  type_resolver_ = nullptr;

  std::string::size_type query_start = 0;

  const descriptor_db::DescSet& desc_set =
      GetDescSet(desc_db, query, &query_start);
  type_resolver_ = desc_set.type_resolver.get();

  DescPtrs desc_ptrs{
      .ty = pb::FieldDescriptor::Type::TYPE_MESSAGE,
      .desc = GetDesc(desc_set, query, &query_start),
      .enum_desc = nullptr,
      .is_repeated = false,
      .is_map = false,
  };
  assert(desc_ptrs.desc != nullptr);

  visitors_.clear();
  if (query_start < query.size()) {
    // Descend into root message unless the query is empty, meaning we want to
    // emit the whole message.
    visitors_.push_back(std::make_unique<DescendIntoSubmessage>());
  }

  std::string part_buf;
  part_buf.reserve(query.size());
  for (std::string::size_type i = query_start; i < query.size(); ++i) {
    char ch = query[i];
    if (ch == '.') {
      CompileQueryPart(part_buf, &desc_ptrs);
      PGPROTO_DEBUG(
          "Query part compiled: visitors=%lu, desc=%s, ty=%d",
          visitors_.size() - 1,
          desc_ptrs.desc ? desc_ptrs.desc->full_name().c_str() : "NULL",
          desc_ptrs.ty);
      part_buf.clear();

      visitors_.push_back(std::make_unique<DescendIntoSubmessage>());
    } else {
      part_buf.push_back(ch);
    }
  }

  if (!part_buf.empty()) {
    CompileQueryPart(part_buf, &desc_ptrs);
    PGPROTO_DEBUG("Final query part compiled: visitors=%lu, desc=%s, ty=%d",
                  visitors_.size() - 1,
                  desc_ptrs.desc ? desc_ptrs.desc->full_name().c_str() : "NULL",
                  desc_ptrs.ty);
    part_buf.clear();
  }

  std::unique_ptr<Emitter> emitter_holder(
      Emitter::Create(desc_ptrs, type_resolver_, limit));
  emitter_ = emitter_holder.get();
  visitors_.push_back(std::move(emitter_holder));

  for (size_t i = 0; i < visitors_.size() - 1; ++i) {
    visitors_[i]->SetNext(visitors_[i + 1].get());
  }
}

const descriptor_db::DescSet& QueryImpl::GetDescSet(
    const descriptor_db::DescDb& desc_db, const std::string& query,
    std::string::size_type* query_start) {
  std::string::size_type i = query.find(':');

  std::string desc_set_name;
  if (i != std::string::npos && query.find(':', i + 1) != std::string::npos) {
    desc_set_name = query.substr(0, i);
    *query_start = i + 1;
  } else {
    desc_set_name = "default";
    *query_start = 0;
  }

  auto di = desc_db.desc_sets.find(desc_set_name);
  if (di == desc_db.desc_sets.end()) {
    throw BadQuery(std::string("descriptor set not found: ") +
                   desc_set_name.c_str());
  }
  return *di->second;
}

const pb::Descriptor* QueryImpl::GetDesc(const descriptor_db::DescSet& desc_set,
                                         const std::string& query,
                                         std::string::size_type* query_start) {
  std::string::size_type i = query.find(':', *query_start);
  if (i == std::string::npos) {
    throw BadQuery(
        "invalid protobuf query - expected: "
        "[<descriptor_set>:]<message_name>:<path>");
  }
  std::string desc_name(query.substr(*query_start, i - *query_start));
  *query_start = i + 1;

  const pb::Descriptor* desc = desc_set.pool->FindMessageTypeByName(desc_name);
  if (desc == nullptr) {
    throw BadQuery(
        "unknown protobuf (did you remember to include the package name?)");
  }
  return desc;
}

void QueryImpl::CompileQueryPart(const std::string& part, DescPtrs* desc_ptrs) {
  if (desc_ptrs->desc == nullptr) {
    throw BadQuery(std::string("query does not refer to a known field: ") +
                   part);
  }

  if (part.empty()) {
    throw BadQuery("unexpected empty query part");
  }

  std::string::size_type bracket = part.find('[');
  std::string::size_type pipe = part.find('|');
  std::string::size_type field_selector_end = std::min(bracket, pipe);
  if (field_selector_end == std::string::npos) {
    field_selector_end = part.size();
  }

  const pb::FieldDescriptor* fd;
  if ('0' <= part[0] && part[0] <= '9') {
    char* end;
    long l = std::strtol(part.c_str(), &end, 10);
    if (end != &part[field_selector_end]) {
      throw BadQuery(std::string("invalid field number in query: ") + part);
    }
    fd = desc_ptrs->desc->FindFieldByNumber(static_cast<int>(l));
  } else {
    fd = desc_ptrs->desc->FindFieldByName(
        std::string(part.substr(0, field_selector_end)));
  }

  if (fd == nullptr) {
    throw BadQuery(std::string("field not found: ") + part + " in " +
                   desc_ptrs->desc->full_name());
  }

  desc_ptrs->is_repeated = fd->is_repeated();
  desc_ptrs->is_map = fd->is_map();

  desc_ptrs->ty = fd->type();
  desc_ptrs->desc = nullptr;
  desc_ptrs->enum_desc = nullptr;
  if (fd->type() == pb::FieldDescriptor::Type::TYPE_MESSAGE) {
    desc_ptrs->desc = fd->message_type();
  } else if (fd->type() == pb::FieldDescriptor::Type::TYPE_ENUM) {
    desc_ptrs->enum_desc = fd->enum_type();
  }

  if (!fd->is_repeated() && field_selector_end != part.size()) {
    throw BadQuery(
        "non-repeated field must not be followed by an array/map selector");
  }

  std::unique_ptr<FieldSelector> field_selector_holder(
      std::make_unique<FieldSelector>(fd->number(), desc_ptrs->ty,
                                      fd->is_packed()));
  FieldSelector* field_selector = field_selector_holder.get();
  visitors_.push_back(std::move(field_selector_holder));

  std::string filter_str = part.substr(field_selector_end);
  if (fd->is_repeated()) {
    bool bracketed = filter_str.size() > 0 && filter_str.at(0) == '[' &&
                     filter_str[filter_str.size() - 1] == ']';
    bool keys_selector = filter_str == "|keys";
    if (!bracketed && !keys_selector) {
      throw BadQuery(std::string(
          "repeated field must be followed by an array/map selector "
          "like '[*]', or '|keys' (for maps)"));
    }

    if (bracketed) {
      filter_str = filter_str.substr(1, filter_str.size() - 2);
      if (filter_str.empty()) {
        throw BadQuery(
            "empty array/map selector '[]' is invalid - did you mean '[*]'?");
      }
    }

    if (keys_selector && !fd->is_map()) {
      throw BadQuery("'|keys' can only be used on maps");
    }

    if (fd->is_map()) {
      assert(desc_ptrs->desc != nullptr);
      const pb::FieldDescriptor* key_field =
          desc_ptrs->desc->FindFieldByNumber(1);
      const pb::FieldDescriptor* value_field =
          desc_ptrs->desc->FindFieldByNumber(2);
      if (key_field == nullptr || value_field == nullptr) {
        throw BadProto("invalid map field");
      }

      if (keys_selector) {
        desc_ptrs->ty = key_field->type();

        visitors_.push_back(std::make_unique<AllMapEntries>(
            true /* want_keys */, desc_ptrs->ty));
      } else {
        desc_ptrs->ty = value_field->type();
        if (value_field->type() == pb::FieldDescriptor::Type::TYPE_MESSAGE) {
          desc_ptrs->desc = value_field->message_type();
        } else {
          desc_ptrs->desc = nullptr;
        }

        if (filter_str == "*") {
          visitors_.push_back(std::make_unique<AllMapEntries>(
              false /* want_keys */, desc_ptrs->ty));
        } else {
          desc_ptrs->ty = value_field->type();
          if (value_field->type() == pb::FieldDescriptor::Type::TYPE_MESSAGE) {
            desc_ptrs->desc = value_field->message_type();
          } else {
            desc_ptrs->desc = nullptr;
          }

          using WFL = pb::internal::WireFormatLite;
          int key_wire_type = static_cast<int>(WFL::WireTypeForFieldType(
              static_cast<WFL::FieldType>(key_field->type())));

          FieldInfo wanted_key_field;
          wanted_key_field.number = 1;
          wanted_key_field.wire_type = key_wire_type;

          std::string wanted_key_contents;
          if (key_field->type() == pb::FieldDescriptor::TYPE_STRING) {
            wanted_key_field.value.as_size = filter_str.size();
            wanted_key_contents = filter_str;
          } else {
            ParseNumericMapKey(filter_str, key_field->type(),
                               &wanted_key_field.value);
          }

          visitors_.push_back(std::make_unique<MapFilter>(
              wanted_key_field, wanted_key_contents, value_field->type()));
        }
      }
    } else {
      assert(fd->is_repeated());

      if (filter_str != "*") {
        size_t end;
        long n;
        try {
          n = std::stol(filter_str.c_str(), &end, 10);
        } catch (const std::invalid_argument& e) {
          throw BadQuery(std::string("invalid numeric key: ") + filter_str);
        } catch (const std::out_of_range& e) {
          throw BadQuery(std::string("numeric key out of range key type: ") +
                         filter_str);
        }
        if (end != filter_str.size()) {
          throw BadQuery(std::string("expected numeric indexer at: ") +
                         filter_str);
        }

        field_selector->SetWantedIndex(static_cast<int>(n));
      }
    }
  }
}

namespace {
template <typename T>
T ParseNum(const std::string& s) {
  size_t end;
  using Tmp =
      typename std::conditional<std::is_unsigned_v<T>, unsigned long long,
                                signed long long>::type;
  Tmp tmp;
  try {
    if constexpr (std::is_unsigned_v<T>) {
      tmp = std::stoull(s, &end);
    } else {
      tmp = std::stoll(s, &end);
    }
  } catch (const std::invalid_argument& e) {
    throw BadQuery(std::string("invalid numeric key: ") + s);
  } catch (const std::out_of_range& e) {
    throw BadQuery(std::string("numeric key out of range key type: ") + s);
  }
  if (end != s.size()) {
    throw BadQuery(std::string("invalid numeric key: ") + s);
  }
  return static_cast<T>(tmp);
}
}  // namespace

void QueryImpl::ParseNumericMapKey(const std::string& s,
                                   pb::FieldDescriptor::Type ty,
                                   FieldInfo::Value* v) {
#ifndef PROTOBUF_LITTLE_ENDIAN
  static_assert(false, "big-endian not yet supported");
#endif
  using T = pb::FieldDescriptor::Type;
  using WFL = pb::internal::WireFormatLite;

  switch (ty) {
    case T::TYPE_INT64:
    case T::TYPE_SFIXED64:
      v->as_uint64 = static_cast<int64_t>(ParseNum<int64_t>(s));
      break;
    case T::TYPE_UINT64:
    case T::TYPE_FIXED64:
      v->as_uint64 = ParseNum<uint64_t>(s);
      break;
    case T::TYPE_INT32:
    case T::TYPE_SFIXED32:
    case T::TYPE_ENUM:
      v->as_uint32 = static_cast<int32_t>(ParseNum<int32_t>(s));
      break;
    case T::TYPE_FIXED32:
    case T::TYPE_UINT32:
      v->as_uint32 = ParseNum<uint32_t>(s);
      break;
    case T::TYPE_BOOL:
      if (s == "true") {
        v->as_uint32 = 1;
      } else if (s == "false") {
        v->as_uint32 = 0;
      } else {
        throw BadQuery(std::string("invalid boolean map key: ") + s);
      }
      break;
    case T::TYPE_SINT32:
      v->as_uint32 = WFL::ZigZagEncode32(ParseNum<int32_t>(s));
      break;
    case T::TYPE_SINT64:
      v->as_uint64 = WFL::ZigZagEncode64(ParseNum<int64_t>(s));
      break;
    default:
      throw BadQuery(std::string("unrecognized numeric key type: ") +
                     std::to_string(ty));
  }
}

}  // namespace querying
}  // namespace postgres_protobuf
