#pragma once

#include "../../../../common/utils.hpp"

#include <boost/system/error_code.hpp>

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace pldm::utils
{

class TestLoggingAsioConnection
{
  public:
    template <typename Callback, typename... Args>
    void async_method_call(Callback&& callback, Args&&...)
    {
        if constexpr (std::is_invocable_v<Callback, boost::system::error_code>)
        {
            std::forward<Callback>(callback)(boost::system::error_code{});
        }
        else
        {
            // Callback expects additional return values (e.g. mapper
            // response); invoke with default-constructed arguments to
            // indicate an empty / no-match result.
            invokeWithDefaults(std::forward<Callback>(callback),
                               boost::system::error_code{});
        }
    }

    template <typename Callback, typename... Args>
    void async_method_call_timed(Callback&& callback, Args&&...)
    {
        std::forward<Callback>(callback)(boost::system::error_code{});
    }

  private:
    template <typename Callback, typename... Supplied>
    static void invokeWithDefaults(Callback&& cb, Supplied&&... supplied)
    {
        invokeWithDefaultsImpl(std::forward<Callback>(cb),
                               std::index_sequence_for<Supplied...>{},
                               std::forward<Supplied>(supplied)...);
    }

    template <typename F>
    struct CallableTraits : CallableTraits<decltype(&F::operator())>
    {};

    template <typename C, typename R, typename... A>
    struct CallableTraits<R (C::*)(A...)>
    {
        using ArgsTuple = std::tuple<std::decay_t<A>...>;
        static constexpr size_t arity = sizeof...(A);
    };

    template <typename C, typename R, typename... A>
    struct CallableTraits<R (C::*)(A...) const>
    {
        using ArgsTuple = std::tuple<std::decay_t<A>...>;
        static constexpr size_t arity = sizeof...(A);
    };

    template <typename Callback, size_t... SuppliedIdx, typename... Supplied>
    static void invokeWithDefaultsImpl(Callback&& cb,
                                       std::index_sequence<SuppliedIdx...>,
                                       Supplied&&... supplied)
    {
        using Traits = CallableTraits<std::decay_t<Callback>>;
        constexpr size_t total = Traits::arity;
        constexpr size_t numSupplied = sizeof...(Supplied);

        if constexpr (total <= numSupplied)
        {
            std::forward<Callback>(cb)(std::forward<Supplied>(supplied)...);
        }
        else
        {
            fillRemaining<Callback, total, numSupplied>(
                std::forward<Callback>(cb),
                std::make_index_sequence<total - numSupplied>{},
                std::forward<Supplied>(supplied)...);
        }
    }

    template <typename Callback, size_t Total, size_t NumSupplied,
              size_t... ExtraIdx, typename... Supplied>
    static void fillRemaining(Callback&& cb, std::index_sequence<ExtraIdx...>,
                              Supplied&&... supplied)
    {
        using Traits = CallableTraits<std::decay_t<Callback>>;
        using ArgsTuple = typename Traits::ArgsTuple;
        std::forward<Callback>(
            cb)(std::forward<Supplied>(supplied)...,
                std::tuple_element_t<NumSupplied + ExtraIdx, ArgsTuple>{}...);
    }
};

class DBusLoggingTestHandler : public DBusHandler
{
  public:
    static auto& getAsioConnection()
    {
        static auto conn = std::make_shared<TestLoggingAsioConnection>();
        return conn;
    }
};

} // namespace pldm::utils

#define DBusHandler DBusLoggingTestHandler
