// MeetMind — 粒子特效覆盖层（二次元风装饰）
//
// 一个透明覆盖层，用 QPainter 在界面上方绘制飘浮的粒子
// （星星 / 心形 / 圆点 / 花瓣），营造轻盈的二次元氛围。
// 通过 WA_TransparentForMouseEvents 让鼠标事件穿透，不干扰交互。
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace mm::gui {

class ParticleOverlay : public QWidget {
    Q_OBJECT

public:
    explicit ParticleOverlay(QWidget* parent = nullptr);
    ~ParticleOverlay() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Particle {
        double x = 0.0;      // 归一化 0..1（乘宽）
        double y = 0.0;      // 归一化 0..1（乘高）
        double vy = 0.02;    // 上升速度（归一化 / 秒）
        double size = 6.0;   // 像素
        double phase = 0.0;  // 闪烁 / 摇摆相位
        int type = 0;        // 0 星 1 心 2 圆点 3 花瓣
        QColor color;
    };

    void step(double dt);
    void drawShape(QPainter& p, const Particle& pt, double cx, double cy) const;

    QVector<Particle> particles_;
    QTimer timer_;
    QElapsedTimer clock_;
};

}  // namespace mm::gui
