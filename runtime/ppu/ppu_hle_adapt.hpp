/*
 * ps3recomp - PPU HLE call adapter (PPC64 ELFv1 -> host C)
 *
 * ps3_hle_call dispatches a firmware import to a host C function. The host
 * function has its own declared C type; calling it through a single universal
 * function-pointer type is undefined behaviour and silently gets the ABI wrong
 * on every host that is not PPC64 (on arm64 an `s32` return arrives as a
 * zero-extended w0, and float parameters are never read from f1 at all).
 *
 * ps3_hle_adapt<&handler> derives the handler's exact signature from
 * `decltype(handler)` and marshals one guest call into one host call:
 *
 *   parameters, by 0-based POSITION i (PPC64 ELFv1):
 *     integral/enum/bool   gpr[3+i]; a type of 4 bytes or less is the low word
 *                          of the register (the caller extended it per its own
 *                          declared type, so no sign guessing here)
 *     pointer              low 32 bits of gpr[3+i], zero-extended. A guest
 *                          pointer is a 32-bit effective address; the handler
 *                          translates it with GUEST_PTR (runtime/ppu/ppu_memory.h)
 *                          exactly as the C libraries do today.
 *     float/double         the next FPR (fpr[1+k], k = number of floating-point
 *                          parameters before it), still consuming its
 *                          positional GPR slot
 *     i >= 8               the caller's parameter save area, a big-endian
 *                          doubleword at r1 + 48 + 8*i for integral/pointer
 *                          types (see libs/network/sceNpScore.c stack_arg,
 *                          which reads the ninth argument at r1+112)
 *
 *   return, by the handler's declared type:
 *     void                 r3 is not touched
 *     signed integral<=4   sign-extended into r3
 *     unsigned integral<=4 zero-extended into r3 (bool: 0/1)
 *     enum                 per its underlying type
 *     64-bit integral      r3 is the 64-bit value unchanged
 *     float/double         fpr[1]; r3 is not touched
 *     pointer              NO mapping here. A handler that returns a guest EA
 *                          opts in with ps3_hle_adapt_guest_ea (its low 32 bits
 *                          are zero-extended), and one that returns a host
 *                          pointer is registered with
 *                          ps3_hle_adapt_host_ptr_stop, which fails closed at
 *                          call time instead of handing the guest a truncated
 *                          host address. The classification lives in
 *                          tools/gen_hle_nids.py so every entry carries the
 *                          reason it is in its list.
 *
 * A signature with no defined mapping (reference, array, struct/union by value,
 * member pointer, variadic, more than 13 floating-point parameters) fails the
 * build with a static_assert naming the problem; the instantiation context
 * names the handler. Nothing is assumed about an unmapped type.
 *
 * Compiled into the generated registration unit and ppu_hle.cpp (C++20).
 */

#ifndef PS3RECOMP_PPU_HLE_ADAPT_HPP
#define PS3RECOMP_PPU_HLE_ADAPT_HPP

/* ppu_hle.cpp includes the lifter-generated ppu_recomp.h first, which defines
 * ppu_context itself (and PPU_CONTEXT_DEFINED). The generated registration
 * unit and the tests have no lifted header, so this is where they get the
 * runtime definition. The two layouts are identical. */
#ifndef PPU_CONTEXT_DEFINED
#  include "ppu_context.h"
#endif

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

/* The guest-memory doubleword read (runtime/ppu/ppu_loader.cpp): big-endian,
 * bounds-checked, 32-bit effective address. Used only by the i >= 8 path. */
extern "C" uint64_t vm_read64(uint64_t a);

/* Every registered handler is called through this one typed shape. */
typedef void (*ps3_hle_fn)(ppu_context* ctx);

/* Registration API (runtime/ppu/ppu_hle.cpp). Generated tables call it with
 * an adapter instantiation; nothing else may forge a handler. */
extern "C" void ps3_hle_register(uint32_t nid, const char* name, ps3_hle_fn handler);

/* Fails closed for a handler classified as returning a host pointer: the stop
 * names the NID/handler from the call in progress. Defined in ppu_hle.cpp. */
extern "C" void ps3_hle_stop_unsupported_return(ppu_context* ctx);

namespace ps3_hle_detail {

template <typename...>
struct pack {};

template <typename P>
struct pack_size;

template <typename... A>
struct pack_size<pack<A...>> : std::integral_constant<std::size_t, sizeof...(A)> {};

/* The handler's declared function type. A plain function pointer is the only
 * accepted shape; everything else stops here. */
template <typename T>
struct signature {
    static_assert(sizeof(T) == 0,
                  "HLE adapter: handler is not a plain function pointer "
                  "(function reference, member pointer, std::function, ...)");
};

template <typename R, typename... A>
struct signature<R (*)(A...)> {
    using ret  = R;
    using args = pack<A...>;
};

template <typename R, typename... A>
struct signature<R (*)(A...) noexcept> {
    using ret  = R;
    using args = pack<A...>;
};

template <typename R, typename... A>
struct signature<R (*)(A..., ...)> {
    static_assert(sizeof(R) == 0,
                  "HLE adapter: variadic handler has no PPC64 ELFv1 mapping "
                  "(named parameters of a variadic function are marshalled by the "
                  "same rules, but a guest call to the ... part is not modelled)");
};

template <typename R, typename... A>
struct signature<R (*)(A..., ...) noexcept> {
    static_assert(sizeof(R) == 0,
                  "HLE adapter: variadic handler has no PPC64 ELFv1 mapping");
};

/* A variadic handler's fixed part is typed; its trailing arguments are not,
 * so they get their own opt-in adapter rather than a static_assert. */
template <typename T>
struct variadic_signature {
    static_assert(sizeof(T) == 0, "HLE adapter: handler is not a plain function pointer");
};

template <typename R, typename... A>
struct variadic_signature<R (*)(A..., ...)> {
    using ret   = R;
    using fixed = pack<A...>;
};

enum class ret_policy { none, guest_ea, host_ptr_stop };

template <typename T>
constexpr bool is_integral_like = std::is_integral_v<T> || std::is_enum_v<T>;

/* One unsupported parameter class gets one message, so a build failure says
 * what the type is rather than only that it is unsupported. */
template <typename T>
constexpr void check_param()
{
    static_assert(!std::is_reference_v<T>,
                  "HLE adapter: a reference parameter has no PPC64 ELFv1 mapping "
                  "(take a pointer, which is the guest effective address)");
    static_assert(!std::is_array_v<T>,
                  "HLE adapter: an array parameter has no PPC64 ELFv1 mapping "
                  "(C already decays it to a pointer in the declaration)");
    static_assert(!std::is_member_pointer_v<T>,
                  "HLE adapter: a member pointer parameter has no PPC64 ELFv1 mapping");
    static_assert(!std::is_class_v<T> && !std::is_union_v<T>,
                  "HLE adapter: a struct/union parameter by value has no PPC64 ELFv1 "
                  "mapping (take a pointer: the guest effective address)");
    static_assert(is_integral_like<T> || std::is_floating_point_v<T> ||
                  std::is_pointer_v<T>,
                  "HLE adapter: parameter type has no PPC64 ELFv1 mapping");
}

template <typename R, ret_policy P>
constexpr void check_return()
{
    if constexpr (std::is_pointer_v<R>) {
        static_assert(P != ret_policy::none,
                      "HLE adapter: a pointer return has no defined mapping. A guest EA "
                      "needs ps3_hle_adapt_guest_ea and a host pointer needs "
                      "ps3_hle_adapt_host_ptr_stop; classify the handler in "
                      "tools/gen_hle_nids.py with a comment saying which it is");
    } else {
        static_assert(P == ret_policy::none,
                      "HLE adapter: this handler is classified as pointer-returning in "
                      "tools/gen_hle_nids.py but its declared return type is not a pointer");
        static_assert(!std::is_reference_v<R>,
                      "HLE adapter: a reference return has no PPC64 ELFv1 mapping");
        static_assert(!std::is_array_v<R>,
                      "HLE adapter: an array return is not a C function result");
        static_assert(!std::is_member_pointer_v<R>,
                      "HLE adapter: a member pointer return has no PPC64 ELFv1 mapping");
        static_assert(!std::is_class_v<R> && !std::is_union_v<R>,
                      "HLE adapter: a struct/union return value has no PPC64 ELFv1 mapping "
                      "(the guest call returns a value in r3/f1, not in memory)");
        static_assert(is_integral_like<R> || std::is_floating_point_v<R> ||
                      std::is_void_v<R>,
                      "HLE adapter: return type has no PPC64 ELFv1 mapping");
    }
}

/* Instantiating check_args<A...> runs every parameter check. */
template <typename P>
struct check_args;

template <typename... A>
struct check_args<pack<A...>> {
    static constexpr bool ok = ((check_param<A>(), true) && ...);
};

/* Number of floating-point parameters before position I. */
template <typename... A>
constexpr std::size_t fp_before(std::size_t i)
{
    const bool is_fp[] = { std::is_floating_point_v<A>..., false };
    std::size_t n = 0;
    for (std::size_t k = 0; k < i; ++k) n += is_fp[k] ? 1u : 0u;
    return n;
}

/* One GPR slot, or the caller's parameter save area from the ninth argument on.
 * ELFv1 places the overflow arguments at r1 + 48 + 8*i in the CALLER's frame,
 * which is what r1 points at on entry to the callee (sceNpScore.c's stack_arg
 * reads argument 9, i = 8, at r1 + 112). */
template <std::size_t I>
uint64_t gpr_or_stack(ppu_context* ctx)
{
    if constexpr (I < 8)
        return ctx->gpr[3 + I];
    else
        return vm_read64(ctx->gpr[1] + 48 + 8 * I);
}

template <typename T>
T from_integer(uint64_t v)
{
    static_assert(!std::is_pointer_v<T>, "internal: pointer uses its own path");
    if constexpr (sizeof(T) <= 4)
        return static_cast<T>(static_cast<uint32_t>(v));
    else
        return static_cast<T>(v);
}

template <typename A, std::size_t I, typename... All>
A load_arg(ppu_context* ctx)
{
    constexpr std::size_t fpc = fp_before<All...>(I);
    if constexpr (std::is_floating_point_v<A>) {
        static_assert(fpc < 13,
                      "HLE adapter: more than 13 floating-point parameters "
                      "(the ABI has f1-f13)");
        return static_cast<A>(ctx->fpr[1 + fpc]);
    } else if constexpr (std::is_pointer_v<A>) {
        /* GUEST_PTR convention: a guest pointer is a 32-bit effective address. */
        return reinterpret_cast<A>(
            static_cast<uintptr_t>(static_cast<uint32_t>(gpr_or_stack<I>(ctx))));
    } else if constexpr (std::is_enum_v<A>) {
        using U = std::underlying_type_t<A>;
        return static_cast<A>(from_integer<U>(gpr_or_stack<I>(ctx)));
    } else {
        return from_integer<A>(gpr_or_stack<I>(ctx));
    }
}

template <typename R>
uint64_t to_r3(R v)
{
    if constexpr (std::is_enum_v<R>) {
        using U = std::underlying_type_t<R>;
        return to_r3<U>(static_cast<U>(v));
    } else {
        static_assert(std::is_integral_v<R>, "internal: non-integral uses its own path");
        if constexpr (std::is_signed_v<R>)
            return static_cast<uint64_t>(static_cast<int64_t>(v));
        else
            return static_cast<uint64_t>(v);
    }
}

template <auto Fn, ret_policy P, typename R, typename... A, std::size_t... I>
void invoke(ppu_context* ctx, pack<A...>*, std::index_sequence<I...>)
{
    (check_param<A>(), ...);
    if constexpr (std::is_void_v<R>) {
        Fn(load_arg<A, I, A...>(ctx)...);
    } else if constexpr (std::is_floating_point_v<R>) {
        ctx->fpr[1] = static_cast<double>(Fn(load_arg<A, I, A...>(ctx)...));
    } else if constexpr (std::is_pointer_v<R>) {
        static_assert(P == ret_policy::guest_ea,
                      "internal: a pointer return uses ps3_hle_adapt_guest_ea or "
                      "ps3_hle_adapt_host_ptr_stop");
        R v = Fn(load_arg<A, I, A...>(ctx)...);
        ctx->gpr[3] = static_cast<uint64_t>(
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v)));
    } else if constexpr (is_integral_like<R>) {
        ctx->gpr[3] = to_r3<R>(Fn(load_arg<A, I, A...>(ctx)...));
    }
    /* No final else: a return type with no mapping was already rejected by
     * check_return(), with a message that says what it is. */
}

template <auto Fn, ret_policy P>
void adapt(ppu_context* ctx)
{
    using sig = signature<decltype(Fn)>;
    check_return<typename sig::ret, P>();
    invoke<Fn, P, typename sig::ret>(
        ctx, static_cast<typename sig::args*>(nullptr),
        std::make_index_sequence<pack_size<typename sig::args>::value>{});
}

/* One variadic call: the fixed parameters by position, then the remaining
 * integer argument registers as trailing host arguments. */
template <std::size_t Nf, typename R, typename... A,
          std::size_t... FI, std::size_t... TI>
R call_variadic(R (*f)(A..., ...), ppu_context* ctx,
                std::index_sequence<FI...>, std::index_sequence<TI...>)
{
    return f(load_arg<A, FI, A...>(ctx)...,
             static_cast<uint64_t>(ctx->gpr[3 + Nf + TI])...);
}

template <auto Fn, typename R, typename... A, std::size_t... I>
void invoke_variadic(ppu_context* ctx, pack<A...>*, std::index_sequence<I...>)
{
    (check_param<A>(), ...);
    constexpr std::size_t nf = sizeof...(A);
    static_assert(nf <= 8,
                  "HLE adapter: a variadic handler with more than 8 fixed parameters "
                  "would need arguments from the guest parameter save area, which this "
                  "forwarding does not model");
    R (*f)(A..., ...) = Fn;   /* the exact declared type: no incompatible call */
    constexpr std::size_t ntail = 8 - nf;
    if constexpr (std::is_void_v<R>) {
        call_variadic<nf, R>(f, ctx, std::index_sequence<I...>{},
                             std::make_index_sequence<ntail>{});
    } else if constexpr (is_integral_like<R>) {
        ctx->gpr[3] = to_r3<R>(call_variadic<nf, R>(
            f, ctx, std::index_sequence<I...>{}, std::make_index_sequence<ntail>{}));
    } else if constexpr (std::is_floating_point_v<R>) {
        ctx->fpr[1] = static_cast<double>(call_variadic<nf, R>(
            f, ctx, std::index_sequence<I...>{}, std::make_index_sequence<ntail>{}));
    }
}

} // namespace ps3_hle_detail

/* The ordinary handler: every parameter and return type with a defined
 * PPC64 ELFv1 mapping above, and no pointer return. */
template <auto Fn>
void ps3_hle_adapt(ppu_context* ctx)
{
    ps3_hle_detail::adapt<Fn, ps3_hle_detail::ret_policy::none>(ctx);
}

/* A variadic handler (a guest prints, formats, or logs). The fixed parameters
 * are marshalled by the rules above; the remaining integer argument registers
 * r(3+n)..r10 are forwarded as trailing uint64_t arguments, so the handler's
 * va_start/va_arg reads exactly the guest's GPR words. PPC64 ELFv1 has the
 * caller place a variadic float/double in the GPR image as well as in an FPR,
 * so a forwarded word carries a double bit-exactly wherever va_arg reads
 * variadic arguments from memory slots (Apple arm64); on x86-64 SysV doubles
 * are read from the XMM save area, which these words do not populate. Values
 * the guest passed beyond r10 (its parameter save area) are not forwarded, so
 * a format string must fit its arguments in the eight integer registers. */
template <auto Fn>
void ps3_hle_adapt_variadic(ppu_context* ctx)
{
    using sig = ps3_hle_detail::variadic_signature<decltype(Fn)>;
    ps3_hle_detail::check_return<typename sig::ret,
                                 ps3_hle_detail::ret_policy::none>();
    ps3_hle_detail::invoke_variadic<Fn, typename sig::ret>(
        ctx, static_cast<typename sig::fixed*>(nullptr),
        std::make_index_sequence<ps3_hle_detail::pack_size<typename sig::fixed>::value>{});
}

/* A handler whose declared pointer return is a GUEST effective address: the
 * low 32 bits are zero-extended into r3, exactly the address a guest
 * dereference would compute (guest memory is a 32-bit address space). */
template <auto Fn>
void ps3_hle_adapt_guest_ea(ppu_context* ctx)
{
    ps3_hle_detail::adapt<Fn, ps3_hle_detail::ret_policy::guest_ea>(ctx);
}

/* A handler whose declared pointer return is a HOST pointer. There is no
 * correct value to hand the guest, so it never runs: the call stops with
 * hle-unsupported-return instead of returning a truncated host address. */
template <auto Fn>
void ps3_hle_adapt_host_ptr_stop(ppu_context* ctx)
{
    using sig = ps3_hle_detail::signature<decltype(Fn)>;
    ps3_hle_detail::check_return<typename sig::ret,
                                 ps3_hle_detail::ret_policy::host_ptr_stop>();
    /* Still validate every parameter, so an unmappable signature fails the
     * build even though the handler never runs. */
    static_assert(ps3_hle_detail::check_args<typename sig::args>::ok,
                  "unreachable: an unsupported parameter already failed to compile");
    ps3_hle_stop_unsupported_return(ctx);
}

#endif /* PS3RECOMP_PPU_HLE_ADAPT_HPP */
