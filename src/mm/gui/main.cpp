// MeetMind — 图形界面入口
// 除常规启动外，支持 --selftest-render <png>：以离屏平台渲染主窗口并保存为 PNG，
// 用于 CI 环境下的界面冒烟测试（配合 QT_QPA_PLATFORM=offscreen）。
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QPixmap>
#include <QTimer>
#include <cstdio>

#include "mm/common/config.h"
#include "mm/common/crash_guard.h"
#include "mm/common/logger.h"
#include "main_window.h"

namespace {

int runGui(int argc, char** argv) {
    QApplication app(argc, argv);
    // 显式指定中文字体族，避免在无桌面环境（如离屏渲染）下退化为方块
    {
        QFont font = app.font();
        font.setFamily(QStringLiteral("Microsoft YaHei"));
        font.setPointSize(9);
        app.setFont(font);
    }

    // 全局样式：现代化简约浅色主题（扁平、圆角、统一强调色 #2563eb）
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: #f8fafc; }
        QToolBar { background: #ffffff; border-bottom: 1px solid #e2e8f0; padding: 6px; spacing: 6px; }
        QToolBar QLabel { color: #475569; padding-right: 4px; }
        QToolButton { border: none; border-radius: 6px; padding: 6px 12px; background: transparent; color: #334155; }
        QToolButton:hover { background: #f1f5f9; }
        QToolButton:pressed { background: #e2e8f0; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: transparent; padding: 8px 20px; margin-right: 2px; color: #64748b; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: #2563eb; border-bottom: 2px solid #2563eb; }
        QTabBar::tab:hover:!selected { color: #1e293b; }
        QPushButton { background: #2563eb; color: #ffffff; border: none; border-radius: 6px; padding: 7px 16px; }
        QPushButton:hover { background: #1d4ed8; }
        QPushButton:pressed { background: #1e40af; }
        QPushButton:disabled { background: #cbd5e1; color: #94a3b8; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: #ffffff; border: 1px solid #cbd5e1; border-radius: 6px; padding: 5px 8px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #2563eb; }
        QComboBox::drop-down { border: none; width: 22px; }
        QListWidget, QTableView, QTreeView { background: #ffffff; border: 1px solid #e2e8f0; border-radius: 8px; }
        QTextBrowser, QPlainTextEdit, QTextEdit { background: #ffffff; border: 1px solid #e2e8f0; border-radius: 8px; }
        QProgressBar { border: none; background: #e2e8f0; border-radius: 5px; text-align: center; color: #334155; }
        QProgressBar::chunk { background: #2563eb; border-radius: 5px; }
        QCheckBox, QRadioButton { spacing: 6px; }
        QGroupBox { border: 1px solid #e2e8f0; border-radius: 8px; margin-top: 12px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #475569; }
        QScrollBar:vertical { background: transparent; width: 10px; }
        QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 5px; min-height: 24px; }
        QScrollBar:horizontal { background: transparent; height: 10px; }
        QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 5px; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QSplitter::handle { background: #e2e8f0; }
        QStatusBar { background: #ffffff; border-top: 1px solid #e2e8f0; color: #64748b; }
        QHeaderView::section { background: #f1f5f9; border: none; border-bottom: 1px solid #e2e8f0; padding: 6px 8px; color: #475569; }
    )"));

    QCoreApplication::setApplicationName(QStringLiteral("MeetMind"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(MEETMIND_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("MeetMind"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QString::fromUtf8("MeetMind — 端侧语音转写与智能会议纪要工具"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption inputOption(
        QStringList{QStringLiteral("i"), QStringLiteral("input")},
        QString::fromUtf8("启动后直接载入并处理指定 WAV 文件"), QStringLiteral("file"));
    QCommandLineOption configOption(
        QStringList{QStringLiteral("c"), QStringLiteral("config")},
        QString::fromUtf8("指定配置文件路径"), QStringLiteral("file"));
    QCommandLineOption renderOption(
        QStringList{QStringLiteral("selftest-render")},
        QString::fromUtf8("离屏渲染主窗口到 PNG 后退出（界面冒烟测试）"),
        QStringLiteral("png"));
    QCommandLineOption exitAfterOption(
        QStringList{QStringLiteral("selftest-exit-ms")},
        QString::fromUtf8("自检模式下等待多久后截图（毫秒，默认 600）"),
        QStringLiteral("ms"), QStringLiteral("600"));
    QCommandLineOption tabOption(
        QStringList{QStringLiteral("selftest-tab")},
        QString::fromUtf8("自检截图前切到第 N 个标签页（0 起算；默认 0）"),
        QStringLiteral("n"));
    QCommandLineOption realtimeOption(
        QStringList{QStringLiteral("selftest-realtime")},
        QString::fromUtf8("自检：以文件模拟实时流启动实时转写（值为 WAV 路径）"),
        QStringLiteral("wav"));
    QCommandLineOption realtimeSpeedOption(
        QStringList{QStringLiteral("selftest-realtime-speed")},
        QString::fromUtf8("自检实时流的模拟倍速（默认 1.0）"),
        QStringLiteral("n"), QStringLiteral("1.0"));

    parser.addOption(inputOption);
    parser.addOption(configOption);
    parser.addOption(renderOption);
    parser.addOption(exitAfterOption);
    parser.addOption(tabOption);
    parser.addOption(realtimeOption);
    parser.addOption(realtimeSpeedOption);
    parser.process(app);

    const std::string configPath = parser.isSet(configOption)
                                       ? parser.value(configOption).toStdString()
                                       : std::string();
    mm::Config config = mm::Config::loadOrDefault(configPath);
    mm::Logger::instance().setLevel(config.logLevel);

    mm::gui::MainWindow window(config);
    window.show();

    // 自检可指定要截图的标签页（例如实时转写面板）
    if (parser.isSet(tabOption)) {
        window.selectTab(parser.value(tabOption).toInt());
    }
    std::printf("[selftest] 标签页数量: %d\n", window.tabCount());

    const QString renderTarget = parser.isSet(renderOption) ? parser.value(renderOption) : QString();
    if (!renderTarget.isEmpty()) {
        int delayMs = parser.value(exitAfterOption).toInt();
        if (delayMs <= 0) delayMs = 600;
        QTimer::singleShot(delayMs, &window, [&window, renderTarget, &app]() {
            // 先让事件循环完成一次布局，再抓取窗口
            const QPixmap shot = window.grab();
            const QFileInfo info(renderTarget);
            if (!info.absolutePath().isEmpty()) {
                QDir().mkpath(info.absolutePath());
            }
            const bool ok = shot.save(renderTarget, "PNG");
            std::printf("[selftest] 渲染窗口 %dx%d -> %s : %s\n", window.width(),
                        window.height(), renderTarget.toUtf8().constData(),
                        ok ? "成功" : "失败");
            const bool nonEmpty = !shot.isNull() && shot.width() > 0 && shot.height() > 0;
            std::fflush(stdout);
            app.exit(ok && nonEmpty ? 0 : 1);
        });
    }

    // 自检：以文件模拟实时流跑通「界面 → 音频源 → 实时转写」全链路
    if (parser.isSet(realtimeOption)) {
        const QString rtPath = parser.value(realtimeOption);
        const double rtSpeed = parser.value(realtimeSpeedOption).toDouble();
        QTimer::singleShot(200, &window, [&window, rtPath, rtSpeed]() {
            window.startRealtime(rtPath, rtSpeed > 0.0 ? rtSpeed : 1.0);
        });
    }

    const QString input = parser.isSet(inputOption) ? parser.value(inputOption) : QString();
    if (!input.isEmpty()) {
        if (!QFileInfo::exists(input)) {
            std::fprintf(stderr, "输入文件不存在: %s\n", input.toUtf8().constData());
            return 2;
        }
        // 等窗口完成首帧后再启动处理，确保进度信号能正确投递
        QTimer::singleShot(0, &window, [&window, input]() { window.openAndProcess(input); });
    }

    const int rc = app.exec();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    // 同 CLI：装崩溃兜底，避免未捕获异常（如路径编码转换失败）直接 terminate
    mm::installTerminateHandler(mm::kExitInternalError);
    return mm::runGuarded([&]() { return runGui(argc, argv); });
}
