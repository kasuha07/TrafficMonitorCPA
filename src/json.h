#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpa::json {

// 极简 JSON DOM，仅满足插件解析 CPA 响应的需要。字符串保持 UTF-8。
class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    // 对象成员查找；不存在或不是对象时返回 nullptr。
    const Value* Find(std::string_view key) const;
    Value* Find(std::string_view key);
    // 按顺序尝试多个键（兼容 snake_case / camelCase）。
    const Value* FindAny(std::initializer_list<std::string_view> keys) const;

    const std::vector<Value>& Items() const { return array_; }
    const std::vector<std::pair<std::string, Value>>& Members() const { return object_; }

    std::optional<bool> AsBool() const;
    // 数字或可解析为数字的字符串。
    std::optional<double> AsNumber() const;
    // 字符串（去除首尾空白后非空）。
    std::optional<std::string> AsString() const;
    // 原始字符串内容（不去除空白，不复制）；不是字符串时为空。
    std::string_view StringView() const { return type_ == Type::String ? std::string_view(string_) : std::string_view(); }
    // 取走原始字符串内容（不去除空白），避免复制较大的字符串；不是字符串时返回空串。
    std::string TakeString() { return type_ == Type::String ? std::move(string_) : std::string(); }

private:
    friend class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

std::optional<Value> Parse(std::string_view text, std::string* error = nullptr);

// 生成带引号的 JSON 字符串字面量。
std::string Quote(std::string_view s);

} // namespace cpa::json
