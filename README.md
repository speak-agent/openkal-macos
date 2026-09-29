# openkal-macos

An implementation of [openkal](https://github.com/mcpplibs/openkal) for macOS,
written on the kernel's own calls.

```toml
[dependencies]
openkal = "0.14.0"

[target.'cfg(os = "macos")'.dependencies]
openkal-macos = "0.12.0"
```

Its purpose is as much to test the specification as to be used. A specification
satisfied only by the system it was written against has not been shown to be
portable, however many programs that system hosts; a second implementation on a
different system is what turns the claim into an observation.

## Interfaces provided

All fifteen. `tools/check-surface.sh --complete` in the specification package
compares the exported names against `SURFACE.txt`.

The five that openkal 0.8 added were declined by this implementation until now,
and each is provided in whole:

| | on this system |
| --- | --- |
| `openkal.net` | the kernel's own socket calls. There is no `accept4` and no flag upon `socket` that closes a descriptor across a spawn, so close-on-exec is set afterwards with `fcntl` |
| `openkal.datagram` | the same calls, with `SOCK_DGRAM` |
| `openkal.timeout` | `poll`, whose bound is stated in milliseconds. `ppoll` does not exist here, so a millisecond is the granularity this implementation reports — what the environment can distinguish rather than what would be convenient |
| `openkal.space` | the duplication primitive `openkal.process` was already built on. The duplicate is distinguished by the call's **second** return value: both images receive a process identifier in the first, so an implementation testing that one against zero would decide that neither image was the duplicate |
| `openkal.exec` | a mapping obtained writable and made executable afterwards, which is the only order this system permits. The instruction-cache maintenance is **not** performed here: `__builtin___clear_cache` becomes a call to `___clear_cache` in the compiler's support library on `arm64`, and this implementation is reachable from a program that carries no other runtime. The specification places that maintenance upon the program |

**Clause 6.5 names this system and `openkal.exec` in terms**, and says
availability may be settled by how the artifact is *produced* — a signed
declaration applied after the link. That clause is about memory which is
writable and executable **at the same time**, which is not what this interface
offers: a region here is writable, then published, then executable, and never
both. The conformance suite reserves a region, writes an instruction sequence
into it, publishes it, **calls it**, and compares what it returned — so the
reading is settled by the system rather than by this paragraph. If that
observation stops holding, the remedy is clause 6.5's: a feature of this
package, provided at dependency resolution.

The package exports no module: the interface belongs to the specification, and
this package supplies definitions.

## Why it does not borrow the system's C library

openkal says nothing about what else a program contains, and a program above it
may supply every name the system's own library supplies. If it does, and the
names agree, this implementation's calls resolve to the program's and the
program's resolve back here. The recursion is unbounded and it appears in
neither side's source.

Version 0.2 of this implementation borrowed the system's library. That is a
correct implementation of openkal for a program that borrows nothing, and it is
wrong for the program clause 1 names first among the consumers the specification
expects. Version 0.3 issues the kernel's calls instead.

**Two names remain, and both were chosen for one property: no C library defines
them.** A program that defines every ordinary name still leaves them reachable.

| name | why a kernel call could not replace it |
| --- | --- |
| `clock_gettime_nsec_np` | this system counts elapsed time in a unit of the processor's; the conversion lives in a library rather than in the kernel |
| `pthread_create_from_mach_thread` | creating an execution context here means arranging state the kernel does not arrange, and reproducing that arrangement would be reproducing a part of this system inside an implementation of openkal |

The second is required only under the `standalone` feature. Which names were
reachable and which were not was measured on the system rather than remembered:
`.github/workflows/probe.yml` asked it, and the sources are written against its
answers.

## The `standalone` feature

Whether this implementation is the whole of the program's environment.

Ordinarily a program already carries a runtime of its own, that runtime has
received control from the loader, and it creates execution contexts. This
implementation borrows both, and that is the default.

Sometimes there is no such runtime — because the program supplies one itself, or
because it has none. Then nothing in the program has received control and
nothing creates contexts, and this implementation supplies the entry point and
creates contexts through the one name above. That is what `standalone` selects.

It is a statement about the program, not a smaller or faster variant of the
implementation, and the consumer that knows which arrangement holds is the one
that declares it.

## Where this system differs from Linux, and what each difference demonstrates

These are recorded because they are the return on writing a second
implementation. Each is a place where an interface could have assumed a
mechanism, and did not.

**A failure is reported by a flag, not by the sign of the result.** The kernel's
convention is not the one openkal's reference implementation meets, and openkal
does not name a convention: an operation reports one of thirteen values from a
closed set, and how the environment reported it is the implementation's business.

**The monotonic clock continues during suspension.** On Linux it stops. A
program measuring an interval across a suspension obtains different answers from
the two, and no operation reports which it is dealing with.
`KAL_TIME_PROP_MONOTONIC_SUSPENDS` exists for this, and this divergence is the
clearest evidence in either implementation that the properties of clause 6.2
were necessary rather than decorative.

**Names are compared without regard to case.** A program that creates two names
differing only in case succeeds on one implementation and not on the other.
`KAL_FS_PROP_CASE_SENSITIVE` reports it in advance, which no operation could.

**There is no kernel call that suspends for a duration.** What this system's own
library uses is a wait upon an object a program carrying no other runtime does
not have. So the wait `openkal.task` already requires is used, upon an address
nothing ever wakes. That is not a substitute for a sleep; it is a sleep,
expressed with the operation this system has.

**There is no kernel call that starts a program relative to a directory.** The
directory is entered by the duplicate before it replaces itself — a duplicate
that exists for the length of two calls and is not a resource the caller
receives, which is exactly what openkal declines to offer as an operation of its
own. The caller's working directory is untouched, which is the property the
interface requires.

**The suspension primitive exists here too.** `openkal.task` declares its
boundary as a wait upon a word. This kernel offers that operation under a
different name and with no shared ancestry with the one Linux offers. That two
unrelated systems provide it is the evidence that it is the shape of the thing
rather than the shape of one kernel — and version 0.2, which built the primitive
out of a mutex and a condition variable, had this backwards.

## Part of a region published (0.13.0)

openkal 0.15's `kal_exec_publish_part` is `mprotect` on the part, in the page
this system protects in (`kal_exec_granularity`, the same page the rest of
`exec.cpp` rounds to: 16 KiB on arm64). Cross-built from Linux; not yet run on
the system.

## Conformance

The suite lives in the specification package and is the same suite every
implementation runs. That it is the same suite is the point: a conformance
suite that differed between implementations would be testing implementations
rather than the specification.

```bash
git clone https://github.com/mcpplibs/openkal .spec
bash .spec/tools/run-conformance.sh openkal-macos . full,optional
```

## Architectures

`arm64` and `x86_64`. The system-call numbers are the same on both; what differs
is the calling convention, the register the current context's record is reached
through, and one field of a signal context. All three are in `src/sys.h`.

## License

Apache-2.0.
