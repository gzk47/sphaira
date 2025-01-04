#include "ui/list.hpp"

namespace sphaira::ui {

List::List(const Vec4& pos, const Vec4& v, const Vec2& pad)
: m_v{v}
, m_pad{pad} {
    m_pos = pos;
}

void List::Do(NVGcontext* vg, Theme* theme, u64 index, u64 count, Callback callback, float y_off) const {
    auto v = m_v;

    if (vg) {
        nvgSave(vg);
        nvgScissor(vg, GetX(), GetY(), GetW(), GetH());
    }

    // index = (y_off / (v.h + m_pad.y)) / (GetW() / (v.w + m_pad.x));
    // v.y -= y_off;

    for (u64 i = index; i < count; v.y += v.h + m_pad.y) {
        if (v.y > GetY() + GetH()) {
            break;
        }

        const auto x = v.x;

        for (; i < count; i++, v.x += v.w + m_pad.x) {
            // only draw if full x is in bounds
            if (v.x + v.w > GetX() + GetW()) {
                break;
            }

            Vec4 vv = v;
            // if not drawing, only return clipped v as its used for touch
            if (!vg) {
                vv.w = std::min(v.x + v.w, m_pos.x + m_pos.w) - v.x;
                vv.h = std::min(v.y + v.h, m_pos.y + m_pos.h) - v.y;
            }

            if (!callback(vg, theme, vv, i)) {
                return;
            }
        }

        v.x = x;
    }

    if (vg) {
        nvgRestore(vg);
    }
}

} // namespace sphaira::ui
