#pragma once

#include <optional>
#include <string>
#include <variant>

namespace pldm::dbus_accessors
{

/** @brief Typed, exception-free getter for a value held by a std::variant.
 *
 *  Returns the held value as std::optional<T>. If the variant does not
 *  currently hold a T (or is value-less by exception), returns std::nullopt
 *  instead of throwing std::bad_variant_access.
 *
 *  Per unify-mctp_discovery_guidelines.md § 2.3 recommended item 12 and
 *  § 2.4 anti-pattern "Use raw `std::get<>` on D-Bus variants", consumer
 *  code should call this helper instead of `std::get<T>(v)` on values
 *  reaching us from sdbusplus / D-Bus signal payloads. The unsafe pattern
 *  becomes not-expressible at the call site.
 *
 *  Marked `noexcept` so the call cannot itself become a new exception
 *  source; marked `[[nodiscard]]` so callers cannot silently drop the
 *  optional and re-introduce an implicit-assumption bug.
 */
template <typename T, typename Variant>
[[nodiscard]] std::optional<T> tryGet(const Variant& v) noexcept
{
    if (const auto* p = std::get_if<T>(&v))
    {
        return *p;
    }
    return std::nullopt;
}

/** @brief Typed, exception-free property lookup against a D-Bus property
 *  map (key → variant).
 *
 *  Returns the T value associated with `key` as std::optional<T>:
 *    - std::nullopt if `key` is not present in the map;
 *    - std::nullopt if the variant at `key` holds an alternative other
 *      than T;
 *    - the held T value otherwise.
 *
 *  Together with `tryGet`, replaces the
 *  `std::get<T>(map.at(key))` pattern that throws on either missing key
 *  or wrong-typed variant.
 */
template <typename T, typename PropertyMap>
[[nodiscard]] std::optional<T> tryGetProp(const PropertyMap& props,
                                          const std::string& key) noexcept
{
    const auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    return tryGet<T>(it->second);
}

} // namespace pldm::dbus_accessors
