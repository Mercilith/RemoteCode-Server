#pragma once

// Windows Service entry point. Tries to run as an SCM-dispatched service;
// if StartServiceCtrlDispatcherW reports this process wasn't launched by
// the SCM (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT — e.g. run directly
// from a terminal for local debugging), falls back to running the same
// main loop directly in the console instead of requiring an install for
// every test run. Either way, blocks until shutdown and returns a process
// exit code.
class ServiceMain {
public:
    static int Run();
};
