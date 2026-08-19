#pragma once

#include <string>

// Small, general CreateProcessW-based subprocess runner — adapted from the
// pattern already used by orchestrator/AgentTurn.cpp's RunProcessCapture
// (that one is worker/node-specific: stdin write + stdout capture with
// stderr discarded to protect JSON parsing). This one is simpler: no stdin,
// and BOTH stdout and stderr are captured together into outOutput (so a
// caller like `gh repo clone` failure gets the actual error text, which gh
// writes to stderr).
namespace ProcessRunner {

// Runs `commandLine` with working directory `cwd` (may be empty to inherit
// the caller's). Captures combined stdout+stderr into outOutput and the
// process's exit code into outExitCode. Returns false only if the process
// could not be created at all (e.g. the executable isn't on PATH) — a
// nonzero outExitCode is a normal (not a `false`-returning) failure mode,
// distinguishable by the caller via outExitCode.
bool RunCommand(
    const std::wstring& commandLine, const std::wstring& cwd, std::string& outOutput, int& outExitCode);

} // namespace ProcessRunner
