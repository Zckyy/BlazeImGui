// Minimal JSON value + parser/writer used for config files. Internal to BlazeImGui.
// Supports objects (insertion-ordered), arrays, numbers, strings, bools and null.
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace blaze::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(double d) : type_(Type::Number), n_(d) {}
    Value(int i) : type_(Type::Number), n_(double(i)) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static Value MakeArray()  { Value v; v.type_ = Type::Array;  return v; }
    static Value MakeObject() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool               asBool(bool def = false) const { return isBool() ? b_ : def; }
    double             asNumber(double def = 0) const { return isNumber() ? n_ : def; }
    const std::string& asString() const { return s_; }

    // Arrays
    std::vector<Value>&       items()       { return arr_; }
    const std::vector<Value>& items() const { return arr_; }
    void push(Value v) { type_ = Type::Array; arr_.push_back(std::move(v)); }

    // Objects
    std::vector<std::pair<std::string, Value>>&       members()       { return obj_; }
    const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }
    const Value* find(const std::string& key) const;
    Value&       operator[](const std::string& key);   // Inserts if missing (turns into object)

    std::string dump(int indent = 2) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

// Returns false and fills `error` on malformed input.
bool Parse(const std::string& text, Value& out, std::string* error = nullptr);
std::string Escape(const std::string& s);

bool ReadFile(const std::wstring& path, std::string& out);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);

std::wstring Widen(const std::string& utf8);
std::string  Narrow(const std::wstring& wide);

} // namespace blaze::json
