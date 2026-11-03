#include "Editor/History.h"

#include <algorithm>

namespace Engine {

namespace {
const std::string kEmpty;
} // namespace

bool History::Dirty() const
{
    if (m_SavedPosition == kUnreachable)
        return true;
    const std::size_t lo = std::min(m_Position, m_SavedPosition), hi = std::max(m_Position, m_SavedPosition);
    for (std::size_t i = lo; i < hi; ++i)
        if (m_Commands[i].sceneChange)
            return true;
    return false;
}

void History::Push(EditCommand command)
{
    if (m_SavedPosition > m_Position && m_SavedPosition != kUnreachable) {
        // The saved state was on the dropped redo branch: still reachable if that only changed
        // things outside the scene.
        bool scene = false;
        for (std::size_t i = m_Position; i < m_SavedPosition; ++i)
            scene = scene || m_Commands[i].sceneChange;
        m_SavedPosition = scene ? kUnreachable : m_Position;
    }
    m_Commands.resize(m_Position);
    m_Commands.push_back(std::move(command));
    if (m_Commands.size() > kMaxCommands) {
        const bool scene = m_Commands.front().sceneChange;
        m_Commands.erase(m_Commands.begin());
        if (m_SavedPosition != kUnreachable)
            m_SavedPosition = m_SavedPosition > 0 ? m_SavedPosition - 1 : scene ? kUnreachable : 0;
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
