// MeetMind 测试入口
#include <cstring>
#include <iostream>
#include <string>

#include "mm/common/logger.h"
#include "testing.h"

namespace {

void printUsage() {
    std::cout << "用法: mm_tests [选项]\n"
                 "  -v, --verbose        打印每个用例的耗时\n"
                 "  -f, --filter <关键词> 仅运行名称包含关键词的用例\n"
                 "  -l, --list           列出全部用例\n"
                 "  -q, --quiet          关闭引擎日志（默认即为 warn）\n"
                 "  -h, --help           显示帮助\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    std::string filter;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-l" || arg == "--list") {
            listOnly = true;
        } else if (arg == "-q" || arg == "--quiet") {
            mm::Logger::instance().setLevel(mm::LogLevel::Off);
        } else if ((arg == "-f" || arg == "--filter") && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(9);
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            printUsage();
            return 2;
        }
    }

    // 测试期间默认只显示警告及以上，避免淹没测试输出
    if (mm::Logger::instance().level() != mm::LogLevel::Off) {
        mm::Logger::instance().setLevel(mm::LogLevel::Warn);
    }
    // 测试要求完全离线
    mm::Logger::instance().setTimestampEnabled(false);

    auto& registry = mmtest::Registry::instance();

    if (listOnly) {
        for (const auto& tc : registry.cases()) {
            std::cout << tc.suite << "." << tc.name << "\n";
        }
        std::cout << "共 " << registry.cases().size() << " 个用例\n";
        return 0;
    }

    const int failed = registry.run(verbose, filter);
    return failed == 0 ? 0 : 1;
}
