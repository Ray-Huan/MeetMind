// MeetMind — 逆文本正则化（ITN）
// 将口语化中文数词/单位规范化，使纪要更易读；对已是阿拉伯数字的内容保持幂等。
//
// 支持：
//   百分之三十        → 30%
//   三点五            → 3.5
//   二零二六年        → 2026年
//   三千万            → 30000000
//   两千零五          → 2005
// 已知限制：口语省略式（「两千五」读作 2500）按字面解析为 2005，不作口语补全。
#pragma once

#include <string>

namespace mm::nlp {

struct ItnOptions {
    bool convertPercent = true;    ///< 百分之X → X%
    bool convertNumbers = true;    ///< 中文数词 → 阿拉伯数字
    bool normalizeFullWidth = true;///< 全角标点/数字 → 半角
    bool collapseSpaces = true;    ///< 折叠多余空白
    int minDigitRunForYear = 3;    ///< 纯数字读法（如二零二六）最少字符数
};

class TextNormalizer {
public:
    explicit TextNormalizer(ItnOptions options = ItnOptions{});
    ~TextNormalizer() = default;

    /// 对单段文本做规范化。
    std::string normalize(const std::string& text) const;

    /// 解析中文数词为整数；失败返回 false。
    static bool parseChineseInteger(const std::string& text, long long* out);
    /// 解析中文数词（含「点」小数）为 double；失败返回 false。
    static bool parseChineseNumber(const std::string& text, double* out);

    const ItnOptions& options() const { return options_; }

private:
    ItnOptions options_;
};

}  // namespace mm::nlp
