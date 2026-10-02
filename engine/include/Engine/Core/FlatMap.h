#pragma once
#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Engine {

// Sorted vector map with the part of std::map's interface the engine uses (iteration in key
// order, find / [] / at / erase by key). Unlike std::map, whose move constructor allocates in
// MSVC's standard library, it moves without throwing - so it may be a member of ECS components
// (see the Component concept in ECS/Registry.h). Lookups accept any key comparable with Less.
template <class Key, class Value, class Less = std::less<>>
class FlatMap {
public:
    using key_type       = Key;
    using mapped_type    = Value;
    using value_type     = std::pair<Key, Value>;
    using iterator       = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    FlatMap() = default;
    FlatMap(std::initializer_list<value_type> items)
    {
        for (const value_type& item : items)
            insert_or_assign(item.first, item.second);
    }

    [[nodiscard]] iterator       begin() noexcept { return m_Items.begin(); }
    [[nodiscard]] iterator       end() noexcept { return m_Items.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return m_Items.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_Items.end(); }
    [[nodiscard]] std::size_t    size() const noexcept { return m_Items.size(); }
    [[nodiscard]] bool           empty() const noexcept { return m_Items.empty(); }
    void                         clear() noexcept { m_Items.clear(); }

    template <class K>
    [[nodiscard]] iterator find(const K& key)
    {
        const auto it = LowerBound(m_Items, key);
        return it != m_Items.end() && !Less{}(key, it->first) ? it : m_Items.end();
    }
    template <class K>
    [[nodiscard]] const_iterator find(const K& key) const
    {
        const auto it = LowerBound(m_Items, key);
        return it != m_Items.end() && !Less{}(key, it->first) ? it : m_Items.end();
    }
    template <class K>
    [[nodiscard]] bool contains(const K& key) const { return find(key) != end(); }

    template <class K>
    [[nodiscard]] Value& at(const K& key)
    {
        const auto it = find(key);
        if (it == end())
            throw std::out_of_range("FlatMap::at: no such key");
        return it->second;
    }
    template <class K>
    [[nodiscard]] const Value& at(const K& key) const
    {
        const auto it = find(key);
        if (it == end())
            throw std::out_of_range("FlatMap::at: no such key");
        return it->second;
    }

    Value& operator[](const Key& key) { return Slot(key); }
    Value& operator[](Key&& key) { return Slot(std::move(key)); }

    template <class V>
    Value& insert_or_assign(const Key& key, V&& value)
    {
        Value& slot = Slot(key);
        slot        = std::forward<V>(value);
        return slot;
    }

    // By key (not by iterator: that overload below takes precedence for iterators).
    template <class K>
        requires(!std::is_convertible_v<const K&, const_iterator>)
    std::size_t erase(const K& key)
    {
        const auto it = find(key);
        if (it == end())
            return 0;
        m_Items.erase(it);
        return 1;
    }
    iterator erase(const_iterator it) { return m_Items.erase(it); }

    bool operator==(const FlatMap&) const = default;

private:
    template <class Items, class K>
    static auto LowerBound(Items& items, const K& key)
    {
        return std::lower_bound(items.begin(), items.end(), key,
                                [](const value_type& item, const K& k) { return Less{}(item.first, k); });
    }
    template <class K>
    Value& Slot(K&& key)
    {
        auto it = LowerBound(m_Items, key);
        if (it == m_Items.end() || Less{}(key, it->first))
            it = m_Items.emplace(it, Key(std::forward<K>(key)), Value{});
        return it->second;
    }

    std::vector<value_type> m_Items;
};

} // namespace Engine
