#include "sys.h"
#include <openkal/task.h>
#include <openkal/memory.h>

// Execution contexts, and the primitive they are built upon.
//
// The primitive is a kernel call: this system has the same operation the other
// kernel's futex provides, under a different name and with no shared ancestry,
// and openkal's kal_task_wait is that operation. That two unrelated systems
// offer it is the evidence that it is the shape of the thing rather than the
// shape of one kernel.
//
// Contexts are not a kernel call. Creating one on this system means arranging
// state the kernel does not arrange, and the arrangement belongs to this
// system's thread library rather than to its kernel. Which of that library's
// names may be used is decided by one property: a program above openkal may
// itself define every ordinary name, so only a name no C library defines is
// reachable. `pthread_create_from_mach_thread' is such a name, and the
// measurement that established it is in .github/workflows/probe.yml.
//
// Where the program carries a runtime of its own --- the ordinary arrangement,
// and the default --- there is no such constraint and the ordinary names are
// used, which also avoids the cost the other arrangement pays.

// DECLARED, NOT INCLUDED — AND THAT IS THE SAME RULE THE BRANCH BELOW
// ALREADY FOLLOWS.
//
// `pthread_create_from_mach_thread` has always been declared here rather than
// taken from a header, because no header declares it. The three ordinary names
// were taken from `<pthread.h>`, and that header is this system's SDK — the one
// thing every other implementation in this ecosystem avoids: openkal-linux
// writes the system-call numbers, openkal-opensbi the SBI identifiers,
// openkal-windows the Win32 declarations.
//
// Measured 2026-08-23, building this package ON this system with the target
// side coming from the graph:
//
//     …/MacOSX.sdk/usr/include/mach/mach_time.h:61:1:
//       error: a type specifier is required for all declarations   (× 19)
//
// The SDK's `<pthread.h>` won the search over the C library's in the graph, and
// pulled in a Mach header that its own prerequisites were not there for. On a
// Linux host the same build had taken the graph's copy and said nothing, which
// is why the difference is a HOST difference and shows up only here.
//
// `pthread_t` IS A POINTER ON BOTH C LIBRARIES — `struct __pthread*` in musl
// and `struct _opaque_pthread_t*` on this system — so `void*` is the same
// argument at the ABI, which is the level `extern "C"` matches at. The value is
// stored as an integer below in either case.
extern "C" {
#ifdef OKM_STANDALONE
int pthread_create_from_mach_thread(void** thread, const void* attr,
                                    void* (*start)(void*), void* arg);
#else
int pthread_create(void** thread, const void* attr,
                   void* (*start)(void*), void* arg);
int pthread_join(void* thread, void** value);
int pthread_attr_init(void* attr);
int pthread_attr_setstacksize(void* attr, unsigned long size);
int pthread_attr_destroy(void* attr);
#endif
}

// EIGHT MEBIBYTES, AS A PROCESS'S FIRST CONTEXT HAS.
//
// The thread library gives a thread it is not told otherwise 512 KiB. A program
// above recurses on any context as deeply as on the first, and one that sizes
// its own threads cannot say so here (the size is a property, not a parameter):
// Clang asks for 8 MiB and instantiates templates accordingly, and mcppls, which
// runs Clang on contexts of this implementation, ran off the end of 512 KiB in
// Sema::DeduceTemplateArguments.
//
// Standalone, the size cannot be asked of the thread library: its attribute
// names are among those a program above defines (musl's pthread_attr_*), so
// they would set that library's attributes, not this system's. The context
// instead runs its entry on a stack of its own, mapped here, through
// okm_call_on_stack; the stack the thread library gave it holds only the frames
// of `run'. The stack's lowest page is made inaccessible, so running off the
// end faults where it happens.
constexpr okm_uptr kStack = 8u * 1024u * 1024u;

#ifdef OKM_STANDALONE
// Calls fn(arg) with the stack pointer at `top' and returns on the stack it was
// called on. A frame record on the new stack links to the caller's, so a
// backtrace from inside crosses back to the thread's own frames.
extern "C" void okm_call_on_stack(void (*fn)(void*), void* arg, void* top);
#if defined(__aarch64__)
asm(".globl _okm_call_on_stack\n"
    ".p2align 2\n"
    "_okm_call_on_stack:\n"
    "  stp x29, x30, [sp, #-16]!\n"   // the caller's frame, on its stack
    "  mov x29, sp\n"
    "  mov sp, x2\n"                  // the new stack, 16-byte aligned
    "  stp x29, x30, [sp, #-16]!\n"   // a record linking back to it
    "  mov x29, sp\n"
    "  mov x8, x0\n"
    "  mov x0, x1\n"
    "  blr x8\n"
    "  ldp x29, x30, [sp], #16\n"     // x29: the frame on the old stack
    "  mov sp, x29\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n");
#elif defined(__x86_64__)
asm(".globl _okm_call_on_stack\n"
    ".p2align 4\n"
    "_okm_call_on_stack:\n"
    "  pushq %rbp\n"                  // the caller's frame, on its stack
    "  movq %rsp, %rbp\n"
    "  movq %rdx, %rsp\n"             // the new stack, 16-byte aligned at the call
    "  movq %rdi, %rax\n"
    "  movq %rsi, %rdi\n"
    "  callq *%rax\n"                 // the callee's saved %rbp links back
    "  movq %rbp, %rsp\n"
    "  popq %rbp\n"
    "  retq\n");
#else
#error "okm_call_on_stack: no definition for this architecture"
#endif
#endif

namespace {

struct context {
    void (*entry)(void*);
    void* arg;
#ifdef OKM_STANDALONE
    volatile okm_u32 finished;
    void* stack;
#else
    unsigned long thread;
#endif
};

void* run(void* p) {
    auto* c = static_cast<context*>(p);
#ifdef OKM_STANDALONE
    okm_call_on_stack(c->entry, c->arg, static_cast<unsigned char*>(c->stack) + kStack);
    __atomic_store_n(&c->finished, 1u, __ATOMIC_RELEASE);
    okm::sys(okm::nr_ulock_wake,
             static_cast<okm_long>(okm::ul_compare_and_wait | okm::ulf_no_errno
                                 | okm::ulf_wake_all),
             reinterpret_cast<okm_long>(const_cast<okm_u32*>(&c->finished)), 0);
#else
    c->entry(c->arg);
#endif
    return nullptr;
}

int translate_posix(int e) {
    switch (e) {
        case okm::e_inval: case okm::e_fault: return kal_err_invalid;
        case okm::e_again:                    return kal_err_again;
        case okm::e_nomem:                    return kal_err_no_memory;
        case okm::e_perm:                     return kal_err_permission;
        default:                              return kal_err_io;
    }
}

}  // namespace

extern "C" {

int kal_task_start(void (*entry)(void*), void* arg, kal_task* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;
    auto* c = static_cast<context*>(kal_alloc(sizeof(context), alignof(context)));
    if (c == nullptr) return kal_err_no_memory;
    okm::fill(c, 0, sizeof(context));
    c->entry = entry; c->arg = arg;

#ifdef OKM_STANDALONE
    // A mapping of this size is a whole one of its own, so its first page is
    // the stack's lowest. A refusal leaves the stack without its guard, not
    // without its context.
    c->stack = kal_alloc(kStack, 16);
    if (c->stack == nullptr) { kal_free(c, sizeof(context), alignof(context)); return kal_err_no_memory; }
    okm::sys(okm::nr_mprotect, reinterpret_cast<okm_long>(c->stack),
             static_cast<okm_long>(kal_memory_granularity()), 0 /* PROT_NONE */);
    void* thread = nullptr;
    const int rc = pthread_create_from_mach_thread(&thread, nullptr, run, c);
    if (rc != 0) kal_free(c->stack, kStack, 16);
#else
    // _opaque_pthread_attr_t: a long and 56 bytes, on both architectures.
    alignas(16) unsigned char attr[64] = {};
    pthread_attr_init(attr);
    pthread_attr_setstacksize(attr, kStack);
    void* id = nullptr;
    const int rc = ::pthread_create(&id, attr, run, c);
    pthread_attr_destroy(attr);
    if (rc == 0) c->thread = static_cast<unsigned long>(reinterpret_cast<okm_uptr>(id));
#endif
    if (rc != 0) { kal_free(c, sizeof(context), alignof(context)); return translate_posix(rc); }
    *out = kal_task{ reinterpret_cast<kal_uintptr>(c) };
    return kal_ok;
}

int kal_task_join(kal_task h) {
    auto* c = reinterpret_cast<context*>(h.h);
    if (c == nullptr) return kal_err_invalid;
#ifdef OKM_STANDALONE
    // Waited for with the primitive rather than with the thread library's own
    // wait, because that one's name is among the ones a program above may
    // define. The word the context sets before it ends is what is waited upon.
    while (__atomic_load_n(&c->finished, __ATOMIC_ACQUIRE) == 0) {
        okm::sys(okm::nr_ulock_wait,
                 static_cast<okm_long>(okm::ul_compare_and_wait | okm::ulf_no_errno),
                 reinterpret_cast<okm_long>(const_cast<okm_u32*>(&c->finished)), 0, 0);
    }
    // The entry has returned from it: `run' is on the thread library's stack.
    kal_free(c->stack, kStack, 16);
#else
    const int rc = ::pthread_join(reinterpret_cast<void*>(
                                      static_cast<okm_uptr>(c->thread)), nullptr);
    if (rc != 0) return translate_posix(rc);
#endif
    kal_free(c, sizeof(context), alignof(context));
    return kal_ok;
}

void kal_task_yield(void) { okm::relax(); }

kal_uintptr kal_task_current(void) { return okm::current_context(); }

int kal_task_wait(const kal_u32* word, kal_u32 expected,
                  kal_u64 timeout_ns) {
    // The unit this system takes is the microsecond, and zero means no timeout.
    // A timeout shorter than a microsecond is rounded up rather than down: a
    // wait that returned before the time it was given would make every timed
    // wait above it wrong.
    okm_u32 microseconds = 0;
    if (timeout_ns != 0) {
        const kal_u64 rounded = (timeout_ns + 999u) / 1000u;
        microseconds = rounded > 0xfffffffeu ? 0xfffffffeu : static_cast<okm_u32>(rounded);
    }
    for (;;) {
        const okm_long r = okm::sys(okm::nr_ulock_wait,
                                    static_cast<okm_long>(okm::ul_compare_and_wait
                                                        | okm::ulf_no_errno),
                                    reinterpret_cast<okm_long>(const_cast<kal_u32*>(word)),
                                    static_cast<okm_long>(expected),
                                    static_cast<okm_long>(microseconds));
        if (r >= 0) return kal_ok;
        // The value had already changed, which is a successful outcome: the
        // caller's condition no longer holds and it should re-examine it.
        if (r == -okm::e_again) return kal_ok;
        if (okm::interrupted(r)) continue;
        if (r == -okm::e_timedout) return kal_err_again;
        return okm::translate(r);
    }
}

// How many contexts can run at the same moment. Version 0.10.
//
// Added because its absence was a WRONG ANSWER and not a refusal:
// `KAL_TASK_PROP_PARALLEL' says whether and not how many, so a C library above
// answered 1 with no error and a program sizing a pool of workers got one.
kal_uintptr kal_task_parallelism(void) {
    int mib[2] = { okm::ctl_hw, okm::hw_ncpu };
    int value = 0;
    okm_uptr length = sizeof value;
    const okm_long r = okm::sys(okm::nr_sysctl,
                                reinterpret_cast<okm_long>(mib), 2,
                                reinterpret_cast<okm_long>(&value),
                                reinterpret_cast<okm_long>(&length), 0, 0);
    // Zero is "cannot say", and openkal distinguishes it from one on purpose.
    if (okm::failed(r) || value <= 0) return 0;
    return static_cast<kal_uintptr>(value);
}

int kal_task_wake(const kal_u32* word, kal_uintptr count, kal_uintptr* woken) {
    if (count == 0) { if (woken) *woken = 0; return kal_ok; }
    okm_long operation = okm::ul_compare_and_wait | okm::ulf_no_errno;
    if (count > 1) operation |= okm::ulf_wake_all;
    const okm_long r = okm::sys(okm::nr_ulock_wake, operation,
                                reinterpret_cast<okm_long>(const_cast<kal_u32*>(word)), 0);
    // This system reports that nothing was waiting as a failure. Nothing having
    // been waiting is not a failure of the operation, so it is reported as none
    // woken.
    if (okm::failed(r) && r != -okm::e_noent) return okm::translate(r);
    if (woken) *woken = okm::failed(r) ? 0u : count;
    return kal_ok;
}

// A context started here observes the thread-local storage of the toolchain
// that compiled the program: this system's thread library establishes it for
// every context it creates, which is why the position can be reported without
// this implementation doing anything to earn it.
kal_uintptr kal_task_props(void) { return
    KAL_TASK_PROP_PREEMPTIVE | KAL_TASK_PROP_PARALLEL
  | KAL_TASK_PROP_WAIT_TIMEOUT | KAL_TASK_PROP_THREAD_LOCAL; }

}
