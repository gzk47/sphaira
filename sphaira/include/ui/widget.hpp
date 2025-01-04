#pragma once

#include "ui/object.hpp"
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>

namespace sphaira::ui {

struct uiButton final : Object {
    uiButton(Button button, Action action) : m_button{button}, m_action{action} {}
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    Button m_button;
    Action m_action;
    Vec4 m_button_pos{};
    Vec4 m_hint_pos{};
};

struct Widget : public Object {
    using Actions = std::map<Button, Action>;
    using uiButtons = std::vector<uiButton>;

    virtual ~Widget() = default;

    virtual void Update(Controller* controller, TouchInfo* touch);
    virtual void Draw(NVGcontext* vg, Theme* theme);

    virtual void OnFocusGained() {
        m_focus = true;
    }

    virtual void OnFocusLost() {
        m_focus = false;
    }

    virtual auto HasFocus() const -> bool {
        return m_focus;
    }

    virtual auto IsMenu() const -> bool {
        return false;
    }

    auto HasAction(Button button) const -> bool;
    void SetAction(Button button, Action action);
    void SetActions(std::same_as<std::pair<Button, Action>> auto ...args) {
        const std::array list = {args...};
        for (const auto& [button, action] : list) {
            SetAction(button, action);
        }
    }

    auto GetActions() const {
        return m_actions;
    }

    void RemoveAction(Button button);

    void RemoveActions() {
        m_actions.clear();
    }

    auto FireAction(Button button, u8 type = ActionType::DOWN) -> bool;

    void SetPop(bool pop = true) {
        m_pop = pop;
    }

    auto ShouldPop() const -> bool {
        return m_pop;
    }

    auto GetUiButtons() const -> uiButtons;
    static auto GetUiButtons(const Actions& actions, float x = 1220, float y = 675) -> uiButtons;

    static auto ScrollHelperDown(u64& index, u64& start, u64 step, s64 row, s64 page, u64 size) -> bool;
    static auto ScrollHelperUp(u64& index, u64& start, s64 step, s64 row, s64 page, s64 size) -> bool;

    Actions m_actions;
    bool m_focus{false};
    bool m_pop{false};
};

} // namespace sphaira::ui
