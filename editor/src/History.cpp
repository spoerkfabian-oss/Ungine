#include "History.h"

namespace Engine {

namespace {
const std::string kEmpty;
} // namespace

void History::Push(EditCommand command)
{
    if (m_SavedPosition > m_Position && m_SavedPosition != kUnreachable)
        m_SavedPosition = kUnreachable; // the saved state was on the dropped redo branch
    m_Commands.resize(m_Position);
    m_Commands.push_back(std::move(command));
    if (m_Commands.size() > kMaxCommands) {
        m_Commands.erase(m_Commands.begin());
        m_SavedPosition = m_SavedPosition == 0 || m_SavedPosition == kUnreachable ? kUnreachable : m_SavedPosition - 1;
    }
    m_Position = m_Commands.size();
    ++m_Revision;
}

bool History::Undo()
{
    if (!CanUndo())
        return false;
    m_Commands[--m_Position].undo();
    ++m_Revision;
    return true;
}

bool History::Redo()
{
    if (!CanRedo())
        return false;
    m_Commands[m_Position++].redo();
    ++m_Revision;
    return true;
}

void History::Clear()
{
    m_Commands.clear();
    m_Position      = 0;
    m_SavedPosition = 0;
    ++m_Revision;
}

const std::string& History::UndoLabel() const
{
    return CanUndo() ? m_Commands[m_Position - 1].label : kEmpty;
}

const std::string& History::RedoLabel() const
{
    return CanRedo() ? m_Commands[m_Position].label : kEmpty;
}

} // namespace Engine
