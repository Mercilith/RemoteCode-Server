import { query } from "@anthropic-ai/claude-agent-sdk";

interface TurnMessage {
  senderType: string;
  senderId: string;
  content: string;
}

interface TurnRequest {
  systemPrompt: string;
  // Recent chat history, oldest first — this pass replays it in full each
  // turn instead of resuming an SDK session (see plan: Section 5 deferred).
  messages: TurnMessage[];
  // Command to spawn the orchestrator's hand-rolled MCP server over stdio,
  // already scoped to this turn's agent id by the caller.
  mcpServerCommand: string;
  mcpServerArgs: string[];
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
  let errorMessage = "";

  try {
    const raw = await readStdin();
    const request: TurnRequest = JSON.parse(raw);

    const transcript = request.messages
      .map((m) => `[${m.senderType}:${m.senderId}] ${m.content}`)
      .join("\n");
    const prompt = `${transcript}\n\nRespond with your next message in the conversation above.`;

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
      },
    })) {
      if (message.type === "result") {
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
  process.stdout.write(JSON.stringify({ response }) + "\n");
}

main();
