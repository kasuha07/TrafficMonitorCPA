#include "json.h"

#include <cmath>
#include <cstdlib>

namespace cpa::json {

namespace {

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> Run(std::string* error)
    {
        Value v;
        SkipWs();
        if (!ParseValue(v, 0)) {
            if (error) *error = error_ + " at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        SkipWs();
        if (pos_ != text_.size()) {
            if (error) *error = "trailing characters at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 128;

    bool Fail(const char* msg)
    {
        error_ = msg;
        return false;
    }

    void SkipWs()
    {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    bool Consume(std::string_view lit)
    {
        if (text_.substr(pos_, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    bool ParseValue(Value& v, int depth)
    {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        if (pos_ >= text_.size()) return Fail("unexpected end");
        char c = text_[pos_];
        switch (c) {
        case '{': return ParseObject(v, depth);
        case '[': return ParseArray(v, depth);
        case '"':
            v.type_ = Value::Type::String;
            return ParseString(v.string_);
        case 't':
            if (!Consume("true")) return Fail("invalid literal");
            v.type_ = Value::Type::Bool;
            v.bool_ = true;
            return true;
        case 'f':
            if (!Consume("false")) return Fail("invalid literal");
            v.type_ = Value::Type::Bool;
            v.bool_ = false;
            return true;
        case 'n':
            if (!Consume("null")) return Fail("invalid literal");
            v.type_ = Value::Type::Null;
            return true;
        default:
            return ParseNumber(v);
        }
    }

    bool ParseObject(Value& v, int depth)
    {
        v.type_ = Value::Type::Object;
        ++pos_; // {
        SkipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            SkipWs();
            Value member;
            if (!ParseValue(member, depth + 1)) return false;
            v.object_.emplace_back(std::move(key), std::move(member));
            SkipWs();
            if (pos_ >= text_.size()) return Fail("unexpected end in object");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; return true; }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& v, int depth)
    {
        v.type_ = Value::Type::Array;
        ++pos_; // [
        SkipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            v.array_.push_back(std::move(item));
            SkipWs();
            if (pos_ >= text_.size()) return Fail("unexpected end in array");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; return true; }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseHex4(unsigned& out)
    {
        if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return Fail("invalid \\u escape");
        }
        return true;
    }

    bool ParseString(std::string& out)
    {
        ++pos_; // opening quote
        for (;;) {
            if (pos_ >= text_.size()) return Fail("unterminated string");
            char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return Fail("unterminated escape");
            char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!ParseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (!Consume("\\u") || !ParseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return Fail("invalid surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return Fail("lone low surrogate");
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                return Fail("invalid escape");
            }
        }
    }

    bool ParseNumber(Value& v)
    {
        size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        bool digits = false;
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                digits = digits || (c >= '0' && c <= '9');
                ++pos_;
            } else {
                break;
            }
        }
        if (!digits) return Fail("unexpected character");
        std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        double d = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()) return Fail("invalid number");
        v.type_ = Value::Type::Number;
        v.number_ = d;
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;
};

const Value* Value::Find(std::string_view key) const
{
    if (type_ != Type::Object) return nullptr;
    for (const auto& [k, v] : object_) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value* Value::Find(std::string_view key)
{
    return const_cast<Value*>(std::as_const(*this).Find(key));
}

const Value* Value::FindAny(std::initializer_list<std::string_view> keys) const
{
    for (auto key : keys) {
        const Value* v = Find(key);
        if (v && !v->IsNull()) return v;
    }
    return nullptr;
}

std::optional<bool> Value::AsBool() const
{
    if (type_ == Type::Bool) return bool_;
    return std::nullopt;
}

std::optional<double> Value::AsNumber() const
{
    if (type_ == Type::Number) {
        if (std::isfinite(number_)) return number_;
        return std::nullopt;
    }
    if (type_ == Type::String) {
        std::string_view s = Trim(string_);
        if (s.empty()) return std::nullopt;
        std::string tmp(s);
        char* end = nullptr;
        double d = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str() + tmp.size() && std::isfinite(d)) return d;
    }
    return std::nullopt;
}

std::optional<std::string> Value::AsString() const
{
    if (type_ != Type::String) return std::nullopt;
    std::string_view s = Trim(string_);
    if (s.empty()) return std::nullopt;
    return std::string(s);
}

std::optional<Value> Parse(std::string_view text, std::string* error)
{
    // 跳过 UTF-8 BOM
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    return Parser(text).Run(error);
}

std::string Quote(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

} // namespace cpa::json
