#ifndef METROID_PRIME_PORT_PORT_CRASH_H
#define METROID_PRIME_PORT_PORT_CRASH_H

// Writes a crash report into the log: what went wrong, where (module + offset,
// which a symbolizer turns back into a line of code), the build, and the stack.
// Nothing else changes about the crash, so the system still gets it afterwards:
// Windows Error Reporting (Event Viewer), a core dump on Linux, Android's
// tombstone. Windows also writes <user folder>/metroid_prime_port.dmp, and its
// report has function names and lines when metroid_prime_port.pdb is beside the
// program (the release zip has it).
//
// MP_CRASH_TEST=segv|abort crashes right after Install, to check the report.
namespace PortCrash {

void Install();

} // namespace PortCrash

#endif // METROID_PRIME_PORT_PORT_CRASH_H
