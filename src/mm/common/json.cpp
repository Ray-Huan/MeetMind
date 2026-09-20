#include "mm/common/path_utils.h"
#include "mm/common/json.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "mm/common/string_utils.h"

namespace mm {
namespace {

const Json& nullJson() {
    static const Json kNull;
    return kNull;
}

void escapeTo(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void dumpTo(const Json& j, std::string& out, int indent, int depth) {
    const bool pretty = indent > 0;
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent) * (depth + 1), ' ') : std::string();
    const std::string padClose = pretty ? std::string(static_cast<size_t>(indent) * depth, ' ') : std::string();

    switch (j.type()) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.asBool() ? "true" : "false"; break;
        case Json::Type::Number: {
            const double v = j.asDouble();
            if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9.0e15) {
                out += std::to_string(static_cast<int64_t>(v));
            } else if (!std::isfinite(v)) {
                out += "0";
            } else {
                out += str::numberToString(v, 6);
            }
            break;
        }
        case Json::Type::String: escapeTo(j.asString(), out); break;
        case Json::Type::Array: {
            if (j.size() == 0) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < j.size(); ++i) {
                if (i) out.push_back(',');
                if (pretty) { out.push_back('\n'); out += pad; }
                dumpTo(j.at(i), out, indent, depth + 1);
            }
            if (pretty) { out.push_back('\n'); out += padClose; }
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            if (j.size() == 0) {
                out += "{}";
                break;
            }
            out.push_back('{');
            bool first = true;
            for (size_t i = 0; i < j.size(); ++i) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) { out.push_back('\n'); out += pad; }
                // 通过 at() 拿不到 key，这里用成员访问替代；见 Object 遍历辅助
                escapeTo(j.keyAt(i), out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                dumpTo(j.at(i), out, indent, depth + 1);
            }
            if (pretty) { out.push_back('\n'); out += padClose; }
            out.push_back('}');
            break;
        }
    }
}

/// 递归下降解析器。
class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Result<Json> parse() {
        skipWs();
        Result<Json> value = parseValue(0);
        if (!value.ok()) return value;
        skipWs();
        if (pos_ != text_.size()) {
            return fail(ErrorCode::InvalidArgument, errorAt("JSON 结尾存在多余字符"));
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 64;
    static constexpr size_t kMaxStringLength = 64u * 1024 * 1024;

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }

    void skipWs() {
        while (!eof()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    std::string errorAt(const std::string& what) const {
        std::ostringstream os;
        os << "JSON 解析错误(位置 " << pos_ << "): " << what;
        return os.str();
    }

    Result<Json> parseValue(int depth) {
        if (depth > kMaxDepth) {
            return fail(ErrorCode::InvalidArgument, errorAt("嵌套层数超过上限"));
        }
        skipWs();
        if (eof()) return fail(ErrorCode::InvalidArgument, errorAt("意外结束"));
        switch (peek()) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return parseString();
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default: return parseNumber();
        }
    }

    Result<Json> parseObject(int depth) {
        ++pos_;  // '{'
        Json obj = Json::object();
        skipWs();
        if (peek() == '}') { ++pos_; return obj; }
        while (true) {
            skipWs();
            if (peek() != '"') return fail(ErrorCode::InvalidArgument, errorAt("对象的键必须是字符串"));
            Result<Json> key = parseString();
            if (!key.ok()) return key;
            skipWs();
            if (peek() != ':') return fail(ErrorCode::InvalidArgument, errorAt("缺少 ':'"));
            ++pos_;
            Result<Json> value = parseValue(depth + 1);
            if (!value.ok()) return value;
            obj.set(key.value().asString(), std::move(value.value()));
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; break; }
            return fail(ErrorCode::InvalidArgument, errorAt("对象成员后缺少 ',' 或 '}'"));
        }
        return obj;
    }

    Result<Json> parseArray(int depth) {
        ++pos_;  // '['
        Json arr = Json::array();
        skipWs();
        if (peek() == ']') { ++pos_; return arr; }
        while (true) {
            Result<Json> value = parseValue(depth + 1);
            if (!value.ok()) return value;
            arr.push(std::move(value.value()));
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; break; }
            return fail(ErrorCode::InvalidArgument, errorAt("数组元素后缺少 ',' 或 ']'"));
        }
        return arr;
    }

    void appendUtf8(char32_t cp, std::string& out) const {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    Result<Json> parseString() {
        ++pos_;  // '"'
        std::string out;
        while (true) {
            if (eof()) return fail(ErrorCode::InvalidArgument, errorAt("字符串未闭合"));
            const char c = text_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                if (eof()) return fail(ErrorCode::InvalidArgument, errorAt("转义序列不完整"));
                const char e = text_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) {
                            return fail(ErrorCode::InvalidArgument, errorAt("\\u 序列不完整"));
                        }
                        char32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = text_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<char32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<char32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<char32_t>(h - 'A' + 10);
                            else return fail(ErrorCode::InvalidArgument, errorAt("\\u 含非法十六进制字符"));
                        }
                        // 代理对合并
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            char32_t low = 0;
                            size_t p = pos_ + 2;
                            bool good = true;
                            for (int i = 0; i < 4; ++i) {
                                const char h = text_[p + static_cast<size_t>(i)];
                                low <<= 4;
                                if (h >= '0' && h <= '9') low |= static_cast<char32_t>(h - '0');
                                else if (h >= 'a' && h <= 'f') low |= static_cast<char32_t>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') low |= static_cast<char32_t>(h - 'A' + 10);
                                else { good = false; break; }
                            }
                            if (good && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                pos_ += 6;
                            }
                        }
                        appendUtf8(cp, out);
                        break;
                    }
                    default:
                        return fail(ErrorCode::InvalidArgument, errorAt("未知转义字符"));
                }
            } else {
                out.push_back(c);
            }
            if (out.size() > kMaxStringLength) {
                return fail(ErrorCode::InvalidArgument, errorAt("字符串长度超过上限"));
            }
        }
        return Json(std::move(out));
    }

    Result<Json> parseBool() {
        if (text_.compare(pos_, 4, "true") == 0) { pos_ += 4; return Json(true); }
        if (text_.compare(pos_, 5, "false") == 0) { pos_ += 5; return Json(false); }
        return fail(ErrorCode::InvalidArgument, errorAt("非法的字面量"));
    }

    Result<Json> parseNull() {
        if (text_.compare(pos_, 4, "null") == 0) { pos_ += 4; return Json(nullptr); }
        return fail(ErrorCode::InvalidArgument, errorAt("非法的字面量"));
    }

    Result<Json> parseNumber() {
        const size_t start = pos_;
        if (peek() == '-' || peek() == '+') ++pos_;
        bool anyDigit = false;
        while (!eof()) {
            const char c = peek();
            if ((c >= '0' && c <= '9')) { ++pos_; anyDigit = true; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { ++pos_; continue; }
            break;
        }
        if (!anyDigit) return fail(ErrorCode::InvalidArgument, errorAt("非法数字"));
        const std::string token = text_.substr(start, pos_ - start);
        try {
            return Json(std::stod(token));
        } catch (...) {
            return fail(ErrorCode::InvalidArgument, errorAt("数字溢出或格式错误"));
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
};

}  // namespace

// ---- 静态构造 ----

Json Json::array(std::initializer_list<Json> items) {
    Json j = Json::array();
    for (const Json& item : items) j.push(item);
    return j;
}

Json Json::of(const std::vector<std::string>& items) {
    Json j = Json::array();
    for (const std::string& s : items) j.push(Json(s));
    return j;
}

// ---- 读取 ----

const Json& Json::nullRef() const noexcept { return nullJson(); }

bool Json::asBool(bool fallback) const noexcept {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

double Json::asDouble(double fallback) const noexcept {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return fallback;
}

int64_t Json::asInt64(int64_t fallback) const noexcept {
    if (type_ == Type::Number) return static_cast<int64_t>(number_);
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return fallback;
}

int Json::asInt(int fallback) const noexcept {
    if (type_ == Type::Number) return static_cast<int>(number_);
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return fallback;
}

const std::string& Json::asString() const noexcept {
    static const std::string kEmpty;
    return type_ == Type::String ? string_ : kEmpty;
}

size_t Json::size() const noexcept {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

bool Json::has(const std::string& key) const noexcept {
    if (type_ != Type::Object) return false;
    for (const Member& m : object_) {
        if (m.first == key) return true;
    }
    return false;
}

const Json& Json::get(const std::string& key) const noexcept {
    if (type_ != Type::Object) return nullRef();
    for (const Member& m : object_) {
        if (m.first == key) return m.second;
    }
    return nullRef();
}

const Json& Json::getOr(const std::string& key, const Json& fallback) const noexcept {
    if (type_ != Type::Object) return fallback;
    for (const Member& m : object_) {
        if (m.first == key) return m.second;
    }
    return fallback;
}

const Json& Json::at(size_t index) const noexcept {
    if (type_ == Type::Array) {
        if (index < array_.size()) return array_[index];
        return nullRef();
    }
    if (type_ == Type::Object) {
        if (index < object_.size()) return object_[index].second;
        return nullRef();
    }
    return nullRef();
}

const std::string& Json::keyAt(size_t index) const noexcept {
    static const std::string kEmpty;
    if (type_ == Type::Object && index < object_.size()) return object_[index].first;
    return kEmpty;
}

std::string Json::getString(const std::string& key, const std::string& fallback) const {    const Json& v = get(key);
    return v.isString() ? v.asString() : fallback;
}

int Json::getInt(const std::string& key, int fallback) const { return get(key).asInt(fallback); }
int64_t Json::getInt64(const std::string& key, int64_t fallback) const {
    return get(key).asInt64(fallback);
}
double Json::getDouble(const std::string& key, double fallback) const {
    return get(key).asDouble(fallback);
}
bool Json::getBool(const std::string& key, bool fallback) const {
    return get(key).asBool(fallback);
}

// ---- 写入 ----

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (Member& m : object_) {
        if (m.first == key) {
            m.second = std::move(value);
            return *this;
        }
    }
    object_.emplace_back(key, std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(value));
    return *this;
}

std::string Json::dump(int indent) const {
    std::string out;
    out.reserve(256);
    dumpTo(*this, out, indent, 0);
    return out;
}

Result<Json> Json::parse(const std::string& text) {
    Parser parser(text);
    return parser.parse();
}

Result<Json> Json::parseFile(const std::string& path) {
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::FileNotFound, "无法打开 JSON 文件: " + path);
    std::ostringstream os;
    os << in.rdbuf();
    Result<Json> r = parse(os.str());
    if (!r.ok()) return fail(r.code(), path + ": " + r.message());
    return r;
}

Result<void> Json::writeFile(const std::string& path, int indent) const {
    const std::string parent = pathutil::parentPath(path);
    if (!parent.empty() && !parent.empty() && !pathutil::createDirectories(parent)) {
        return fail(ErrorCode::IoError, "无法创建目录: " + parent);
    }
    const std::string data = dump(indent);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out = pathutil::openOutput(tmp);
        if (!out) return fail(ErrorCode::IoError, "无法写入临时文件: " + tmp);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) return fail(ErrorCode::IoError, "写入文件失败: " + tmp);
    }
    if (!pathutil::rename(tmp, path)) {
        return fail(ErrorCode::IoError, "无法替换目标文件: " + path);
    }
    return okStatus();
}

}  // namespace mm
