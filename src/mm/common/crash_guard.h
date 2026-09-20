// MeetMind — 崩溃兜底
//
// 背景：核心库承诺「不抛异常」，但标准库本身会抛 —— 例如 std::filesystem 在
// Windows 上遇到无法转换的路径编码时会抛 filesystem_error，而这个异常发生在
// path 构造阶段，任何 error_code 参数都拦不住。
//
// 真实事故：含中文的音频路径触发上述异常 → 未捕获 → std::terminate → abort，
// 在 Windows 上恰好返回退出码 3，被命令行契约解释成「用户取消」，于是
// 「程序崩溃」被静默伪装成「用户主动取消」，排查时严重误导。
//
// 本模块做两件事：
//   1. installTerminateHandler()：把未捕获异常写进日志，并以**独立退出码**终止，
//      确保崩溃永远不会与任何正常退出码混淆。
//   2. runGuarded()：把 main 主体包进 try/catch，能捕获的就优雅返回。
#pragma once

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "mm/common/logger.h"

namespace mm {

/// 崩溃/内部错误的专用退出码（与 0 成功 / 1 运行期错误 / 2 参数错误 / 3 用户取消 区分）。
constexpr int kExitInternalError = 70;

/// 安装 std::terminate 处理器。应在 main 最开始调用。
void installTerminateHandler(int exitCode = kExitInternalError);

/// 把 main 的主体包进 try/catch；未捕获异常记录日志并返回 errorExitCode。
template <typename Fn>
int runGuarded(Fn&& fn, int errorExitCode = kExitInternalError) {
    try {
        return static_cast<int>(std::forward<Fn>(fn)());
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Error, "fatal",
                               std::string("未捕获异常: ") + e.what());
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        return errorExitCode;
    } catch (...) {
        Logger::instance().log(LogLevel::Error, "fatal", "未捕获的未知异常");
        return errorExitCode;
    }
}

}  // namespace mm
