#pragma once
#include "Element.hpp"
#include "../GUIManager.hpp"
#include "../../TimePoint.hpp"

namespace GUIStuff {

struct NumberSliderData {
    std::function<void()> onChange;
    std::function<void()> onHold;
    std::function<void()> onRelease;
};

template <typename T> class NumberSlider : public Element {
    public:
        NumberSlider(GUIManager& gui):
            Element(gui) {}

        static constexpr float HOLD_ANIMATION_TIME = 0.3f;

        void layout(const Clay_ElementId& id, T* data, T minData, T maxData, const NumberSliderData& config) {
            this->data = data;
            dd.val = *data;
            dd.minData = minData;
            dd.maxData = maxData;
            this->config = config;

            CLAY(id, {
                .layout = {
                    .sizing = {.width = CLAY_SIZING_GROW(100), .height = CLAY_SIZING_FIXED(10)}
                },
                .custom = { .customData = this }
            }) {
            }
        }

        virtual void update() override {
            smooth_two_way_animation_time(dd.holdAnimation, gui.io.deltaTime, dd.isHeld, HOLD_ANIMATION_TIME);
            smooth_two_way_animation_time(dd.hoverAnimation, gui.io.deltaTime, mouseHovering && !gui.last_interaction_is_touch(), gui.io.theme->hoverExpandTime);
            // Also invalidate when the slider's bounding box has moved (e.g.
            // the slider lives inside a ScrollArea and the user scrolled).
            // Without this gate, the slider's last draw stays cached at its
            // prior screen position, and the scroll surface paints over it
            // wherever it moved to -- the slider visually disappears until
            // a hover / value / hold change forces a redraw.
            bool bbChanged = boundingBox != oldBoundingBox;
            if(oldDD != dd || bbChanged) {
                gui.invalidate_draw_element(this, {
                    .top = 10.0f,
                    .bottom = 10.0f,
                    .left = 10.0f,
                    .right = 10.0f
                });
                oldDD = dd;
                oldBoundingBox = boundingBox;
            }
        }

        virtual void clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* command, bool skiaAA) override {
            auto& bb = boundingBox.value();

            canvas->save();
            canvas->translate(bb.min.x(), bb.min.y());

            float lerpTimeHover = dd.hoverAnimation / io.theme->hoverExpandTime;

            static BezierEasing easeHeight(0.68, -1.55, 0.265, 2.55);

            float lerpTimeHeld = easeHeight(dd.holdAnimation / HOLD_ANIMATION_TIME);

            float holderRadius = lerp_vec(4.0, 5.0, lerpTimeHover);
            float holderHeight = lerp_vec(4.0, 10.0, lerpTimeHeld);

            const float yChange = bb.height() * 0.5f - holderRadius * 0.5f;

            float holderPos = lerp_time<float>(dd.val, dd.maxData, dd.minData) * bb.width();

            SkRect barFull = SkRect::MakeXYWH(0.0f, yChange, holderPos, holderRadius);
            SkRect barEmpty = SkRect::MakeXYWH(holderPos, yChange, bb.width() - holderPos, holderRadius);

            SkPaint barFullP;
            barFullP.setAntiAlias(skiaAA);
            barFullP.setColor(convert_vec4<SkColor4f>(io.theme->fillColor1));
            canvas->drawRoundRect(barFull, 5.0f, 5.0f, barFullP);

            SkPaint barEmptyP;
            barEmptyP.setAntiAlias(skiaAA);
            barEmptyP.setColor(convert_vec4<SkColor4f>(io.theme->backColor2));
            canvas->drawRoundRect(barEmpty, 5.0f, 5.0f, barEmptyP);

            canvas->translate(holderPos, bb.height() * 0.5f);

            SkRect holderRect = SkRect::MakeLTRB(-holderRadius, -holderHeight, holderRadius, holderHeight);
            SkPaint holderBorderP;
            holderBorderP.setAntiAlias(skiaAA);
            holderBorderP.setStyle(SkPaint::kStroke_Style);
            holderBorderP.setStrokeWidth(3.0f);
            holderBorderP.setColor(convert_vec4<SkColor4f>(io.theme->fillColor1));
            canvas->drawRoundRect(holderRect, holderRadius, holderRadius, holderBorderP);

            SkPaint holderP;
            holderP.setAntiAlias(skiaAA);
            holderP.setColor(convert_vec4<SkColor4f>(io.theme->backColor2));
            canvas->drawRoundRect(holderRect, holderRadius, holderRadius, holderP);
            
            canvas->restore();
        }
        virtual void input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button) override {
            bool oldIsHeld = dd.isHeld;
            dd.isHeld = mouseHovering && button.button == InputManager::MouseButton::LEFT && button.down;
            if(oldIsHeld && !dd.isHeld) {
                gui.set_post_callback_func([&] {
                    if(config.onRelease) config.onRelease();
                });
            }
            else if(dd.isHeld && boundingBox.has_value())
                update_slider_pos(button.pos, true);
        }

        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override {
            if(dd.isHeld && boundingBox.has_value())
                update_slider_pos(motion.pos, false);
        }

        virtual void input_finger_touch_callback(const InputManager::FingerTouchCallbackArgs& touch) override {
            bool oldIsHeld = dd.isHeld;
            dd.isHeld = mouseHovering && touch.down;
            if(oldIsHeld && !dd.isHeld) {
                gui.set_post_callback_func([&] {
                    if(config.onRelease) config.onRelease();
                });
            }
            else if(dd.isHeld && boundingBox.has_value())
                update_slider_pos(touch.pos, true);
        }

        virtual void input_finger_motion_callback(const InputManager::FingerMotionCallbackArgs& motion) override {
            if(dd.isHeld && boundingBox.has_value())
                update_slider_pos(motion.pos, false);
        }

    private:
        void update_slider_pos(const Vector2f& p, bool justHeld) {
            gui.set_post_callback_func([&, p, justHeld] {
                float fracPosOnSlider = (p.x() - boundingBox.value().min.x()) / boundingBox.value().width();
                dd.val = *data = static_cast<T>(std::clamp<double>(std::lerp<double>(dd.minData, dd.maxData, fracPosOnSlider), dd.minData, dd.maxData)); // Clamp as double then cast so that unsigned types dont wrap on clamp
                if(justHeld && config.onHold) config.onHold();
                if(config.onChange) config.onChange();
            });
        }

        T* data = nullptr;

        struct DisplayData {
            bool isHeld = false;
            T val = 0.0;

            T minData = 0.0;
            T maxData = 1.0;

            float hoverAnimation = 0.0;
            float holdAnimation = 0.0;

            bool operator!=(const DisplayData&) const = default;
            bool operator==(const DisplayData&) const = default;
        };

        DisplayData dd;
        DisplayData oldDD;
        // Snapshot of the slider's bounding box from the previous frame;
        // used by update() to invalidate when scrolling / window resize
        // moves the slider without changing its display data.
        std::optional<SCollision::AABB<float>> oldBoundingBox;

        NumberSliderData config;
};

}
