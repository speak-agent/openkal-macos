#include "sys.h"
#include <openkal/exec.h>
#include <openkal/memory.h>

// openkal.exec on this system.
//
// A mapping obtained writable and made executable afterwards, which is the
// order the interface requires and the only order this system permits: a
// mapping that is writable and executable at once is refused here, and the
// interface states the narrower contract for exactly that reason.
//
// CLAUSE 6.5 NAMES THIS SYSTEM AND THIS INTERFACE IN TERMS, and what it
// says is that availability may be settled by how the artifact is PRODUCED ---
// a signed declaration applied after the link, by whoever produces the
// artifact. That would make the interface a feature of this package rather than
// a part of it.
//
// THIS IMPLEMENTATION PROVIDES IT UNCONDITIONALLY, AND THE READING IS A
// MEASUREMENT RATHER THAN A CHOICE. The declaration clause 6.5 describes is
// what a program needs to obtain memory that is writable and executable AT THE
// SAME TIME --- `MAP_JIT' and the pair of calls that flip one region between
// the two. It is not what a program needs to obtain memory that is writable,
// then made executable and no longer writable, which is the whole of what this
// interface offers.
//
// THE SPECIFICATION'S OWN SUITE IS WHERE THAT DISTINCTION STOPS BEING AN
// ARGUMENT --- `conformance/src/sections/exec.cpp' in the openkal repository,
// which this package runs through `run-conformance.sh openkal-macos .
// full,optional'. It reserves a region, writes an instruction sequence into it,
// publishes it, CALLS it, and compares what it returned.
//
// An earlier version of this comment named `tests/conformance_exec.cpp',
// which is in this repository and does not exist. A comment naming the place a
// question is answered is a promise, and one pointing at nothing sends a reader
// to look and leaves them unable to tell whether the check is missing or the
// reference is.
//
// ⇒ If that section fails on this system, the reading above is wrong and the
// remedy is clause 6.5's: a feature of this package, provided at dependency
// resolution, together with whatever produces the artifact. The question is
// answered by the system rather than by this comment.
//
// THE INSTRUCTION CACHE IS NOT INVALIDATED HERE, AND ONE VERSION OF THIS
// FILE DID INVALIDATE IT. THE MEASUREMENT IS WHY IT DOES NOT.
//
// A processor with separate caches for data and instructions has just had bytes
// written through the data path that it is about to fetch through the
// instruction path, and nothing in `mprotect' makes the second path see the
// first's writes. So `__builtin___clear_cache' looked like the right thing to
// add, on the reading that it expands to nothing on x86_64 and to the
// maintenance sequence INLINE on aarch64.
//
// The second half of that reading was false, and this package's own
// independence check said so within the hour:
//
//     target/aarch64-macos/…/obj/exec.o references a symbol it must not:
//     ___clear_cache
//
// The builtin becomes a CALL into the compiler's support library on this
// architecture, exactly as it does on riscv64. This implementation is reachable
// from a program that carries no other runtime and the check exists to keep it
// that way, so acquiring that dependency is not available here.
//
// THE SPECIFICATION PLACES THE MAINTENANCE UPON THE PROGRAM in any case --- the
// conformance suite performs it itself and says why: the program is the party
// that knows which bytes it wrote. So nothing is lost by not doing it, and what
// the three implementations now share is a rule rather than an accident:
//
//     an implementation performs the maintenance where its environment offers
//     it as a CALL of the environment's own (openkal-windows has
//     `FlushInstructionCache'), and does not where the only means is a compiler
//     builtin that becomes a dependency upon the compiler's support library.
//
// openkal-linux/src/exec.cpp records the same rule from the other side.

namespace {

// THE GRANULARITY IS ASKED FOR RATHER THAN ASSUMED. This file held
// `constexpr okm_uptr kPage = 4096' and a comment arguing that rounding to the
// smaller of this system's two page sizes was still correct because the kernel
// rounds up. The argument holds for the reservation and fails for the release:
// `munmap' with a length shorter than the mapping unmaps less than was mapped.
// It is the same defect `src/memory.cpp' records having measured, in the same
// system, one file away --- so the number now comes from the one operation
// that answers it.
okm_uptr round_up(okm_uptr n, okm_uptr to) { return (n + to - 1) & ~(to - 1); }

okm_uptr granularity() {
    return static_cast<okm_uptr>(kal_memory_granularity());
}

// WHETHER THIS SYSTEM GRANTS EXECUTABLE MEMORY IS MEASURED, NOT ARGUED.
//
// This file previously carried both answers. One comment reasoned that the
// write-then-publish order is the case an entitlement is NOT needed for and
// concluded the interface is provided unconditionally; another, thirty lines
// below it, reasoned that executable memory is granted only to an artifact
// produced with an entitlement and returned zero. The operations behaved as
// the first said and the capability word said the second.
//
// AND THE DISAGREEMENT WAS INVISIBLE UNTIL A CONSUMER COULD READ THE WORD.
// A statically-linked caller never asked: it linked the operations and used
// them. The word became load-bearing when the specification made an
// implementation's own account of itself part of the ABI, and the conformance
// suite then reported what had been true all along --- `an implementation that
// does not claim availability reserves nothing' DID NOT HOLD, because this one
// claimed nothing and reserved anyway.
//
// The remedy is not to pick the more likely of the two readings. It is that
// neither this file nor any comment in it is the party that knows: the answer
// depends on how the artifact was signed, which is settled after this code is
// compiled and can differ between two runs of the same binary. So the enquiry
// performs the thing it is being asked about --- one reservation, one publish,
// one release --- and reports what the kernel said.
//
// The probe is the operation's own path, so an environment where publishing
// fails is one where this reports unavailable and `kal_exec_alloc' declines,
// and the two can no longer disagree.
int probe() {
    const okm_uptr bytes = granularity();
    const okm_long m = okm::sys(okm::nr_mmap, 0, static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_write,
                                okm::map_private | okm::map_anon, -1, 0);
    if (okm::failed(m)) return 2;
    const okm_long p = okm::sys(okm::nr_mprotect, m,
                                static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_exec);
    okm::sys(okm::nr_munmap, m, static_cast<okm_long>(bytes));
    return okm::failed(p) ? 2 : 1;
}

// Asked once. Constant-initialised, so no guard variable is emitted and this
// file acquires no dependency upon the runtime --- the property the
// independence check in this package exists to hold. Two contexts racing here
// perform the probe twice and store the same answer.
bool available() {
    static int cached = 0;
    int v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (v == 0) { v = probe(); __atomic_store_n(&cached, v, __ATOMIC_RELAXED); }
    return v == 1;
}

}  // namespace

extern "C" {

void* kal_exec_alloc(kal_uintptr size) {
    if (size == 0) return nullptr;
    // An implementation that does not claim availability reserves nothing.
    // Otherwise the word is advice a caller cannot act upon: it would report
    // unavailable and then hand back memory, and a caller that believed the
    // word would have declined memory it could have had.
    if (!available()) return nullptr;
    const okm_uptr bytes = round_up(static_cast<okm_uptr>(size), granularity());
    const okm_long r = okm::sys(okm::nr_mmap, 0, static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_write,
                                okm::map_private | okm::map_anon, -1, 0);
    if (okm::failed(r)) return nullptr;
    return reinterpret_cast<void*>(r);
}

int kal_exec_publish(void* p, kal_uintptr size) {
    if (p == nullptr || size == 0) return kal_err_invalid;
    const okm_uptr bytes = round_up(static_cast<okm_uptr>(size), granularity());
    const okm_long r = okm::sys(okm::nr_mprotect, reinterpret_cast<okm_long>(p),
                                static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_exec);
    if (okm::failed(r)) return okm::translate(r);
    return kal_ok;
}

void kal_exec_free(void* p, kal_uintptr size) {
    if (p == nullptr || size == 0) return;
    const okm_uptr bytes = round_up(static_cast<okm_uptr>(size), granularity());
    okm::sys(okm::nr_munmap, reinterpret_cast<okm_long>(p),
             static_cast<okm_long>(bytes));
}

// The partial form (openkal 0.15): the same protection call on part of the
// mapping, in the page this system protects in --- the granularity the rest of
// this file already works in.
kal_uintptr kal_exec_granularity(void) {
    return static_cast<kal_uintptr>(granularity());
}

int kal_exec_publish_part(void* p, kal_uintptr offset, kal_uintptr size) {
    const okm_uptr page = granularity();
    if (p == nullptr || size == 0 || offset % page != 0 || size % page != 0) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_mprotect,
                                reinterpret_cast<okm_long>(static_cast<unsigned char*>(p) + offset),
                                static_cast<okm_long>(size),
                                okm::prot_read | okm::prot_exec);
    if (okm::failed(r)) return okm::translate(r);
    return kal_ok;
}

// A published region may NOT be reserved for writing again on this system, and
// the position is withheld accordingly. Asking this kernel to make an
// executable mapping writable is the case it refuses, which is the whole reason
// the interface separates the two states; a caller that must change published
// bytes reserves a second region and abandons the first, which is what the
// header says a zero in that position means.
kal_uintptr kal_exec_props(void) {
    return available() ? KAL_EXEC_PROP_AVAILABLE : 0;
}

}  // extern "C"
