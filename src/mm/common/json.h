// MeetMind — 轻量 JSON 值类型（序列化 + 解析）
// 目的：在不引入第三方依赖的前提下满足导出与会话持久化需求。
// 约束：对象成员保持插入顺序；解析深度上限 64 层；字符串长度上限 64 MiB。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mm/common/result.h"

namespace mm {

class Json {
public:
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}                       // NOLINT
    Json(int v) : type_(Type::Number), number_(static_cast<double>(v)) {}  // NOLINT
    Json(int64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}  // NOLINT
    Json(double v) : type_(Type::Number), number_(v) {}                 // NOLINT
    Json(const char* v) : type_(Type::String), string_(v ? v : "") {}   // NOLINT
    Json(std::string v) : type_(Type::String), string_(std::move(v)) {} // NOLINT

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json array(std::initializer_list<Json> items);
    static Json of(const std::vector<std::string>& items);

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::Array; }
    bool isObject() const noexcept { return type_ == Type::Object; }

    // ---- 读取（类型不匹配返回兜底值，不抛异常） ----
    bool asBool(bool fallback = false) const noexcept;
    double asDouble(double fallback = 0.0) const noexcept;
    int64_t asInt64(int64_t fallback = 0) const noexcept;
    int asInt(int fallback = 0) const noexcept;
    const std::string& asString() const noexcept;

    /// 数组长度；非数组返回 0。
    size_t size() const noexcept;
    /// 对象是否含 key。
    bool has(const std::string& key) const noexcept;
    /// 对象取值；不存在返回 Null 引用。
    const Json& get(const std::string& key) const noexcept;
    /// 带默认值的取值。
    const Json& getOr(const std::string& key, const Json& fallback) const noexcept;
    /// 数组下标；越界返回 Null 引用。
    const Json& at(size_t index) const noexcept;
    /// 对象第 index 个成员的键名；越界或非对象返回空串。
    const std::string& keyAt(size_t index) const noexcept;

    // 便捷取值
    std::string getString(const std::string& key, const std::string& fallback = "") const;
    int getInt(const std::string& key, int fallback = 0) const;
    int64_t getInt64(const std::string& key, int64_t fallback = 0) const;
    double getDouble(const std::string& key, double fallback = 0.0) const;
    bool getBool(const std::string& key, bool fallback = false) const;

    // ---- 写入 ----
    Json& set(const std::string& key, Json value);
    Json& push(Json value);
    Json& append(const std::string& key, Json value) { return set(key, std::move(value)); }
    /// 便捷：直接追加字符串数组元素。
    Json& pushString(const std::string& v) { return push(Json(v)); }

    // ---- 序列化 / 反序列化 ----
    /// 缩进为 0 时输出紧凑格式。
    std::string dump(int indent = 2) const;
    static Result<Json> parse(const std::string& text);
    static Result<Json> parseFile(const std::string& path);
    Result<void> writeFile(const std::string& path, int indent = 2) const;

private:
    const Json& nullRef() const noexcept;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

}  // namespace mm
