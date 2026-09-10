#pragma once

#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainterPath>
#include <QRectF>
#include <QString>

#include <vector>

// Observe the actual text sent to the paint engine, including glyphs that a
// rectangle would clip. A screenshot alone cannot distinguish a missing digit.
class RecordingPaintDevice final : public QPaintDevice {
public:
    struct Text {
        QString value;
        QRectF bounds;
    };

    RecordingPaintDevice(int width, int height) : m_width(width), m_height(height) {}
    QPaintEngine* paintEngine() const override { return &m_engine; }
    const std::vector<Text>& text() const { return m_engine.text; }

protected:
    int metric(PaintDeviceMetric metric) const override
    {
        switch (metric) {
        case PdmWidth: return m_width;
        case PdmHeight: return m_height;
        case PdmWidthMM: return m_width * 254 / 960;
        case PdmHeightMM: return m_height * 254 / 960;
        case PdmDepth: return 32;
        case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return static_cast<int>(devicePixelRatioFScale());
        default: return 96;
        }
    }

private:
    class Engine final : public QPaintEngine {
    public:
        Engine() : QPaintEngine(AllFeatures) {}
        bool begin(QPaintDevice*) override { text.clear(); return true; }
        bool end() override { return true; }
        Type type() const override { return User; }
        void updateState(const QPaintEngineState&) override {}
        void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
        void drawPath(const QPainterPath&) override {}
        void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
        void drawPolygon(const QPoint*, int, PolygonDrawMode) override {}
        void drawTextItem(const QPointF& point, const QTextItem& item) override
        {
            text.push_back({item.text(), state->transform().mapRect(
                QRectF(point.x(), point.y() - item.ascent(),
                    item.width(), item.ascent() + item.descent()))});
        }
        std::vector<Text> text;
    };
    int m_width;
    int m_height;
    mutable Engine m_engine;
};
