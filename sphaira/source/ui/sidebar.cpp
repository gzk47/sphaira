#include "ui/sidebar.hpp"
#include "app.hpp"
#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"

namespace sphaira::ui {
namespace {

auto DistanceBetweenY(Vec4 va, Vec4 vb) -> Vec4 {
    return Vec4{
        va.x, va.y,
        va.w, vb.y - va.y
    };
}

} // namespace

SidebarEntryBase::SidebarEntryBase(std::string&& title)
: m_title{std::forward<std::string>(title)} {

}

auto SidebarEntryBase::Draw(NVGcontext* vg, Theme* theme) -> void {
    // draw spacers or highlight box if in focus (selected)
    if (HasFocus()) {
        gfx::drawRect(vg, m_pos, nvgRGB(50,50,50));
        gfx::drawRect(vg, m_pos, nvgRGB(0,0,0));
        gfx::drawRectOutline(vg, 4.f, theme->elements[ThemeEntryID_SELECTED_OVERLAY].colour, m_pos, theme->elements[ThemeEntryID_SELECTED].colour);
        // gfx::drawRect(vg, m_pos.x - 4.f, m_pos.y - 4.f, m_pos.w + 8.f, m_pos.h + 8.f, theme->elements[ThemeEntryID_SELECTED_OVERLAY].colour);
        // gfx::drawRect(vg, m_pos.x, m_pos.y, m_pos.w, m_pos.h, theme->elements[ThemeEntryID_SELECTED].colour);
    } else {
        gfx::drawRect(vg, m_pos.x, m_pos.y, m_pos.w, 1.f, nvgRGB(81, 81, 81)); // spacer
        gfx::drawRect(vg, m_pos.x, m_pos.y + m_pos.h, m_pos.w, 1.f, nvgRGB(81, 81, 81)); // spacer
    }
}

SidebarEntryBool::SidebarEntryBool(std::string title, bool option, Callback cb, std::string true_str, std::string false_str)
: SidebarEntryBase{std::move(title)}
, m_option{option}
, m_callback{cb}
, m_true_str{std::move(true_str)}
, m_false_str{std::move(false_str)} {

    SetAction(Button::A, Action{"OK"_i18n, [this](){
            m_option ^= 1;
            m_callback(m_option);
        }
    });
}

SidebarEntryBool::SidebarEntryBool(std::string title, bool& option, std::string true_str, std::string false_str)
: SidebarEntryBool{std::move(title), option, Callback{} } {
    m_callback = [](bool& option){
        option ^= 1;
    };
}

auto SidebarEntryBool::Draw(NVGcontext* vg, Theme* theme) -> void {
    SidebarEntryBase::Draw(vg, theme);

    // if (HasFocus()) {
    //     gfx::drawText(vg, Vec2{m_pos.x + 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT_SELECTED].colour, m_title.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    // } else {
    // }

    gfx::drawText(vg, Vec2{m_pos.x + 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT].colour, m_title.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    if (m_option == true) {
        gfx::drawText(vg, Vec2{m_pos.x + m_pos.w - 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT_SELECTED].colour, m_true_str.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    } else { // text info
        gfx::drawText(vg, Vec2{m_pos.x + m_pos.w - 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT].colour, m_false_str.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    }
}

SidebarEntryCallback::SidebarEntryCallback(std::string title, Callback cb, bool pop_on_click)
: SidebarEntryBase{std::move(title)}
, m_callback{cb}
, m_pop_on_click{pop_on_click} {
    SetAction(Button::A, Action{"OK"_i18n, [this](){
            m_callback();
            if (m_pop_on_click) {
                SetPop();
            }
        }
    });
}

auto SidebarEntryCallback::Draw(NVGcontext* vg, Theme* theme) -> void {
    SidebarEntryBase::Draw(vg, theme);

    // if (HasFocus()) {
    //     gfx::drawText(vg, Vec2{m_pos.x + 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT_SELECTED].colour, m_title.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    // } else {
        gfx::drawText(vg, Vec2{m_pos.x + 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT].colour, m_title.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    // }
}

SidebarEntryArray::SidebarEntryArray(std::string title, Items items, std::string& index)
: SidebarEntryArray{std::move(title), std::move(items), Callback{}, 0} {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index);
    if (it != m_items.cend()) {
        m_index = std::distance(m_items.cbegin(), it);
    }

    m_list_callback = [&index, this]() {
        App::Push(std::make_shared<PopupList>(
            m_title, m_items, index, m_index
        ));
    };

    // m_callback = [&index, this](auto& idx) {
    //     App::Push(std::make_shared<PopupList>(
    //         m_title, m_items, index, idx
    //     ));
    // };
}

SidebarEntryArray::SidebarEntryArray(std::string title, Items items, Callback cb, std::string index)
: SidebarEntryArray{std::move(title), std::move(items), cb, 0} {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index);
    if (it != m_items.cend()) {
        m_index = std::distance(m_items.cbegin(), it);
    }
}

SidebarEntryArray::SidebarEntryArray(std::string title, Items items, Callback cb, std::size_t index)
: SidebarEntryBase{std::forward<std::string>(title)}
, m_items{std::move(items)}
, m_callback{cb}
, m_index{index} {

    m_list_callback = [this]() {
        App::Push(std::make_shared<PopupList>(
            m_title, m_items, [this](auto op_idx){
                if (op_idx) {
                    m_index = *op_idx;
                    m_callback(m_index);
                }
            }, m_index
        ));
    };

    SetAction(Button::A, Action{"OK"_i18n, [this](){
            // m_callback(m_index);
            m_list_callback();
        }
    });
}

auto SidebarEntryArray::Draw(NVGcontext* vg, Theme* theme) -> void {
    SidebarEntryBase::Draw(vg, theme);

    const auto& text_entry = m_items[m_index];
    // const auto& colour = HasFocus() ? theme->elements[ThemeEntryID_TEXT_SELECTED].colour : theme->elements[ThemeEntryID_TEXT].colour;
    const auto& colour = theme->elements[ThemeEntryID_TEXT].colour;

    gfx::drawText(vg, Vec2{m_pos.x + 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, colour, m_title.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, Vec2{m_pos.x + m_pos.w - 15.f, m_pos.y + (m_pos.h / 2.f)}, 20.f, theme->elements[ThemeEntryID_TEXT_SELECTED].colour, text_entry.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
}

Sidebar::Sidebar(std::string title, Side side, Items&& items)
: Sidebar{std::move(title), "", side, std::forward<Items>(items)} {
}

Sidebar::Sidebar(std::string title, Side side)
: Sidebar{std::move(title), "", side, {}} {
}

Sidebar::Sidebar(std::string title, std::string sub, Side side, Items&& items)
: m_title{std::move(title)}
, m_sub{std::move(sub)}
, m_side{side}
, m_items{std::forward<Items>(items)} {
    switch (m_side) {
        case Side::LEFT:
            SetPos(Vec4{0.f, 0.f, 450.f, 720.f});
            break;

        case Side::RIGHT:
            SetPos(Vec4{1280.f - 450.f, 0.f, 450.f, 720.f});
            break;
    }

    // setup top and bottom bar
    m_top_bar = Vec4{m_pos.x + 15.f, 86.f, m_pos.w - 30.f, 1.f};
    m_bottom_bar = Vec4{m_pos.x + 15.f, 646.f, m_pos.w - 30.f, 1.f};
    m_title_pos = Vec2{m_pos.x + 30.f, m_pos.y + 40.f};
    m_base_pos = Vec4{GetX() + 30.f, GetY() + 170.f, m_pos.w - (30.f * 2.f), 70.f};

    const Vec4 pos = DistanceBetweenY(m_top_bar, m_bottom_bar);
    m_list = std::make_unique<List>(pos, m_base_pos);

    this->SetActions(
        std::make_pair(Button::DOWN, Action{[this](){
            auto index = m_index;
            if (ScrollHelperDown(index, m_index_offset, 1, 1, 6, m_items.size())) {
                SetIndex(index);
            }
        }}),
        std::make_pair(Button::UP, Action{[this](){
            auto index = m_index;
            if (ScrollHelperUp(index, m_index_offset, 1, 1, 6, m_items.size())) {
                SetIndex(index);
            }
        }}),
        // each item has it's own Action, but we take over B
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );
}

Sidebar::Sidebar(std::string title, std::string sub, Side side)
: Sidebar{std::move(title), sub, side, {}} {
}


auto Sidebar::Update(Controller* controller, TouchInfo* touch) -> void {
    m_items[m_index]->Update(controller, touch);
    const auto old_index = m_index;
    Widget::Update(controller, touch);

    if (touch->is_clicked && touch->in_range(GetPos())) {
        m_list->Do(m_index_offset, m_items.size(), [this, touch](auto* vg, auto* theme, auto v, auto i) {
            const auto& [x, y, w, h] = v;
            m_items[i]->SetY(y);
            if (touch->in_range(m_items[i]->GetPos())) {
                SetIndex(i);
                FireAction(Button::A);
                m_items[m_index]->FireAction(Button::A);
                return false;
            }
            return true;
        });
    } else if (touch->is_clicked && !touch->in_range(GetPos())) {
        App::PopToMenu();
    }

    Actions draw_actions{m_actions};
    const auto& actions_ref = m_items[m_index]->GetActions();
    draw_actions.insert(actions_ref.cbegin(), actions_ref.cend());

    const auto d = GetUiButtons(draw_actions, m_pos.x + m_pos.w - 60.f);
    for (auto& e : d) {
        if (touch->is_clicked && touch->in_range(e.GetPos())) {
            FireAction(e.m_button);
            m_items[m_index]->FireAction(e.m_button);
        }
    }

    if (m_items[m_index]->ShouldPop()) {
        SetPop();
    }
}

auto Sidebar::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::drawRect(vg, m_pos, nvgRGBA(0, 0, 0, 220));
    gfx::drawText(vg, m_title_pos, m_title_size, theme->elements[ThemeEntryID_TEXT].colour, m_title.c_str());
    if (!m_sub.empty()) {
        gfx::drawTextArgs(vg, m_pos.x + m_pos.w - 30.f, m_title_pos.y + 10.f, 18, NVG_ALIGN_TOP | NVG_ALIGN_RIGHT, theme->elements[ThemeEntryID_TEXT].colour, m_sub.c_str());
    }
    gfx::drawRect(vg, m_top_bar, theme->elements[ThemeEntryID_TEXT].colour);
    gfx::drawRect(vg, m_bottom_bar, theme->elements[ThemeEntryID_TEXT].colour);

    // only draw scrollbar if needed
    const auto dist = DistanceBetweenY(m_top_bar, m_bottom_bar);
    gfx::drawScrollbar(vg, theme, GetX() + GetW() - 20, m_base_pos.y - 10, dist.h - m_base_pos.y + 48, m_index_offset, m_items.size(), 6);

    m_list->Do(vg, theme, m_index_offset, m_items.size(), [this](auto* vg, auto* theme, auto v, auto i) {
        const auto& [x, y, w, h] = v;
        m_items[i]->SetY(y);
        m_items[i]->Draw(vg, theme);
        return true;
    });

    // draw the buttons. fetch the actions from current item and insert into array.
    Actions draw_actions{m_actions};
    const auto& actions_ref = m_items[m_index]->GetActions();
    draw_actions.insert(actions_ref.cbegin(), actions_ref.cend());

    auto d = GetUiButtons(draw_actions, m_pos.x + m_pos.w - 60.f);
    for (auto& e : d) {
        e.Draw(vg, theme);
    }

    // gfx::drawButtons(vg, draw_actions, theme->elements[ThemeEntryID_TEXT].colour, m_pos.x + m_pos.w - 60.f);
}

auto Sidebar::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
    SetHidden(false);
}

auto Sidebar::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    SetHidden(true);
}

void Sidebar::Add(std::shared_ptr<SidebarEntryBase> entry) {
    m_items.emplace_back(entry);
    m_items.back()->SetPos(m_base_pos);

    // give focus to first entry.
    m_items[m_index]->OnFocusGained();
}

void Sidebar::SetIndex(std::size_t index) {
    // if we moved
    if (m_index != index) {
        m_items[m_index]->OnFocusLost();
        m_index = index;
        m_items[m_index]->OnFocusGained();

        if (m_index > m_index_offset && m_index - m_index_offset >= 5) {
            m_index_offset = m_index - 5;
        }
    }
}

} // namespace sphaira::ui
