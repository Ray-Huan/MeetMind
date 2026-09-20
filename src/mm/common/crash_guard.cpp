#include "mm/common/crash_guard.h"

#include <cstdlib>

namespace mm {

void installTerminateHandler(int exitCode) {
    // 用静态变量捕获退出码：std::terminate 处理器不能带参数
    static int g_exitCode = kExitInternalError;
    g_exitCode = exitCode;

    std::set_terminate([]() {
        std::string what = "未知异常";
        if (std::exception_ptr ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
                what = "非 std::exception 类型的异常";
            }
        }
        try {
            Logger::instance().log(LogLevel::Error, "fatal",
                                   "未捕获异常导致进程终止: " + what +
                                       "（退出码 " + std::to_string(g_exitCode) + "）");
        } catch (...) {
            // 日志本身失败时无能为力，继续走终止流程
        }
        std::fflush(nullptr);
        // 用 _Exit 跳过析构与 atexit，避免二次崩溃
        std::_Exit(g_exitCode);
    });
}

}  // namespace mm
