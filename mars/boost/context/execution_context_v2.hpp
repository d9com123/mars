
//          Copyright Oliver Kowalke 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_CONTEXT_EXECUTION_CONTEXT_H
#define BOOST_CONTEXT_EXECUTION_CONTEXT_H

#include <boost/context/detail/config.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <ostream>
#include <tuple>
#include <utility>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/intrusive_ptr.hpp>

#include <boost/context/detail/apply.hpp>
#include <boost/context/detail/disable_overload.hpp>
#include <boost/context/detail/exception.hpp>
#include <boost/context/detail/exchange.hpp>
#include <boost/context/detail/fcontext.hpp>
#include <boost/context/detail/tuple.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/flags.hpp>
#include <boost/context/preallocated.hpp>
#include <boost/context/segmented_stack.hpp>
#include <boost/context/stack_context.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_PREFIX
#endif

namespace mars_boost {} namespace boost = mars_boost; namespace mars_boost {
namespace context {
namespace detail {

inline
transfer_t context_unwind( transfer_t t) {
    throw forced_unwind( t.fctx);
    return { nullptr, nullptr };
}

template< typename Rec >
transfer_t context_exit( transfer_t t) noexcept {
    Rec * rec = static_cast< Rec * >( t.data);
    // destroy context stack
    rec->deallocate();
    return { nullptr, nullptr };
}

template< typename Rec >
void context_entry( transfer_t t_) noexcept {
    // transfer control structure to the context-stack
    Rec * rec = static_cast< Rec * >( t_.data);
    BOOST_ASSERT( nullptr != rec);
    transfer_t t = { nullptr, nullptr };
    try {
        // jump back to `context_create()`
        t = jump_fcontext( t_.fctx, nullptr);
        // start executing
        t = rec->run( t);
    } catch ( forced_unwind const& e) {
        t = { e.fctx, nullptr };
    }
    BOOST_ASSERT( nullptr != t.fctx);
    // destroy context-stack of `this`context on next context
    ontop_fcontext( t.fctx, rec, context_exit< Rec >);
    BOOST_ASSERT_MSG( false, "context already terminated");
}

template< typename Ctx, typename Fn, typename ... Args >
transfer_t context_ontop( transfer_t t) {
    auto tpl = static_cast< std::tuple< Fn, std::tuple< Args ... > > * >( t.data);
    BOOST_ASSERT( nullptr != tpl);
    typename std::decay< Fn >::type fn = std::forward< Fn >( std::get< 0 >( * tpl) );
    auto args = std::move( std::get< 1 >( * tpl) );
    Ctx ctx{ t.fctx };
    // execute function
    auto result = apply(
            fn,
            std::tuple_cat(
                std::forward_as_tuple( std::move( ctx) ),
                std::move( args) ) );
    ctx = std::move( std::get< 0 >( result) );
    // apply returned data
    detail::tail( args) = std::move( result);
    std::get< 1 >( * tpl) = std::move( args);
    return { exchange( ctx.fctx_, nullptr), & std::get< 1 >( * tpl) };
}

template< typename Ctx, typename StackAlloc, typename Fn, typename ... Params >
class record {
private:
    StackAlloc                                          salloc_;
    stack_context                                       sctx_;
    typename std::decay< Fn >::type                     fn_;
    std::tuple< typename std::decay< Params >::type ... > params_;

    static void destroy( record * p) noexcept {
        StackAlloc salloc = p->salloc_;
        stack_context sctx = p->sctx_;
        // deallocate record
        p->~record();
        // destroy stack with stack allocator
        salloc.deallocate( sctx);
    }

public:
    record( stack_context sctx, StackAlloc const& salloc,
            Fn && fn, Params && ... params) noexcept :
        salloc_( salloc),
        sctx_( sctx),
        fn_( std::forward< Fn >( fn) ),
        params_( std::forward< Params >( params) ... ) {
    }

    record( record const&) = delete;
    record & operator=( record const&) = delete;

    void deallocate() noexcept {
        destroy( this);
    }

    transfer_t run( transfer_t t) {
        Ctx from{ t.fctx };
        typename Ctx::args_tpl_t args = std::move( * static_cast< typename Ctx::args_tpl_t * >( t.data) );
        auto tpl = std::tuple_cat(
                    params_,
                    std::forward_as_tuple( std::move( from) ),
                    std::move( args) );
        // invoke context-function
        Ctx cc = apply( std::move( fn_), std::move( tpl) );
        return { exchange( cc.fctx_, nullptr), nullptr };
    }
};

template< typename Ctx, typename StackAlloc, typename Fn, typename ... Params >
fcontext_t context_create( StackAlloc salloc, Fn && fn, Params && ... params) {
    typedef record< Ctx, StackAlloc, Fn, Params ... >  record_t;

    auto sctx = salloc.allocate();
    // reserve space for control structure
#if defined(BOOST_NO_CXX11_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
    const std::size_t size = sctx.size - sizeof( record_t);
    void * sp = static_cast< char * >( sctx.sp) - sizeof( record_t);
#else
    constexpr std::size_t func_alignment = 64; // alignof( record_t);
    constexpr std::size_t func_size = sizeof( record_t);
    // reserve space on stack
    void * sp = static_cast< char * >( sctx.sp) - func_size - func_alignment;
    // align sp pointer
    std::size_t space = func_size + func_alignment;
    sp = std::align( func_alignment, func_size, sp, space);
    BOOST_ASSERT( nullptr != sp);
    // calculate remaining size
    const std::size_t size = sctx.size - ( static_cast< char * >( sctx.sp) - static_cast< char * >( sp) );
#endif
    // create fast-context
    const fcontext_t fctx = make_fcontext( sp, size, & context_entry< record_t >);
    BOOST_ASSERT( nullptr != fctx);
    // placment new for control structure on context-stack
    auto rec = ::new ( sp) record_t{
            sctx, salloc, std::forward< Fn >( fn), std::forward< Params >( params) ... };
    // transfer control structure to context-stack
    return jump_fcontext( fctx, rec).fctx;
}

template< typename Ctx, typename StackAlloc, typename Fn, typename ... Params >
fcontext_t context_create( preallocated palloc, StackAlloc salloc, Fn && fn, Params && ... params) {
    typedef record< Ctx, StackAlloc, Fn, Params ... >  record_t;

    // reserve space for control structure
#if defined(BOOST_NO_CXX11_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
    const std::size_t size = palloc.size - sizeof( record_t);
    void * sp = static_cast< char * >( palloc.sp) - sizeof( record_t);
#else
    constexpr std::size_t func_alignment = 64; // alignof( record_t);
    constexpr std::size_t func_size = sizeof( record_t);
    // reserve space on stack
    void * sp = static_cast< char * >( palloc.sp) - func_size - func_alignment;
    // align sp pointer
    std::size_t space = func_size + func_alignment;
    sp = std::align( func_alignment, func_size, sp, space);
    BOOST_ASSERT( nullptr != sp);
    // calculate remaining size
    const std::size_t size = palloc.size - ( static_cast< char * >( palloc.sp) - static_cast< char * >( sp) );
#endif
    // create fast-context
    const fcontext_t fctx = make_fcontext( sp, size, & context_entry< record_t >);
    BOOST_ASSERT( nullptr != fctx);
    // placment new for control structure on context-stack
    auto rec = ::new ( sp) record_t{
            palloc.sctx, salloc, std::forward< Fn >( fn), std::forward< Params >( params) ... };
    // transfer control structure to context-stack
    return jump_fcontext( fctx, rec).fctx;
}

}

template< typename ... Args >
class execution_context {
private:
    typedef std::tuple< Args ... >     args_tpl_t;
    typedef std::tuple< execution_context, typename std::decay< Args >::type ... >               ret_tpl_t;

    template< typename Ctx, typename StackAlloc, typename Fn, typename ... Params >
    friend class detail::record;

    template< typename Ctx, typename Fn, typename ... ArgsT >
    friend detail::transfer_t detail::context_ontop( detail::transfer_t);

    detail::fcontext_t  fctx_{ nullptr };

    execution_context( detail::fcontext_t fctx) noexcept :
        fctx_( fctx) {
    }

public:
    constexpr execution_context() noexcept = default;

#if defined(BOOST_USE_SEGMENTED_STACKS)
    // segmented-stack requires to preserve the segments of the `current` context
    // which is not possible (no global pointer to current context)
    template< typename Fn, typename ... Params >
    execution_context( std::allocator_arg_t, segmented_stack, Fn &&, Params && ...) = delete;

    template< typename Fn, typename ... Params >
    execution_context( std::allocator_arg_t, preallocated, segmented_stack, Fn &&, Params && ...) = delete;
#else
    template< typename Fn,
              typename ... Params,
              typename = detail::disable_overload< execution_context, Fn >
    >
    execution_context( Fn && fn, Params && ... params) :
        // deferred execution of fn and its arguments
        // arguments are stored in std::tuple<>
        // non-type template parameter pack via std::index_sequence_for<>
        // preserves the number of arguments
        // used to extract the function arguments from std::tuple<>
        fctx_( detail::context_create< execution_context >(
                    fixedsize_stack(),
                    std::forward< Fn >( fn),
                    std::forward< Params >( params) ... ) ) {
    }

    template< typename StackAlloc,
              typename Fn,
              typename ... Params
    >
    execution_context( std::allocator_arg_t, StackAlloc salloc, Fn && fn, Params && ... params) :
        // deferred execution of fn and its arguments
        // arguments are stored in std::tuple<>
        // non-type template parameter pack via std::index_sequence_for<>
        // preserves the number of arguments
        // used to extract the function arguments from std::tuple<>
        fctx_( detail::context_create< execution_context >(
                    salloc,
                    std::forward< Fn >( fn),
                    std::forward< Params >( params) ... ) ) {
    }

    template< typename StackAlloc,
              typename Fn,
              typename ... Params
    >
    execution_context( std::allocator_arg_t, preallocated palloc, StackAlloc salloc, Fn && fn, Params && ... params) :
        // deferred execution of fn and its arguments
        // arguments are stored in std::tuple<>
        // non-type template parameter pack via std::index_sequence_for<>
        // preserves the number of arguments
        // used to extract the function arguments from std::tuple<>
        fctx_( detail::context_create< execution_context >(
                    palloc, salloc,
                    std::forward< Fn >( fn),
                    std::forward< Params >( params) ... ) ) {
    }
#endif

    ~execution_context() {
        if ( nullptr != fctx_) {
            detail::ontop_fcontext( detail::exchange( fctx_, nullptr), nullptr, detail::context_unwind);
        }
    }

    execution_context( execution_context && other) noexcept :
        fctx_( other.fctx_) {
        other.fctx_ = nullptr;
    }

    execution_context & operator=( execution_context && other) noexcept {
        if ( this != & other) {
            execution_context tmp = std::move( other);
            swap( tmp);
        }
        return * this;
    }

    execution_context( execution_context const& other) noexcept = delete;
    execution_context & operator=( execution_context const& other) noexcept = delete;

    ret_tpl_t operator()( Args ... args) {
        BOOST_ASSERT( nullptr != fctx_);
        args_tpl_t data( std::forward< Args >( args) ... );
        detail::transfer_t t = detail::jump_fcontext( detail::exchange( fctx_, nullptr), & data);
        if ( nullptr != t.data) {
            data = std::move( * static_cast< args_tpl_t * >( t.data) );
        }
        return std::tuple_cat( std::forward_as_tuple( execution_context( t.fctx) ), std::move( data) );
    }

    template< typename Fn >
    ret_tpl_t operator()( exec_ontop_arg_t, Fn && fn, Args ... args) {
        BOOST_ASSERT( nullptr != fctx_);
        args_tpl_t data{ std::forward< Args >( args) ... };
        auto p = std::make_tuple( fn, std::move( data) );
        detail::transfer_t t = detail::ontop_fcontext(
                detail::exchange( fctx_, nullptr),
                & p,
                detail::context_ontop< execution_context, Fn, Args ... >);
        if ( nullptr != t.data) {
            data = std::move( * static_cast< args_tpl_t * >( t.data) );
        }
        return std::tuple_cat( std::forward_as_tuple( execution_context( t.fctx) ), std::move( data) );
    }

    explicit operator bool() const noexcept {
        return nullptr != fctx_;
    }

    bool operator!() const noexcept {
        return nullptr == fctx_;
    }

    bool operator==( execution_context const& other) const noexcept {
        return fctx_ == other.fctx_;
    }

    bool operator!=( execution_context const& other) const noexcept {
        return fctx_ != other.fctx_;
    }

    bool operator<( execution_context const& other) const noexcept {
        return fctx_ < other.fctx_;
    }

    bool operator>( execution_context const& other) const noexcept {
        return other.fctx_ < fctx_;
    }

    bool operator<=( execution_context const& other) const noexcept {
        return ! ( * this > other);
    }

    bool operator>=( execution_context const& other) const noexcept {
        return ! ( * this < other);
    }

    template< typename charT, class traitsT >
    friend std::basic_ostream< charT, traitsT > &
    operator<<( std::basic_ostream< charT, traitsT > & os, execution_context const& other) {
        if ( nullptr != other.fctx_) {
            return os << other.fctx_;
        } else {
            return os << "{not-a-context}";
        }
    }

    void swap( execution_context & other) noexcept {
        std::swap( fctx_, other.fctx_);
    }
};

#include <boost/context/execution_context_v2_void.ipp>

template< typename ... Args >
void swap( execution_context< Args ... > & l, execution_context< Args ... > & r) noexcept {
    l.swap( r);
}

}}

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_CONTEXT_EXECUTION_CONTEXT_H
