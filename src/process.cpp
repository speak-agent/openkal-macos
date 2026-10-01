#include "sys.h"
#include "handle.h"
#include <openkal/process.h>
#include <openkal/memory.h>

// A program image that has been started.
//
// This system has no operation that starts a program relative to a directory,
// so the directory is entered by the duplicate before it replaces itself --- a
// duplicate that exists for the length of two calls, is not a resource the
// caller receives, and is exactly what openkal declines to offer for that
// reason. The caller's own working directory is not touched, which is the
// property the interface requires and which an implementation that entered the
// directory before duplicating would not have.

namespace {

// A bound against a vector no program could mean, not a limit of the kernel's: what it limits is the
// bytes (a quarter of the stack's limit in all, 128 KiB for one string), and a linker is started with
// one argument per object. 512 refused a link of seven hundred objects (mcxx linking xlings) with
// kal_err_no_memory, far below any length the kernel would refuse.
constexpr kal_uintptr kMaxEntries = kal_uintptr { 1 } << 20;

// The counted arrays the interface takes become the terminated arrays this
// kernel takes. Every allocation happens before the program is duplicated, so
// that the duplicate performs nothing but a few calls: a duplicate of a program
// that has more than one execution context may hold a lock no context in it
// will release.
struct vector {
    char** slots = nullptr;
    char*  bytes = nullptr;
    okm_uptr slots_bytes = 0;
    okm_uptr bytes_bytes = 0;
    bool   ok = true;

    bool build(const char** items, const kal_uintptr* lens, kal_uintptr n) {
        if (n > kMaxEntries) { ok = false; return false; }
        okm_uptr total = 0;
        for (kal_uintptr i = 0; i < n; ++i) total += lens[i] + 1;
        slots_bytes = (n + 1) * sizeof(char*);
        bytes_bytes = total == 0 ? 1 : total;
        slots = static_cast<char**>(kal_alloc(slots_bytes, alignof(char*)));
        bytes = static_cast<char*>(kal_alloc(bytes_bytes, 1));
        if (!slots || !bytes) { ok = false; return false; }
        okm_uptr at = 0;
        for (kal_uintptr i = 0; i < n; ++i) {
            okm::copy(bytes + at, items[i], lens[i]);
            bytes[at + lens[i]] = '\0';
            slots[i] = bytes + at;
            at += lens[i] + 1;
        }
        slots[n] = nullptr;
        return true;
    }

    ~vector() {
        if (slots) kal_free(slots, slots_bytes, alignof(char*));
        if (bytes) kal_free(bytes, bytes_bytes, 1);
    }
};

constexpr okm_long nr_fchdir = 13;
// openkal 0.11: the unit a started program joins.
constexpr okm_long nr_setpgid = 82;

// --- reporting a replacement that failed -----------------------------------
//
// THE REPLACEMENT HAPPENS IN THE DUPLICATE, SO ITS FAILURE WAS REPORTED TO
// NOBODY. Version 0.13.
//
// A program is started here by duplicating this image and replacing the
// duplicate. The replacement is the part that can fail --- the name is absent,
// or is a directory, or is not a program, or may not be executed --- and it
// failed inside an image the caller does not have. This implementation ended
// that image with 127 and answered kal_ok with a handle, so a caller learned
// something was wrong only by waiting and reading 127, which is exactly what a
// program that RAN and exited 127 reports. openkal SPEC.md clause 11 entry 16.
//
// The arrangement is openkal-linux's: a pipe whose ends close when the image
// is replaced. Nothing arrives, the replacement happened; a value arrives, it
// did not, and the value says why.
struct exec_report {
    int  fd[2] = { -1, -1 };
    bool armed = false;

    // THE PIPE MUST NOT SIT WHERE THE DUPLICATE IS ABOUT TO PLACE SOMETHING.
    // The duplicate places streams at 0, 1 and 2 and granted directories at 3
    // and upwards, so a pipe that happened to hold one of those numbers would
    // be closed by the very placement whose failure it exists to report --- and
    // the parent would then read end-of-input and call that success.
    //
    // F_DUPFD_CLOEXEC answers the lowest FREE descriptor at or above a bound,
    // atomically. There is no pipe2 on this kernel, so each end is marked
    // close-on-exec separately before being lifted; a caller that spawns from
    // one context, which is what this operation is used from, is not affected
    // by the two steps not being atomic with each other.
    bool open(kal_uintptr placements) {
        okm_long w = -1;
        const okm_long r = okm::pipe_pair(w);
        if (okm::failed(r)) return false;
        fd[0] = static_cast<int>(r);
        fd[1] = static_cast<int>(w);
        okm::sys(okm::nr_fcntl, fd[0], okm::f_setfd, okm::fd_cloexec);
        okm::sys(okm::nr_fcntl, fd[1], okm::f_setfd, okm::fd_cloexec);
        const okm_long floor = 3 + static_cast<okm_long>(placements);
        armed = lift(fd[0], floor) && lift(fd[1], floor);
        if (!armed) close_both();
        return armed;
    }

    void close_both() {
        if (fd[0] >= 0) okm::sys(okm::nr_close, fd[0]);
        if (fd[1] >= 0) okm::sys(okm::nr_close, fd[1]);
        fd[0] = fd[1] = -1;
    }

    // In the duplicate, once the replacement has returned --- which it does
    // only when it did not happen.
    void say(okm_long failure) const {
        if (!armed) return;
        okm_long value = failure;
        okm::sys(okm::nr_write, fd[1],
                 reinterpret_cast<okm_long>(&value), sizeof value);
    }

    // In this image. Zero when the replacement happened, otherwise the
    // kernel's own negative value for why it did not.
    okm_long heard() {
        if (!armed) return 0;
        okm::sys(okm::nr_close, fd[1]);
        fd[1] = -1;
        okm_long value = 0;
        okm_long n;
        // A transfer this short is not divided, but it can be interrupted.
        do {
            n = okm::sys(okm::nr_read, fd[0],
                         reinterpret_cast<okm_long>(&value), sizeof value);
        } while (n == -okm::e_intr);
        okm::sys(okm::nr_close, fd[0]);
        fd[0] = -1;
        return (n == static_cast<okm_long>(sizeof value)) ? value : 0;
    }

private:
    static bool lift(int& f, okm_long floor) {
        const okm_long n = okm::sys(okm::nr_fcntl, f, okm::f_dupfd_cloexec, floor);
        if (okm::failed(n)) return false;
        okm::sys(okm::nr_close, f);
        f = static_cast<int>(n);
        return true;
    }
};

// A duplicate that could not be replaced is ended, and this image waits for it
// so that nothing is left for a caller to meet later. It is the one wait this
// implementation performs that a caller did not ask for, and it is bounded:
// the duplicate has already reached `exit'.
inline void reap(okm_long child) {
    int status = 0;
    okm_long r;
    do {
        r = okm::sys(okm::nr_wait4, child,
                     reinterpret_cast<okm_long>(&status), 0, 0);
    } while (r == -okm::e_intr);
}

}  // namespace

extern "C" {

// Starting a program. One function since openkal 0.11, where three declarations
// became one and their modifiers became positions in `kal_spawn'.
//
// AND THIS IMPLEMENTATION IS WHERE THE MISSING MODIFIER WAS ALREADY VISIBLE,
// which is worth recording rather than quietly fixing.
//
// This kernel has no `execveat', so a program named relative to a directory has
// always been started by entering that directory first --- the `fchdir(base)'
// below used to be the whole story. So a started program's working directory WAS
// `base' here, and on the other kernel it was whatever that implementation
// happened to be in. Same openkal calls, two different observable answers,
// and neither was wrong because the specification said nothing.
//
// ⇒ 0.11 gives the caller a second directory, and both implementations now enter
// the one the caller named. The divergence is gone because the thing that caused
// it --- a property nobody had declared --- is declared.
int kal_process_spawn(const kal_spawn* how,
                      const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams,
                      kal_process* out) {
    if (how == nullptr || out == nullptr) return kal_err_invalid;

    const int b = okm::unpack(how->base.h);
    const int w = okm::unpack(how->work.h);
    if (b < 0 || w < 0) return kal_err_invalid;
    if (!okm::acceptable(path, path_len)) return kal_err_invalid;
    if (how->grant_count > 0 && how->grants == nullptr) return kal_err_invalid;

    // A LIFETIME THIS KERNEL CANNOT BIND IS REFUSED BEFORE ANYTHING STARTS.
    // The reason is unchanged from 0.10 and is stated at kal_process_props: the
    // binding must hold however the caller ends, including when it is killed
    // outright, and what this system offers instead is a WATCH, which needs a
    // live context to notice. Composing it would move the defect from "refused"
    // to "works except when it matters".
    if (how->flags != 0) return kal_err_not_supported;

    okm::terminated p(path, path_len);
    if (!p.ok) return kal_err_invalid;

    vector args, envs;
    if (!args.build(argv, argv_lens, argc)) return kal_err_no_memory;
    if (!envs.build(envp, envp_lens, envc)) return kal_err_no_memory;

    constexpr kal_uintptr max_grants = 16;
    if (how->grant_count > max_grants) return kal_err_invalid;
    int granted[max_grants];
    for (kal_uintptr i = 0; i < how->grant_count; ++i) {
        granted[i] = okm::unpack(how->grants[i].dir.h);
        if (granted[i] < 0) return kal_err_invalid;
    }

    // THE PROGRAM'S NAME IS MADE ABSOLUTE BEFORE THE DIRECTORY MOVES, because
    // with no `execveat' the two things `base' and `work' now mean cannot both be
    // served by one `fchdir'. `F_GETPATH' answers the path of an open directory,
    // which src/fs.cpp already relies on for the same reason: this kernel has no
    // call that reports a working directory, so a path is obtained from the
    // descriptor that names it.
    char whole[1024];
    kal_uintptr n = 0;
    if (p.buf[0] == '/') {
        while (p.buf[n] && n < sizeof whole - 1) { whole[n] = p.buf[n]; ++n; }
    } else {
        const okm_long r = okm::sys(okm::nr_fcntl, b, okm::f_getpath,
                                    reinterpret_cast<okm_long>(whole));
        if (okm::failed(r)) return okm::translate(r);
        while (whole[n] && n < sizeof whole - 1) ++n;
        if (n && whole[n - 1] != '/' && n < sizeof whole - 1) whole[n++] = '/';
        for (kal_uintptr i = 0; p.buf[i] && n < sizeof whole - 1; ++i) whole[n++] = p.buf[i];
    }
    if (n >= sizeof whole - 1) return kal_err_invalid;
    whole[n] = '\0';

    const okm_long in = streams ? static_cast<okm_long>(streams->in.h)  : 0;
    const okm_long ou = streams ? static_cast<okm_long>(streams->out.h) : 0;
    const okm_long er = streams ? static_cast<okm_long>(streams->err.h) : 0;

    // The unit, named here by a process group --- which is to say by whichever
    // program formed it first. Zero for the first member; a later one is given
    // the number to join.
    const okm_long join = how->job ? static_cast<okm_long>(how->job->h) : 0;
    const bool     unit = how->job != nullptr;

    // The bound is 3 + grant_count, because the placements below reach that
    // far. A caller that cannot arm the report is not refused for it: `heard'
    // then answers zero and this operation behaves as it did before 0.13,
    // which is the same fallback openkal-linux takes.
    exec_report report;
    report.open(how->grant_count);

    bool is_duplicate = false;
    const okm_long child = okm::duplicate(is_duplicate);
    if (okm::failed(child)) { report.close_both(); return okm::translate(child); }

    // The duplicate is distinguished by the second value the call returns and
    // not by the first: both images receive the same first value here. The
    // reason, and what happens to an implementation that tests the first alone,
    // are in src/sys.h beside the call.
    if (is_duplicate) {
        if (in != 0) okm::sys(okm::nr_dup2, in, 0);
        if (ou != 0) okm::sys(okm::nr_dup2, ou, 1);
        if (er != 0) okm::sys(okm::nr_dup2, er, 2);

        for (kal_uintptr i = 0; i < how->grant_count; ++i) {
            const okm_long want = static_cast<okm_long>(3 + i);
            if (granted[i] != want) okm::sys(okm::nr_dup2, granted[i], want);
        }

        // The directory the program RUNS in --- `whole' already carries where it
        // is named from, so this no longer has to serve both.
        //
        // A FAILURE HERE MUST NOT REACH `execve'. Running the right program in
        // the wrong directory is the silent wrongness this pipe exists to
        // remove, so it is reported through the same channel an exec failure
        // uses.
        if (const okm_long e = okm::sys(nr_fchdir, w); okm::failed(e)) {
            report.say(e);
            okm::sys(okm::nr_exit, 127);
            for (;;) { }
        }

        if (unit) okm::sys(nr_setpgid, 0, join);

        const okm_long why = okm::sys(okm::nr_execve, reinterpret_cast<okm_long>(whole),
                                      reinterpret_cast<okm_long>(args.slots),
                                      reinterpret_cast<okm_long>(envs.slots));
        // Reached only when the replacement did not happen, because when it
        // does there is nothing here to reach.
        report.say(why);
        okm::sys(okm::nr_exit, 127);
        for (;;) { }
    }

    // A START THAT DID NOT HAPPEN IS REPORTED, AND BY ITS REASON. Version
    // 0.13. Before this the duplicate's own failure reached nobody: this image
    // read nothing from a channel that did not exist and answered kal_ok with
    // a handle, whatever the name given it was.
    if (const okm_long why = report.heard()) {
        reap(child);
        return okm::translate(why);
    }

    // Written only after the start succeeded, and only when the unit was new:
    // the first member's identifier IS the group's.
    if (unit && join == 0) how->job->h = static_cast<kal_uintptr>(child);

    *out = kal_process{ static_cast<kal_uintptr>(child) };
    return kal_ok;
}

// A channel: a pair of streams of which one end is meant to cross a spawn.
//
// This kernel's `pipe' reports BOTH descriptors as return values rather than
// through a buffer, which is a property of its calling convention and not of the
// call: the second value comes back in the second register. src/sys.h says the
// same thing about the duplication primitive, and for the same reason.
//
// THERE IS NO pipe2 HERE, so close-on-exec is set afterwards with fcntl. Doing
// it in two steps is not equivalent under a concurrent spawn --- another context
// starting a program between the two would inherit the descriptors --- and this
// implementation states that rather than concealing it. A caller that spawns
// from one context, which is what a program using this operation does, is not
// affected.
int kal_process_channel(kal_stream* mine, kal_stream* theirs) {
    if (mine == nullptr || theirs == nullptr) return kal_err_invalid;

    okm_long second = 0;
    const okm_long first = okm::pipe_pair(second);
    if (okm::failed(first)) return okm::translate(first);

    constexpr okm_long f_setfd = 2, fd_cloexec = 1;
    okm::sys(okm::nr_fcntl, first,  f_setfd, fd_cloexec);
    okm::sys(okm::nr_fcntl, second, f_setfd, fd_cloexec);

    // Bare descriptors, because openkal.stream's transfer operations take what
    // this kernel takes. kal_fs_stream reports a file's stream the same way.
    *mine   = kal_stream{ static_cast<kal_uintptr>(first)  };   // the reading end
    *theirs = kal_stream{ static_cast<kal_uintptr>(second) };   // the writing end
    return kal_ok;
}

void kal_process_channel_close(kal_stream s) {
    // The standard streams are borrowed and are numbered 0, 1 and 2; closing one
    // of those through this operation would take a stream away from the whole
    // program.
    const okm_long fd = static_cast<okm_long>(s.h);
    if (fd < 3) return;
    okm::sys(okm::nr_close, fd);
}

// Starting a program that receives exactly the directories named.

// One program, whatever unit it is in --- the unit has its own operation below,
int kal_process_wait(kal_process h, int* status, int* terminated_by_environment) {
    if (h.h == 0) return kal_err_invalid;
    int st = 0;
    for (;;) {
        const okm_long r = okm::sys(okm::nr_wait4, static_cast<okm_long>(h.h),
                                    reinterpret_cast<okm_long>(&st), 0, 0);
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return okm::translate(r);
        break;
    }
    // The encoding is the kernel's: the low seven bits name the signal that
    // ended the program and are zero when it ended by returning, in which case
    // the next eight bits are what it returned.
    const int signalled = st & 0x7f;
    if (signalled == 0) {
        if (status) *status = (st >> 8) & 0xff;
        if (terminated_by_environment) *terminated_by_environment = 0;
    } else {
        if (status) *status = signalled;
        if (terminated_by_environment) *terminated_by_environment = 1;
    }
    return kal_ok;
}

// so this one's meaning never turns on how the program was started.
int kal_process_terminate(kal_process h) {
    if (h.h == 0) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_kill, static_cast<okm_long>(h.h), 15 /* SIGTERM */);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// NOT CLAIMED HERE, FOR THE SAME REASON THE SIGPIPE NOTE IN src/env.cpp
// GIVES. Observing a request to end means installing a disposition, and this
// kernel's `sigaction' takes a structure carrying a TRAMPOLINE its C library
// supplies. A disposition installed with the wrong shape shows up as a program
// dying in a way nobody can trace --- and a facility this repository has not
// MEASURED is exactly what it refuses to claim elsewhere.
//
// ⇒ Null, and KAL_PROCESS_PROP_STOP_REQUESTED unclaimed, so a caller that asks
// first is told. The other implementation answers it; this one will when it can
// be exercised here.
// A WORD THIS PROGRAM'S ENVIRONMENT SETS WHEN SOMEBODY HAS ASKED IT TO END.
//
// THE TRAMPOLINE IS THIS IMPLEMENTATION'S OWN, WHICH IS WHY THIS ARRIVED A
// VERSION LATE. The raw `sigaction' of this kernel takes a structure whose
// SECOND field is `sa_tramp': the kernel enters that address, not the handler,
// and the handler is passed to it as an argument. A C library ordinarily
// supplies it (`_sigtramp' in libsystem) and there is no C library beneath this
// implementation. A structure installed with a null or wrong `sa_tramp' does not
// fail at installation --- it fails on DELIVERY, inside the handler, at an
// address that belongs to nobody, which is the least attributable failure this
// implementation could ship. So the order was: a conformance check that raises
// the signal first, this second.
//
// ARMED ON THE FIRST ENQUIRY AND NOT AT STARTUP, exactly as openkal-linux
// argues: a program that never asks keeps the default action, and adding this
// operation therefore changes nothing for anyone who does not use it.
namespace {

kal_u32 g_stop_word = 0;
int     g_stop_armed = 0;

#if defined(__aarch64__)

constexpr okm_long nr_sigaction = 46;

// What the kernel enters. Its arguments are (handler, infostyle, sig, siginfo,
// ucontext) in x0..x4; it calls the handler with the last three and then asks
// the kernel to restore the interrupted context.
//
// x19 AND x20 ARE USED WITHOUT BEING SAVED, and that is correct here rather
// than sloppy: this function does not return to its caller. `sigreturn' restores
// the whole of the interrupted context, callee-saved registers included, so the
// values these two held belong to a frame the kernel is about to reinstate.
extern "C" void okm_sigtramp(void);
asm(".globl _okm_sigtramp\n"
    ".p2align 2\n"
    "_okm_sigtramp:\n"
    "  mov x19, x1\n"          // infostyle
    "  mov x20, x4\n"          // ucontext
    "  mov x8,  x0\n"          // handler
    "  mov x0,  x2\n"          // sig
    "  mov x1,  x3\n"          // siginfo
    "  mov x2,  x20\n"         // ucontext
    "  blr x8\n"
    "  mov x0,  x20\n"         // ucontext
    "  mov x1,  x19\n"         // infostyle
    "  mov x16, #184\n"        // SYS_sigreturn
    "  svc #0x80\n"
    "  brk #1\n");             // sigreturn does not come back

void stop_handler(int) {
    __atomic_store_n(&g_stop_word, 1u, __ATOMIC_RELEASE);
    // Woken through the same operation `kal_task_wake' performs, issued as the
    // raw call because a handler may not enter code that takes a lock. Waking
    // ALL of them: any number of contexts may be waiting upon this one word, and
    // the handler has no way to learn how many.
    okm::sys(okm::nr_ulock_wake,
             okm::ul_compare_and_wait | okm::ulf_no_errno | okm::ulf_wake_all,
             reinterpret_cast<okm_long>(&g_stop_word), 0);
}

// The structure this kernel's `sigaction' takes. `sa_tramp' is the second field
// and is the whole reason this is spelled out rather than borrowed.
struct macos_sigaction {
    void (*handler)(int);
    void (*tramp)(void*, int, int, void*, void*);
    unsigned int mask;
    int flags;
};

// THE RESULT IS EXAMINED, AND THE FUNCTION EXISTS TO RETURN IT. An
// installation that failed would leave a word that can never change, and
// answering the caller with one is `nothing here reports success having done
// nothing' in its exact form: the program would ask whether its end had been
// requested, be told no, and go on being told no after it had been.
bool arm_one(int signo) {
    macos_sigaction act{};
    act.handler = &stop_handler;
    act.tramp   = reinterpret_cast<void (*)(void*, int, int, void*, void*)>(&okm_sigtramp);
    return !okm::failed(okm::sys(nr_sigaction, signo,
                                 reinterpret_cast<okm_long>(&act), 0));
}

#endif  // __aarch64__

}  // namespace

const kal_u32* kal_process_stop_requested(void) {
#if defined(__aarch64__)
    // THREE STATES AND NOT TWO: not yet tried, armed, refused. A second
    // caller must be told what the first found rather than arming again --- and
    // must not be told `not yet tried' while the first is still inside the
    // installation.
    int state = __atomic_load_n(&g_stop_armed, __ATOMIC_ACQUIRE);
    if (state == 0) {
        // SIGTERM is the one `kal_process_terminate' sends here; SIGINT is what
        // an interactive stream delivers. Both are requests to end, which is the
        // whole of what this word reports.
        const bool ok = arm_one(15) && arm_one(2);
        state = ok ? 1 : -1;
        __atomic_store_n(&g_stop_armed, state, __ATOMIC_RELEASE);
    }
    return state == 1 ? &g_stop_word : nullptr;
#else
    // DECLINED ON THE OTHER ARCHITECTURE, AND NOT BECAUSE IT CANNOT BE
    // WRITTEN. The trampoline above has an x86_64 counterpart of the same
    // length. What it does not have is a way to be RUN: the build tool has no
    // release for x86_64 on this system, so continuous integration compiles the
    // sources there and executes nothing --- and a trampoline that has never
    // been entered is the one thing this operation must not ship. Clause 6.2
    // makes the absence a fact a caller reads, and `kal_process_props' below
    // does not claim the position.
    return 0;
#endif
}

// This program itself joins or forms a unit --- what `kal_spawn.job' cannot say,
// because that places a program the caller STARTS and a copy wishing to lead a
// unit must say so about ITSELF before it replaces itself.
int kal_process_job_enter(kal_job* j) {
    if (j == nullptr) return kal_err_invalid;
    const okm_long join = static_cast<okm_long>(j->h);
    const okm_long r = okm::sys(nr_setpgid, 0, join);
    if (okm::failed(r)) return okm::translate(r);
    if (join == 0) j->h = static_cast<kal_uintptr>(okm::sys(okm::nr_getpid));
    return kal_ok;
}

// Every program in the unit, including ones never held as a handle.
//
// A group is named by a process identifier, and those are reused: once the
// program that formed it has ended and the numbers have wrapped, this can reach
// a different group. That is what this system does, and it is recorded rather
// than hidden.
int kal_process_job_terminate(kal_job j) {
    if (j.h == 0) return kal_err_invalid;
    // The signal that cannot be declined --- see openkal-linux for the reasoning:
    // a unit contains programs the caller never held a handle to, so a request any
    // member may ignore does not terminate the unit.
    const okm_long r = okm::sys(okm::nr_kill, -static_cast<okm_long>(j.h), 9 /* SIGKILL */);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// A group here is a number and not a resource, so there is nothing to release.
void kal_process_job_close(kal_job) { }

// Releasing the handle does not affect the program. A program that has not been
// waited for continues, and this environment collects it when the caller exits.
void kal_process_close(kal_process) { }

// Starting a program whose lifetime is bound to this one's. Version 0.10.
//

// EVERY POSITION THE SPECIFICATION HAS ASSIGNED IS ACCOUNTED FOR HERE, either
// by being claimed or by being deliberately absent. BOUND_LIFETIME is absent
// because `kal_process_spawn' above refuses every flag; STOP_REQUESTED is
// claimed only where the trampoline it needs has been entered by a running
// program, which is the architecture continuous integration executes.
kal_uintptr kal_process_props(void) { return
    KAL_PROCESS_PROP_TERMINATE | KAL_PROCESS_PROP_STREAM_PASSING
  | KAL_PROCESS_PROP_EXIT_STATUS
  | KAL_PROCESS_PROP_CHANNEL | KAL_PROCESS_PROP_GRANT_DIR
  | KAL_PROCESS_PROP_JOB
#if defined(__aarch64__)
  // AND IT AGREES WITH `kal_process_stop_requested', WHICH IS A REQUIREMENT
  // AND NOT A COURTESY: the header defines null there as the absence this
  // position reports, so the two cannot disagree. It is read and never armed
  // --- asking what an implementation can do must not install a disposition ---
  // so this claims the position until an installation has actually been refused,
  // and stops claiming it afterwards.
  | (__atomic_load_n(&g_stop_armed, __ATOMIC_ACQUIRE) == -1
        ? 0u : KAL_PROCESS_PROP_STOP_REQUESTED)
#endif
  ; }

}
