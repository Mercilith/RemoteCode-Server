import { query } from "@anthropic-ai/claude-agent-sdk";

interface TurnMessage {
  senderType: string;
  senderId: string;
  content: string;
}

interface TurnRequest {
  systemPrompt: string;
  // Recent chat history, oldest first. Always sent, but only actually used
  // as the prompt when resumeSessionId is empty — see below.
  messages: TurnMessage[];
  // Command to spawn the orchestrator's hand-rolled MCP server over stdio,
  // already scoped to this turn's agent id by the caller.
  mcpServerCommand: string;
  mcpServerArgs: string[];
  // Path to an already-`claude /login`'d user's Claude Code config
  // directory (normally %USERPROFILE%\.claude). The orchestrator runs as
  // a Windows Service under SYSTEM, which has no OAuth session of its own —
  // this lets the worker borrow a real user's cached session instead of
  // requiring ANTHROPIC_API_KEY. Empty if not configured.
  claudeConfigDir: string;
  // SDK session id from a previous turn for this (agent, chat) pair, if
  // the orchestrator has one on file. When set, the SDK reloads that
  // session's full history itself (via `resume`), so the prompt only needs
  // to carry the newest message rather than the whole transcript again.
  resumeSessionId: string;
  // True if the message that triggered this turn explicitly @tagged this
  // agent. Every active agent in a chat gets a turn on every message (so
  // it's always in context) — this tells it whether a reply is expected
  // (tagged) or optional, its own call (not tagged). See addressingNote
  // below, which is what actually carries this to the model.
  tagged: boolean;
  // Non-empty only for a repo-onboarding-created agent (see
  // AgentStore::repoLocalPath / Orchestrator::HandleReaction) — the local
  // checkout directory this agent is the dedicated expert for. When set,
  // this agent gets real file/Bash tool access scoped to this directory
  // (see the `tools`/`cwd` branch below). Empty for every other agent,
  // which keeps today's zero-built-in-tools behavior unchanged.
  repoLocalPath: string;
}

async function readStdin(): Promise<string> {
  const chunks: Buffer[] = [];
  for await (const chunk of process.stdin) {
    chunks.push(chunk as Buffer);
  }
  return Buffer.concat(chunks).toString("utf-8");
}

async function main(): Promise<void> {
  let response = "";
  let sessionId = "";
  let errorMessage = "";

  try {
    const raw = await readStdin();
    const request: TurnRequest = JSON.parse(raw);

    if (request.claudeConfigDir) {
      process.env.CLAUDE_CONFIG_DIR = request.claudeConfigDir;
    }

    // Every active agent in a chat gets a turn on every message now (not
    // just ones that @tag it), so it stays free-form conversational rather
    // than a rigid always-must-reply loop — this is what tells the model
    // whether a reply is expected or genuinely optional this turn. Repeated
    // on every turn (not just baked into systemPrompt) because a resumed
    // turn's prompt is otherwise just the bare latest message, and tagged
    // status varies message to message.
    // Models are reluctant to emit a truly empty response even when told
    // to — they tend to write something like "(no response needed)"
    // instead, which is non-empty text that gets posted as a real message.
    // An exact sentinel word is far more reliable than "say nothing."
    const addressingNote = request.tagged
      ? "[You were explicitly @tagged in this message — you should respond.]"
      : "[You were not @tagged in this message. Only reply if you genuinely have " +
        'something useful to add; otherwise your ENTIRE response must be exactly ' +
        'the single word SILENT and nothing else (no punctuation, no parentheses, ' +
        'no explanation) — that is how you decline to reply, and it is expected ' +
        "and normal, not a failure.]";

    let prompt: string;
    if (request.resumeSessionId) {
      // Resuming: the SDK already has everything up through this agent's
      // last turn loaded from the session file — only the newest message
      // (the one that triggered this turn) needs to go in fresh.
      const latest = request.messages[request.messages.length - 1];
      prompt = `${addressingNote}\n\n${latest?.content ?? ""}`;
    } else {
      const transcript = request.messages
        .map((m) => `[${m.senderType}:${m.senderId}] ${m.content}`)
        .join("\n");
      prompt =
        `${transcript}\n\n${addressingNote}\nRespond with your next message in the ` +
        `conversation above, or SILENT if you're choosing not to respond.`;
    }

    // Real capability expansion, confirmed intentionally by Cardon
    // ("full tools now") despite this whole service running as a
    // SYSTEM-privileged Windows Service: a repo-onboarding-created agent
    // (repoLocalPath set) gets real Read/Write/Edit/Bash/Glob/Grep tools,
    // with the SDK's own `cwd` option (confirmed via
    // worker/node_modules/@anthropic-ai/claude-agent-sdk/sdk.d.ts —
    // `Options.cwd?: string`, "Current working directory for the session")
    // pointed at its repo's checkout directory. This is a SOFT, prompt-level
    // boundary only — cwd tells the SDK/Bash tool where to default to, it is
    // NOT an OS-level sandbox. Bash can still `cd ..`, use absolute paths,
    // or otherwise act anywhere the SYSTEM account itself can reach; a
    // determined or simply confused agent is not physically prevented from
    // leaving `repoLocalPath`. Every other agent keeps today's exact
    // behavior: no built-in tools, no cwd override, MCP-only.
    const hasRepoAccess = Boolean(request.repoLocalPath);

    for await (const message of query({
      prompt,
      options: {
        systemPrompt: request.systemPrompt,
        tools: hasRepoAccess ? ["Read", "Write", "Edit", "Bash", "Glob", "Grep"] : [],
        ...(hasRepoAccess ? { cwd: request.repoLocalPath } : {}),
        mcpServers: {
          orchestrator: {
            type: "stdio",
            command: request.mcpServerCommand,
            args: request.mcpServerArgs ?? [],
          },
        },
        // No interactive terminal exists to approve tool calls, and the
        // only tools available are our own scoped MCP tools plus (for a
        // repo-scoped agent) the built-in file/Bash tools above.
        permissionMode: "bypassPermissions",
        allowDangerouslySkipPermissions: true,
        ...(request.resumeSessionId ? { resume: request.resumeSessionId } : {}),
      },
    })) {
      if (message.type === "result") {
        sessionId = message.session_id;
        if (message.subtype === "success") {
          response = message.result;
        } else {
          errorMessage = `agent turn failed: ${message.subtype}`;
        }
      }
    }
  } catch (err) {
    errorMessage = err instanceof Error ? err.message : String(err);
  }

  if (errorMessage) {
    process.stdout.write(JSON.stringify({ error: errorMessage }) + "\n");
    process.exitCode = 1;
    return;
  }
  process.stdout.write(JSON.stringify({ response, sessionId }) + "\n");
}

main();
