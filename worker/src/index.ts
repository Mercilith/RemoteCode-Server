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

    let prompt: string;
    if (request.resumeSessionId) {
      // Resuming: the SDK already has everything up through this agent's
      // last turn loaded from the session file — only the newest message
      // (the one that triggered this turn) needs to go in fresh.
      const latest = request.messages[request.messages.length - 1];
      prompt = latest?.content ?? "";
    } else {
      const transcript = request.messages
        .map((m) => `[${m.senderType}:${m.senderId}] ${m.content}`)
        .join("\n");
      prompt = `${transcript}\n\nRespond with your next message in the conversation above.`;
    }

    for await (const message of query({
      prompt,
      options: {
        systemPrompt: request.systemPrompt,
        // No built-in tools (Bash/Read/Edit/...) — this agent only gets the
        // orchestrator's own MCP tools (post_message, read_chat).
        tools: [],
        mcpServers: {
          orchestrator: {
            type: "stdio",
            command: request.mcpServerCommand,
            args: request.mcpServerArgs ?? [],
          },
        },
        // No interactive terminal exists to approve tool calls, and the
        // only tools available are our own scoped MCP tools.
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
