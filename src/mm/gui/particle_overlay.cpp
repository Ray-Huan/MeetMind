// MeetMind — 粒子特效覆盖层实现
#include "mm/gui/particle_overlay.h"

#include <QRandomGenerator>
#include <QtMath>
#include <QPainter>
#include <QPainterPath>

namespace mm::gui {
namespace {

// 二次元柔和色系（樱花粉 / 浅紫 / 浅蓝 / 浅黄 / 白）
const QColor kPalette[] = {
    QColor(0xff, 0x9e, 0xc7),  // 樱花粉
    QColor(0xff, 0xb6, 0xc1),  // 浅粉
    QColor(0xc9, 0xa7, 0xff),  // 浅紫
    QColor(0xa7, 0xd8, 0xff),  // 浅蓝
    QColor(0xff, 0xe9, 0xa7),  // 浅黄
    QColor(0xff, 0xff, 0xff),  // 白（星）
};

double rand01() { return QRandomGenerator::global()->generateDouble(); }
}  // namespace

ParticleOverlay::ParticleOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);

    const int count = 36;
    particles_.reserve(count);
    for (int i = 0; i < count; ++i) {
        Particle p;
        p.x = rand01();
        p.y = rand01();
        p.vy = 0.015 + rand01() * 0.05;              // 缓急不一
        p.size = 4.0 + rand01() * 8.0;                // 4..12 px
        p.phase = rand01() * 6.28318;
        p.type = int(rand01() * 4) % 4;               // 0..3
        p.color = kPalette[QRandomGenerator::global()->bounded(6)];
        particles_.push_back(p);
    }

    clock_.start();
    connect(&timer_, &QTimer::timeout, this, [this]() {
        const double dt = clock_.restart() / 1000.0;
        step(dt);
        update();
    });
    timer_.start(33);  // ~30 fps
}

ParticleOverlay::~ParticleOverlay() = default;

void ParticleOverlay::step(double dt) {
    if (dt <= 0.0 || dt > 0.2) return;  // 长时间挂起时防跳变
    for (Particle& p : particles_) {
        p.y -= p.vy * dt;
        p.phase += dt * 2.0;
        // 飘出顶部后回到底部重生，并随机左右
        if (p.y < -0.08) {
            p.y = 1.05;
            p.x = rand01();
        }
    }
}

void ParticleOverlay::drawShape(QPainter& p, const Particle& pt, double cx, double cy) const {
    const double s = pt.size;
    // 闪烁透明度：0.45 + 0.55*sin(phase)，柔和呼吸
    const double alpha = 0.45 + 0.55 * (0.5 + 0.5 * qSin(pt.phase));
    QColor c = pt.color;
    c.setAlphaF(qBound(0.15, alpha, 0.95));

    switch (pt.type) {
        case 0: {  // 五角星
            QPainterPath path;
            for (int i = 0; i < 10; ++i) {
                const double r = (i % 2 == 0) ? s : s * 0.45;
                const double a = -M_PI_2 + i * M_PI / 5.0;
                const QPointF ptc(cx + r * qCos(a), cy + r * qSin(a));
                if (i == 0) path.moveTo(ptc); else path.lineTo(ptc);
            }
            path.closeSubpath();
            p.fillPath(path, c);
            break;
        }
        case 1: {  // 心形
            QPainterPath path;
            const double k = s * 0.5;
            path.moveTo(cx, cy + k);
            path.cubicTo(cx - k * 1.4, cy + k * 0.1, cx - k * 0.7, cy - k * 0.9, cx, cy - k * 0.3);
            path.cubicTo(cx + k * 0.7, cy - k * 0.9, cx + k * 1.4, cy + k * 0.1, cx, cy + k);
            p.fillPath(path, c);
            break;
        }
        case 2: {  // 圆点（光斑）
            QRadialGradient g(cx, cy, s);
            g.setColorAt(0.0, c);
            g.setColorAt(1.0, QColor(c.red(), c.green(), c.blue(), 0));
            p.setBrush(g);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(cx, cy), s, s);
            break;
        }
        case 3: {  // 花瓣（椭圆 + 轻微摇摆）
            p.save();
            p.translate(cx, cy);
            p.rotate(qSin(pt.phase * 0.7) * 18.0);
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(0, 0), s * 0.55, s * 1.0);
            p.restore();
            break;
        }
        default:
            break;
    }
}

void ParticleOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = width();
    const double h = height();
    for (const Particle& p : particles_) {
        drawShape(painter, p, p.x * w, p.y * h);
    }
}

}  // namespace mm::gui
