// Conformance: openkal.process and openkal.task.
import openkal.process;
import openkal.macros;
import openkal.task;
import openkal.fs;
import openkal.stream;

namespace {
int failures = 0;

void say(const char* s) {
    kal_uintptr n = 0; while (s[n]) ++n;
    kal::write(kal::err(), s, n);
}

void say_num(int v) {
    if (v < 0) { kal::write(kal::err(), "-", 1); v = -v; }
    char b[12]; int i = 12;
    if (v == 0) b[--i] = '0';
    while (v > 0) { b[--i] = static_cast<char>('0' + v % 10); v /= 10; }
    kal::write(kal::err(), b + i, static_cast<kal_uintptr>(12 - i));
}

void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    const char pre[] = "FAIL: ";
    kal::write(kal::err(), pre, sizeof(pre) - 1);
    kal_uintptr n = 0; while (what[n]) ++n;
    kal::write(kal::err(), what, n);
    kal::write(kal::err(), "\n", 1);
}

// Shared between contexts. The word is what openkal.task suspends upon, and it
// is ordinary memory: the interface adds no object of its own.
volatile __UINT32_TYPE__ g_word = 0;
int g_ran = 0;

void worker(void* arg) {
    g_ran = 1;
    *static_cast<int*>(arg) = 42;
    __atomic_store_n(reinterpret_cast<__UINT32_TYPE__*>(const_cast<__UINT32_TYPE__*>(&g_word)),
                     1u, __ATOMIC_SEQ_CST);
    kal_uintptr woken = 0;
    kal_task_wake(const_cast<const __UINT32_TYPE__*>(&g_word), 1, &woken);
}

// 4 MiB of frames: 256 calls each holding 16 KiB written at both ends, kept
// alive by the read after each call. Clang recurses this deeply on a thread it
// asked 8 MiB for; the thread library's own default is 512 KiB.
int deep(int n) {
    volatile unsigned char frame[16384];
    frame[0] = static_cast<unsigned char>(n);
    frame[sizeof frame - 1] = 1;
    if (n == 0) return frame[sizeof frame - 1];
    return deep(n - 1) + frame[sizeof frame - 1];
}

void deep_worker(void* arg) { *static_cast<int*>(arg) = deep(255); }
}

int main() {
    // --- openkal.process -----------------------------------------------------
    //
    // The program to start is reached through a directory the environment
    // supplied, which is the whole reason the set exists: a program and the
    // program it starts are commonly not beneath one root.
    kal_dir slash{}; char nm[512] = {}; kal_uintptr nl = 0;
    bool have_root = false;
    for (kal_uintptr i = 0; i < kal::fs::preopen_count(); ++i) {
        kal_dir d{}; char n[512]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) != kal_ok) continue;
        if (l == 1 && n[0] == '/') {
            slash = d; nm[0] = '/'; nm[1] = '\0'; nl = l; have_root = true;
        }
    }
    check(have_root, "a directory covering the file system is supplied");

    if (have_root) {
        // How every start below is described. `work' is the same directory as
        // `base' --- a caller that does not care passes it, openkal having no
        // ambient working directory for a default to mean.
        const kal_spawn how{ slash, slash, nullptr, nullptr, 0, 0 };

        // The program that succeeds and the program that fails are at
        // different places on different systems. The test locates them rather
        // than assuming, because assuming would make it a test of one system.
        const char* true_paths[]  = { "bin/true",  "usr/bin/true"  };
        const kal_uintptr true_lens[] = { 8, 12 };
        const char* false_paths[] = { "bin/false", "usr/bin/false" };
        const kal_uintptr false_lens[] = { 9, 13 };

        // THE CANDIDATE IS CHOSEN BY ASKING, AND IT USED TO BE CHOSEN BY
        // SPAWNING AND RETRYING. That retry could not work and never ran:
        // kal_process_spawn reports whether the DUPLICATE was made, and the
        // program is replaced afterwards, inside a copy the caller no longer
        // is. A path that does not exist therefore produces kal_ok and a
        // duplicate that finishes with 127, so the first candidate was always
        // taken and the second was unreachable code.
        //
        // On a system holding /usr/bin/true and no /bin/true the consequence
        // was `status == 127' at the observation below, which is what this
        // system reported the first time these suites were ever run. The
        // observation after it -- that a non-zero status is reported as such --
        // held throughout, upon a program that was never started.
        auto locate = [&](const char* const* paths, const kal_uintptr* lens_) -> int {
            for (int i = 0; i < 2; ++i) {
                kal_node_info info{}; info.self_size = sizeof info;
                if (kal_fs_info(slash, paths[i], lens_[i], 0, kal::fs::field::kind, &info) != kal_ok)
                    continue;
                if (info.kind != kal_node_absent) return i;
            }
            return -1;
        };

        const int t = locate(true_paths, true_lens);
        const int fpath = locate(false_paths, false_lens);
        check(t >= 0, "a program that succeeds is found");
        check(fpath >= 0, "a program that fails is found");

        kal_process p{};
        const char* argv[] = { "openkal" };
        const kal_uintptr lens[] = { 7 };
        int rc = kal_err_invalid;
        if (t >= 0)
            rc = kal_process_spawn(&how, true_paths[t], true_lens[t], argv, lens, 1,
                                   nullptr, nullptr, 0, nullptr, &p);
        check(rc == kal_ok, "a program is started");
        if (rc == kal_ok) {
            int status = -1, terminated = -1;
            check(kal_process_wait(p, &status, &terminated) == kal_ok, "the program is waited for");
            // The observed status is reported when it is wrong. `127' names an
            // image that was not replaced and `0' names one that ran; without
            // the number the two arrive as the same line.
            if (!(status == 0 && terminated == 0)) {
                say("  status="); say_num(status);
                say(" terminated="); say_num(terminated); say("\n");
            }
            check(status == 0 && terminated == 0, "the status it finished with is reported");
            kal_process_close(p);
        }

        // A program that finishes with a non-zero status is distinguished from
        // one that succeeded. Without this, a harness that ignored the status
        // would pass.
        kal_process q{};
        const char* qargv[] = { "openkal" };
        int qrc = kal_err_invalid;
        if (fpath >= 0)
            qrc = kal_process_spawn(&how, false_paths[fpath], false_lens[fpath], qargv, lens, 1,
                                    nullptr, nullptr, 0, nullptr, &q);
        check(qrc == kal_ok, "the program that fails is started");
        if (qrc == kal_ok) {
            int status = -1, terminated = -1;
            kal_process_wait(q, &status, &terminated);
            // NOT MERELY NON-ZERO. 127 is what a duplicate reports when the
            // image was never replaced, so `!= 0' is satisfied by a program
            // that did not run -- which is precisely how this observation held
            // while the one above did not.
            check(status == 1 && terminated == 0,
                  "the status of a program that fails is its own and not 127");
            kal_process_close(q);
        }

        // Clause 7.6: the vector is passed unaltered, and argv[0] is the name
        // the started program observes as its own.
        //
        // The programs started above ignore their arguments, so they cannot
        // distinguish an implementation that passes the vector from one that
        // prepends the path — which is how that defect survived a suite that
        // started programs and read their statuses. A shell does distinguish
        // them: `sh -c <script>` takes $0 from its own argv[0] when no further
        // argument is given, so the script observes the name the caller chose.
        // An implementation that prepended the path would give the shell an
        // extra argument, which it would read as a script file to open, and the
        // status would be non-zero.
        const char* sh_paths[] = { "bin/sh", "usr/bin/sh" };
        const kal_uintptr sh_lens[] = { 6, 10 };
        const char* script = "test \"$0\" = openkal-observed-argv0";
        kal_uintptr script_len = 0; while (script[script_len]) ++script_len;

        // Located rather than retried, for the reason recorded above: the retry
        // this replaces could not distinguish a path that does not exist from
        // one that does, so it always took the first.
        const int sh = locate(sh_paths, sh_lens);
        check(sh >= 0, "a shell is found");

        kal_process r{};
        const char*       rargv[] = { "openkal-observed-argv0", "-c", script };
        const kal_uintptr rlens[] = { 22, 2, script_len };
        int rrc = kal_err_invalid;
        if (sh >= 0)
            rrc = kal_process_spawn(&how, sh_paths[sh], sh_lens[sh], rargv, rlens, 3,
                                    nullptr, nullptr, 0, nullptr, &r);
        check(rrc == kal_ok, "a shell is started");
        if (rrc == kal_ok) {
            int status = -1, terminated = -1;
            kal_process_wait(r, &status, &terminated);
            check(status == 0 && terminated == 0,
                  "the started program observes the argument vector the caller supplied, unaltered");
            kal_process_close(r);
        }

        // A PROGRAM THAT NEEDS AN INTERPRETER, WHICH openkal-linux COULD NOT
        // START AND NOBODY HAD MEASURED ANYWHERE.
        //
        // There, `execveat' with a directory descriptor gives the kernel the
        // name as `/dev/fd/<dirfd>/<name>' --- and the kernel hands THAT to the
        // interpreter to open, after the replacement, by which time a
        // close-on-exec descriptor is gone. Every `#!' script was unstartable on
        // every architecture, and it was found only because a foreign binary
        // needs an interpreter too and so aarch64 failed loudly.
        //
        // THIS IMPLEMENTATION IS EXPECTED TO PASS AND IS CHECKED ANYWAY. It
        // has no `execveat': it enters the directory and calls `execve' with a
        // whole path, so the kernel never has a name the interpreter cannot
        // reopen. That is a reason to believe it works, and believing was how
        // the other one went unmeasured for as long as it did.
        {
            const char* mk =
                "printf '#!/bin/sh\\nexit 0\\n' > /tmp/okm-interp.sh && chmod 755 /tmp/okm-interp.sh";
            kal_uintptr mk_len = 0; while (mk[mk_len]) ++mk_len;
            kal_process m{};
            const char*       mav[] = { "sh", "-c", mk };
            const kal_uintptr mlen[] = { 2, 2, mk_len };
            int made = kal_err_invalid;
            if (sh >= 0)
                made = kal_process_spawn(&how, sh_paths[sh], sh_lens[sh], mav, mlen, 3,
                                         nullptr, nullptr, 0, nullptr, &m);
            if (made == kal_ok) {
                int ms = -1, mt = -1;
                kal_process_wait(m, &ms, &mt);
                kal_process_close(m);
            }
            check(made == kal_ok, "a shell makes an executable script to start");

            kal_process s{};
            const char*       sav[] = { "okm-interp.sh" };
            const kal_uintptr slen[] = { 13 };
            const int se = kal_process_spawn(&how, "tmp/okm-interp.sh", 17,
                                             sav, slen, 1,
                                             nullptr, nullptr, 0, nullptr, &s);
            if (se == kal_ok) {
                int ss = -1, st2 = -1;
                kal_process_wait(s, &ss, &st2);
                check(ss == 0 && st2 == 0,
                      "a program that needs an interpreter starts, and runs");
                say("  interpreter: a #! script started and ran\n");
                kal_process_close(s);
            } else {
                check(false, "a program that needs an interpreter starts");
            }
        }
    }

    // A name that ascends is refused here as it is in the file system.
    kal_process bad{};
    const kal_spawn escape{ kal::fs::working(), kal::fs::working(),
                            nullptr, nullptr, 0, 0 };
    check(kal_process_spawn(&escape, "../bin/true", 11,
                            nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, &bad)
          != kal_ok, "an ascending program name is refused");

    // --- openkal.task --------------------------------------------------------
    int written = 0;
    kal_task t{};
    check(kal_task_start(worker, &written, &t) == kal_ok, "an execution context starts");

    // Suspension upon the word, which the worker changes and then wakes. The
    // loop re-examines the condition after waking, because waking is permitted
    // to be spurious and the specification says so.
    while (__atomic_load_n(reinterpret_cast<__UINT32_TYPE__*>(
               const_cast<__UINT32_TYPE__*>(&g_word)), __ATOMIC_SEQ_CST) == 0u) {
        kal_task_wait(const_cast<const __UINT32_TYPE__*>(&g_word), 0u,
                      1000ull * 1000 * 1000);
    }
    check(kal_task_join(t) == kal_ok, "the context is joined");

    int depth = 0;
    kal_task d{};
    check(kal_task_start(deep_worker, &depth, &d) == kal_ok && kal_task_join(d) == kal_ok && depth == 256,
          "a started context holds 4 MiB of frames");
    check(g_ran == 1 && written == 42, "the context ran and observed shared memory");

    // A wait whose expected value does not match returns rather than suspending,
    // which is what makes the primitive usable without losing a wake.
    __UINT32_TYPE__ other = 5;
    check(kal_task_wait(&other, 6u, 1000ull * 1000) == kal_ok,
          "a wait on a value that already differs returns");

    kal_task_yield();
    check(kal_task_current() != 0, "the calling context has an identity");

    // --- being told that an end has been requested ---------------------------
    //
    // THIS RAISES THE SIGNAL. Compiling a disposition and never delivering
    // one proves nothing here: what this operation needs on this kernel is a
    // TRAMPOLINE, `sa_tramp' in the structure the raw `sigaction' takes, and a
    // wrong one is not a wrong answer --- it is a program that dies inside the
    // handler, at an address nobody can attribute. openkal-macos declined to
    // claim KAL_PROCESS_PROP_STOP_REQUESTED until this check existed, and this
    // comment is why the order was that way round.
    //
    // The signal is raised by a shell, which is how a program reaches its own
    // kernel here without leaving openkal's vocabulary: `kal_process_job_enter'
    // with a zero unit reports the identifier this program runs under, and that
    // is the identifier the shell needs.
    {
        const kal_u32* word = kal_process_stop_requested();
        if (word == nullptr) {
            // Declined, which clause 6.2 permits and KAL_PROCESS_PROP_STOP_REQUESTED
            // states. Nothing below applies --- and the log says which of the two
            // this run was, for the reason given at the observation further down.
            say("  stop-request: declined, and the position is not claimed\n");
            check((kal_process_props() & kal::macros::KAL_PROCESS_PROP_STOP_REQUESTED_M) == 0,
                  "an implementation that answers no word does not claim the position");
        } else {
            check((kal_process_props() & kal::macros::KAL_PROCESS_PROP_STOP_REQUESTED_M) != 0,
                  "an implementation that answers a word claims the position");
            check(*word == 0, "and nothing has asked this program to end yet");

            kal_job unit{};
            const int entered = kal_process_job_enter(&unit);
            check(entered == kal_ok, "this program's identifier is reported");

            if (entered == kal_ok && unit.h != 0) {
                // "kill -TERM <pid>" with the number written out. The shell is
                // given a moment first so that the wait below is entered rather
                // than raced past --- the check does not depend on it, because a
                // word already set makes the wait return at once.
                char script[64];
                kal_uintptr n = 0;
                const char lead[] = "sleep 0.3; kill -TERM ";
                for (kal_uintptr i = 0; i < sizeof(lead) - 1; ++i) script[n++] = lead[i];
                char digits[24]; int d = 24;
                kal_uintptr v = unit.h;
                if (v == 0) digits[--d] = '0';
                while (v > 0) { digits[--d] = static_cast<char>('0' + v % 10); v /= 10; }
                while (d < 24) script[n++] = digits[d++];
                script[n] = 0;

                // Located by asking, as above --- the lambda that does it is
                // scoped to the block above, and duplicating four lines is
                // better than widening something for one caller.
                const char* sh_paths[] = { "bin/sh", "usr/bin/sh" };
                const kal_uintptr sh_lens[] = { 6, 10 };
                int sh = -1;
                for (int i = 0; i < 2 && sh < 0; ++i) {
                    kal_node_info info{}; info.self_size = sizeof info;
                    if (kal_fs_info(slash, sh_paths[i], sh_lens[i], 0,
                                    kal::fs::field::kind, &info) != kal_ok) continue;
                    if (info.kind != kal_node_absent) sh = i;
                }
                check(sh >= 0, "a shell is found to raise the signal");

                kal_process k{};
                const kal_spawn how{ slash, slash, nullptr, nullptr, 0, 0 };
                const char*       kargv[] = { "sh", "-c", script };
                const kal_uintptr klens[] = { 2, 2, n };
                int krc = kal_err_invalid;
                if (sh >= 0)
                    krc = kal_process_spawn(&how, sh_paths[sh], sh_lens[sh],
                                            kargv, klens, 3,
                                            nullptr, nullptr, 0, nullptr, &k);
                check(krc == kal_ok, "the program that raises the signal starts");

                if (krc == kal_ok) {
                    // Up to five seconds, in bounded waits upon the word itself
                    // --- which is the use the word was specified for.
                    for (int i = 0; i < 50 && *word == 0; ++i)
                        kal_task_wait(word, 0u, 100ull * 1000 * 1000);

                    check(*word != 0, "the program is told that its end was requested");

                    // AND IT IS STILL RUNNING, which is the half a compiled
                    // disposition cannot show. Reaching this line is the proof:
                    // a program that died in the handler never gets here.
                    //
                    // SAID ALOUD, AND EVERY OTHER CHECK HERE IS SILENT WHEN
                    // IT HOLDS. That convention cannot serve this one. Each
                    // observation above is skipped rather than failed when its
                    // precondition is absent --- no root, no word, no shell ---
                    // so a green run is consistent BOTH with a trampoline that
                    // was entered and returned and with a block that never ran.
                    // Those are the two outcomes this check exists to tell
                    // apart, and only a line in the log tells them apart.
                    if (*word != 0) say("  stop-request: told, and still running\n");

                    int status = -1, terminated = -1;
                    kal_process_wait(k, &status, &terminated);
                    kal_process_close(k);
                }
            }
        }
    }

    const char ok[] = "openkal-macos: process and task conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return failures == 0 ? 0 : 1;
}
