#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Engine {

// One undoable editor operation. Commands reference entities by UUID: handles change when an
// entity is destroyed and re-created by an undo.
struct EditCommand {
    std::string           label;
    std::function<void()> undo;
    std::function<void()> redo;
};

// Linear undo/redo stack. Commands are pushed after they were executed; pushing drops the redo
// branch. Keeps the newest kMaxCommands.
class History {
public:
    static constexpr std::size_t kMaxCommands = 256;

    void Push(EditCommand command);
    bool Undo();
    bool Redo();
    void Clear();

    [[nodiscard]] bool               CanUndo() const { return m_Position > 0; }
    [[nodiscard]] bool               CanRedo() const { return m_Position < m_Commands.size(); }
    [[nodiscard]] const std::string& UndoLabel() const;
    [[nodiscard]] const std::string& RedoLabel() const;

    // Unsaved changes: the position differs from the one at the last MarkSaved (or Clear).
    [[nodiscard]] bool Dirty() const { return m_Position != m_SavedPosition; }
    void               MarkSaved() { m_SavedPosition = m_Position; }
    // Changes with every Push / Undo / Redo / Clear (follow-up work such as construction scripts).
    [[nodiscard]] std::uint64_t Revision() const { return m_Revision; }

private:
    static constexpr std::size_t kUnreachable = std::numeric_limits<std::size_t>::max();

    std::vector<EditCommand> m_Commands;
    std::size_t              m_Position      = 0; // commands [0, position) are applied
    std::size_t              m_SavedPosition = 0;
    std::uint64_t            m_Revision      = 0;
};

} // namespace Engine
