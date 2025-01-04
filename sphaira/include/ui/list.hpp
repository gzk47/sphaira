#pragma once

#include "ui/object.hpp"

namespace sphaira::ui {

struct List final : Object {
    using Callback = std::function<bool(NVGcontext* vg, Theme* theme, Vec4 v, u64 index)>;

    List(const Vec4& pos, const Vec4& v, const Vec2& pad = {});

    void Do(u64 index, u64 count, Callback callback) const {
        Do(nullptr, nullptr, index, count, callback);
    }

    void Do(NVGcontext* vg, Theme* theme, u64 index, u64 count, Callback callback, float y_off = 0) const;

private:
    auto Draw(NVGcontext* vg, Theme* theme) -> void override {}

private:
    Vec4 m_v;
    Vec2 m_pad;
};

} // namespace sphaira::ui
