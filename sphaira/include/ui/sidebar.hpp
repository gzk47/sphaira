#pragma once

#include "ui/widget.hpp"
#include "ui/list.hpp"
#include "ui/scrolling_text.hpp"
#include <memory>
#include <concepts>
#include <utility>

namespace sphaira::ui {

class SidebarEntryBase : public Widget {
public:
    explicit SidebarEntryBase(const std::string& title, const std::string& info);

    using Widget::Draw;
    virtual void Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left);

protected:
    std::string m_title;

private:
    std::string m_info;
    ScrollingText m_scolling_title{};
};

template<typename T>
concept DerivedFromSidebarBase = std::is_base_of_v<SidebarEntryBase, T>;

class SidebarEntryBool final : public SidebarEntryBase {
public:
    using Callback = std::function<void(bool&)>;

public:
    explicit SidebarEntryBool(const std::string& title, bool option, Callback cb, const std::string& info = "", const std::string& true_str = "On", const std::string& false_str = "Off");
    explicit SidebarEntryBool(const std::string& title, bool& option, const std::string& info = "", const std::string& true_str = "On", const std::string& false_str = "Off");

private:
    void Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) override;

    bool m_option;
    Callback m_callback;
    std::string m_true_str;
    std::string m_false_str;
};

class SidebarEntryCallback final : public SidebarEntryBase {
public:
    using Callback = std::function<void()>;

public:
    explicit SidebarEntryCallback(const std::string& title, Callback cb, const std::string& info);
    explicit SidebarEntryCallback(const std::string& title, Callback cb, bool pop_on_click = false, const std::string& info = "");
    void Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) override;

private:
    Callback m_callback;
    bool m_pop_on_click;
};

class SidebarEntryArray final : public SidebarEntryBase {
public:
    using Items = std::vector<std::string>;
    using ListCallback = std::function<void()>;
    using Callback = std::function<void(s64& index)>;

public:
    explicit SidebarEntryArray(const std::string& title, const Items& items, Callback cb, s64 index = 0, const std::string& info = "");
    explicit SidebarEntryArray(const std::string& title, const Items& items, Callback cb, const std::string& index, const std::string& info = "");
    explicit SidebarEntryArray(const std::string& title, const Items& items, std::string& index, const std::string& info = "");

    void Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

private:
    Items m_items;
    ListCallback m_list_callback;
    Callback m_callback;
    s64 m_index;
    s64 m_tick{};
    float m_text_yoff{};
};

class Sidebar final : public Widget {
public:
    enum class Side { LEFT, RIGHT };
    using Items = std::vector<std::unique_ptr<SidebarEntryBase>>;

public:
    explicit Sidebar(const std::string& title, Side side, Items&& items);
    explicit Sidebar(const std::string& title, Side side);
    explicit Sidebar(const std::string& title, const std::string& sub, Side side, Items&& items);
    explicit Sidebar(const std::string& title, const std::string& sub, Side side);

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

    void Add(std::unique_ptr<SidebarEntryBase>&& entry);

    template<DerivedFromSidebarBase T, typename... Args>
    void Add(Args&&... args) {
        Add(std::make_unique<T>(std::forward<Args>(args)...));
    }

private:
    void SetIndex(s64 index);
    void SetupButtons();

private:
    std::string m_title;
    std::string m_sub;
    Side m_side;
    Items m_items;
    s64 m_index{};

    std::unique_ptr<List> m_list;

    Vec4 m_top_bar{};
    Vec4 m_bottom_bar{};
    Vec2 m_title_pos{};
    Vec4 m_base_pos{};

    static constexpr float m_title_size{28.f};
    // static constexpr Vec2 box_size{380.f, 70.f};
    static constexpr Vec2 m_box_size{400.f, 70.f};
};

} // namespace sphaira::ui
