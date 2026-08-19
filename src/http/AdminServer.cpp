#include "AdminServer.h"

#include <httplib.h>

#include <ctime>
#include <sstream>

#include "../discord/AgentBotClient.h"
#include "../orchestrator/AgentTurn.h"
#include "../third_party/json.hpp"
#include "../util/Text.h"

using nlohmann::json;

namespace {

json AgentToJson(const Agent& agent, bool includeDetail) {
    json out;
    out["id"] = agent.id;
    out["name"] = agent.name;
    out["description"] = agent.description;
    out["status"] = agent.status;
    out["has_own_bot"] = !agent.discordBotTokenEncrypted.empty();
    out["bot_username"] = agent.discordBotUsername;
    if (includeDetail) {
        out["system_prompt"] = agent.systemPrompt;
        try {
            out["tool_permissions"] = json::parse(agent.toolPermissionsJson);
        } catch (const json::parse_error&) {
            out["tool_permissions"] = json::array();
        }
        try {
            out["can_message"] = json::parse(agent.canMessageJson);
        } catch (const json::parse_error&) {
            out["can_message"] = json::array();
        }
    }
    return out;
}

json RepoToJson(const Repo& repo) {
    return json{
        {"id", repo.id},
        {"github_url", repo.githubUrl},
        {"local_path", repo.localPath},
        {"agent_id", repo.agentId},
        {"status", repo.status},
        {"notes", repo.notes},
        {"last_error", repo.lastError},
        {"created_at", repo.createdAt},
        {"updated_at", repo.updatedAt},
    };
}

json WorkspaceToJson(const Workspace& workspace) {
    json repoIds = json::array();
    try {
        repoIds = json::parse(workspace.repoIdsJson);
    } catch (const json::parse_error&) {
        repoIds = json::array();
    }
    return json{
        {"id", workspace.id},
        {"title", workspace.title},
        {"repo_ids", repoIds},
        {"discord_category_id", workspace.discordCategoryId},
        {"chat_id", workspace.chatId},
        {"created_by", workspace.createdBy},
        {"status", workspace.status},
        {"last_error", workspace.lastError},
        {"created_at", workspace.createdAt},
        {"updated_at", workspace.updatedAt},
    };
}

json PromptTemplateToJson(const PromptTemplate& t) {
    return json{{"name", t.name}, {"content", t.content}, {"updated_at", t.updatedAt}};
}

void SendError(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), "application/json");
}

void SendJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Returns false (and sends a 400) if the body isn't valid JSON.
bool ParseBody(const httplib::Request& req, httplib::Response& res, json& outBody) {
    try {
        outBody = json::parse(req.body);
        return true;
    } catch (const json::parse_error&) {
        SendError(res, 400, "invalid JSON body");
        return false;
    }
}

} // namespace

AdminServer::AdminServer(
    AgentStore& agentStore, AgentSessionStore& agentSessionStore, ChatStore& chatStore,
    RepoStore& repoStore, WorkspaceStore& workspaceStore, PromptTemplateStore& promptTemplateStore,
    std::wstring dbPath, std::string claudeConfigDir, std::wstring logDir,
    InjectMessageFn injectMessage, AddRepoFn addRepo, CreateWorkspaceFn createWorkspace)
    : agentStore_(agentStore),
      agentSessionStore_(agentSessionStore),
      chatStore_(chatStore),
      repoStore_(repoStore),
      workspaceStore_(workspaceStore),
      promptTemplateStore_(promptTemplateStore),
      dbPath_(std::move(dbPath)),
      claudeConfigDir_(std::move(claudeConfigDir)),
      logDir_(std::move(logDir)),
      injectMessage_(std::move(injectMessage)),
      addRepo_(std::move(addRepo)),
      createWorkspace_(std::move(createWorkspace)) {}

AdminServer::~AdminServer() = default;

void AdminServer::Run(int port) {
    server_ = std::make_unique<httplib::Server>();

    server_->Get("/agents", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        for (const Agent& agent : agentStore_.ListAll()) {
            out.push_back(AgentToJson(agent, false));
        }
        SendJson(res, out);
    });

    server_->Get("/agents/:id", [this](const httplib::Request& req, httplib::Response& res) {
        Agent agent;
        if (!agentStore_.Get(req.path_params.at("id"), agent)) {
            SendError(res, 404, "no agent with that id");
            return;
        }
        SendJson(res, AgentToJson(agent, true));
    });

    server_->Post("/agents", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("name") || !body.contains("description") || !body.contains("system_prompt")) {
            SendError(res, 400, "name, description, and system_prompt are required");
            return;
        }

        const std::string name = body["name"].get<std::string>();
        const std::string id = Slugify(name);
        Agent existing;
        if (agentStore_.Get(id, existing)) {
            SendError(res, 409, "an agent with id '" + id + "' already exists");
            return;
        }

        Agent agent;
        agent.id = id;
        agent.name = name;
        agent.description = body["description"].get<std::string>();
        agent.systemPrompt = body["system_prompt"].get<std::string>();
        agent.status = "active"; // direct desktop creation skips the Discord approval workflow
        agent.toolPermissionsJson = body.value("tool_permissions", json::array()).dump();
        agent.canMessageJson = body.value("can_message", json::array({"*"})).dump();
        agent.createdBy = "user";
        const int64_t now = static_cast<int64_t>(time(nullptr));
        agent.createdAt = now;
        agent.updatedAt = now;

        if (!agentStore_.Upsert(agent)) {
            SendError(res, 500, "failed to save the new agent");
            return;
        }
        SendJson(res, AgentToJson(agent, true), 201);
    });

    server_->Patch("/agents/:id", [this](const httplib::Request& req, httplib::Response& res) {
        Agent agent;
        if (!agentStore_.Get(req.path_params.at("id"), agent)) {
            SendError(res, 404, "no agent with that id");
            return;
        }
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }

        if (body.contains("name")) {
            agent.name = body["name"].get<std::string>();
        }
        if (body.contains("description")) {
            agent.description = body["description"].get<std::string>();
        }
        if (body.contains("status")) {
            agent.status = body["status"].get<std::string>();
        }
        if (body.contains("system_prompt")) {
            agent.systemPrompt = body["system_prompt"].get<std::string>();
        }
        if (body.contains("tool_permissions")) {
            agent.toolPermissionsJson = body["tool_permissions"].dump();
        }
        if (body.contains("can_message")) {
            agent.canMessageJson = body["can_message"].dump();
        }
        agent.updatedAt = static_cast<int64_t>(time(nullptr));

        if (!agentStore_.Upsert(agent)) {
            SendError(res, 500, "failed to save the update");
            return;
        }
        SendJson(res, AgentToJson(agent, true));
    });

    server_->Post("/agents/:id/bot-token", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agentId = req.path_params.at("id");
        Agent agent;
        if (!agentStore_.Get(agentId, agent)) {
            SendError(res, 404, "no agent with that id");
            return;
        }
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("token") || body["token"].get<std::string>().empty()) {
            SendError(res, 400, "'token' is required");
            return;
        }
        const std::string token = body["token"].get<std::string>();

        // Validates the token as a side effect — a bad/revoked token fails
        // here rather than silently getting stored.
        AgentBotClient client(token);
        std::string botUserId, botUsername;
        if (!client.FetchSelf(botUserId, botUsername)) {
            SendError(
                res, 400,
                "Discord rejected that token — double check it's correct and the bot application still "
                "exists");
            return;
        }
        if (!agentStore_.SetDiscordBotToken(agentId, token, botUserId, botUsername)) {
            SendError(res, 500, "failed to save the bot token");
            return;
        }
        SendJson(res, json{{"bot_user_id", botUserId}, {"bot_username", botUsername}});
    });

    server_->Delete("/agents/:id/bot-token", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agentId = req.path_params.at("id");
        Agent agent;
        if (!agentStore_.Get(agentId, agent)) {
            SendError(res, 404, "no agent with that id");
            return;
        }
        if (!agentStore_.ClearDiscordBotToken(agentId)) {
            SendError(res, 500, "failed to clear the bot token");
            return;
        }
        res.status = 204;
    });

    server_->Post("/agents/:id/revise", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agentId = req.path_params.at("id");
        Agent target;
        if (!agentStore_.Get(agentId, target)) {
            SendError(res, 404, "no agent with that id");
            return;
        }
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("instruction") || body["instruction"].get<std::string>().empty()) {
            SendError(res, 400, "'instruction' is required");
            return;
        }
        const std::string instruction = body["instruction"].get<std::string>();

        Agent alex;
        if (!agentStore_.Get("alex", alex)) {
            SendError(res, 500, "Alex is not available to make this revision");
            return;
        }

        std::ostringstream context;
        context << "Cardon is asking you (via the desktop app, not Discord) to revise an existing "
                   "agent.\n\n"
                << "Agent to revise:\n"
                << "  id: " << target.id << "\n"
                << "  name: " << target.name << "\n"
                << "  description: " << target.description << "\n"
                << "  system_prompt: " << target.systemPrompt << "\n"
                << "  tool_permissions: " << target.toolPermissionsJson << "\n"
                << "  can_message: " << target.canMessageJson << "\n\n"
                << "Cardon's instruction: " << instruction << "\n\n"
                << "If the instruction is clear enough to act on, call update_agent with agent_id=\""
                << target.id
                << "\" and whichever fields should change, then briefly explain what you changed. If "
                   "you need more information first, ask a clarifying question instead of guessing.";

        Message contextMessage;
        contextMessage.senderType = "user";
        contextMessage.senderId = "desktop";
        contextMessage.type = "text";
        contextMessage.content = context.str();
        contextMessage.createdAt = static_cast<int64_t>(time(nullptr));

        const std::string reviseChatId = "desktop-revise-" + target.id;
        std::string resumeSessionId;
        // Design-spec Section 12, Decision #4: don't resume a session that's
        // been idle over an hour — start fresh instead.
        agentSessionStore_.GetIfFresh(
            alex.id, reviseChatId, kDefaultSessionIdleTimeoutSeconds, resumeSessionId);

        // Treated as tagged: Cardon asked Alex directly for this, unlike an
        // ordinary chat turn where a reply may or may not be warranted.
        const AgentTurnResult result = AgentTurn::Run(
            alex, {contextMessage}, dbPath_, claudeConfigDir_, reviseChatId, resumeSessionId, logDir_,
            /*tagged=*/true);
        if (!result.ok) {
            if (!resumeSessionId.empty()) {
                agentSessionStore_.Clear(alex.id, reviseChatId);
            }
            SendError(res, 502, result.error);
            return;
        }
        if (!result.sdkSessionId.empty()) {
            agentSessionStore_.Set(alex.id, reviseChatId, result.sdkSessionId);
        }

        Agent updated = target;
        agentStore_.Get(target.id, updated); // pick up whatever update_agent changed, if anything
        SendJson(res, json{{"reply", result.response}, {"agent", AgentToJson(updated, true)}});
    });

    // --- Repo onboarding endpoints --------------------------------------
    // Exact contract — RemoteCode-Desktop is being built against this in
    // parallel, see the design doc's admin API section.

    server_->Post("/repos", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("github_url") || body["github_url"].get<std::string>().empty()) {
            SendError(res, 400, "'github_url' is required");
            return;
        }
        const std::string githubUrl = body["github_url"].get<std::string>();
        const std::string notes = body.value("notes", std::string());

        if (!addRepo_) {
            SendError(res, 500, "repo onboarding is not wired up");
            return;
        }
        std::string error;
        const std::string repoId = addRepo_(githubUrl, notes, error);
        if (repoId.empty()) {
            SendError(res, 400, error.empty() ? "could not parse github_url" : error);
            return;
        }

        Repo repo;
        const std::string status = repoStore_.Get(repoId, repo) ? repo.status : "cloning";
        SendJson(res, json{{"repo_id", repoId}, {"status", status}}, 201);
    });

    server_->Get("/repos", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        for (const Repo& repo : repoStore_.ListAll()) {
            out.push_back(RepoToJson(repo));
        }
        SendJson(res, out);
    });

    server_->Get("/repos/:id", [this](const httplib::Request& req, httplib::Response& res) {
        Repo repo;
        if (!repoStore_.Get(req.path_params.at("id"), repo)) {
            SendError(res, 404, "no repo with that id");
            return;
        }
        SendJson(res, RepoToJson(repo));
    });

    // --- Workspace endpoints ---------------------------------------------

    server_->Post("/workspaces", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("repo_ids") || !body["repo_ids"].is_array() || body["repo_ids"].empty()) {
            SendError(res, 400, "'repo_ids' must be a non-empty array");
            return;
        }
        std::vector<std::string> repoIds;
        for (const json& v : body["repo_ids"]) {
            if (!v.is_string()) {
                SendError(res, 400, "'repo_ids' entries must be strings");
                return;
            }
            repoIds.push_back(v.get<std::string>());
        }
        const std::string title = body.value("title", std::string());

        if (!createWorkspace_) {
            SendError(res, 500, "workspace creation is not wired up");
            return;
        }
        std::string error;
        const std::string workspaceId = createWorkspace_(repoIds, title, error);
        if (workspaceId.empty()) {
            SendError(res, 400, error.empty() ? "could not create workspace" : error);
            return;
        }

        Workspace workspace;
        const std::string status = workspaceStore_.Get(workspaceId, workspace) ? workspace.status : "creating";
        SendJson(res, json{{"id", workspaceId}, {"status", status}}, 201);
    });

    server_->Get("/workspaces", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        for (const Workspace& workspace : workspaceStore_.ListAll()) {
            out.push_back(WorkspaceToJson(workspace));
        }
        SendJson(res, out);
    });

    server_->Get("/workspaces/:id", [this](const httplib::Request& req, httplib::Response& res) {
        Workspace workspace;
        if (!workspaceStore_.Get(req.path_params.at("id"), workspace)) {
            SendError(res, 404, "no workspace with that id");
            return;
        }
        SendJson(res, WorkspaceToJson(workspace));
    });

    server_->Get("/prompt-templates", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        for (const PromptTemplate& t : promptTemplateStore_.ListAll()) {
            out.push_back(PromptTemplateToJson(t));
        }
        SendJson(res, out);
    });

    server_->Patch("/prompt-templates/:name", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.path_params.at("name");
        std::string existingContent;
        if (!promptTemplateStore_.Get(name, existingContent)) {
            SendError(res, 404, "no prompt template with that name");
            return;
        }
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("content")) {
            SendError(res, 400, "'content' is required");
            return;
        }
        const std::string content = body["content"].get<std::string>();
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (!promptTemplateStore_.Set(name, content, now)) {
            SendError(res, 500, "failed to save the prompt template");
            return;
        }
        SendJson(res, json{{"name", name}, {"content", content}, {"updated_at", now}});
    });

    // --- DEBUG/DEV-ONLY endpoints -------------------------------------
    // Exercise the message-dispatch pipeline (incoming message -> agent
    // turns -> tagging -> replies) without touching real Discord. Same
    // trust boundary as the rest of this API (loopback-only, no auth) —
    // see the class comment in AdminServer.h.

    server_->Get("/debug/chats", [this](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        // includeArchived=true — this is a debug view, nothing should
        // silently disappear from it once a chat is closed/archived.
        for (const Chat& chat : chatStore_.ListChats(/*includeArchived=*/true)) {
            out.push_back(json{
                {"id", chat.id},
                {"title", chat.title},
                {"status", chat.status},
                {"discord_channel_id", chat.discordChannelId},
                {"created_by", chat.createdBy},
                {"participant_agent_ids", chatStore_.ListParticipantAgentIds(chat.id)},
            });
        }
        SendJson(res, out);
    });

    server_->Post("/debug/inject-message", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!ParseBody(req, res, body)) {
            return;
        }
        if (!body.contains("chat_id") || body["chat_id"].get<std::string>().empty()) {
            SendError(res, 400, "'chat_id' is required");
            return;
        }
        if (!body.contains("content") || body["content"].get<std::string>().empty()) {
            SendError(res, 400, "'content' is required");
            return;
        }
        const std::string chatId = body["chat_id"].get<std::string>();
        const std::string content = body["content"].get<std::string>();
        const std::string senderId = body.value("sender_id", std::string());

        if (!injectMessage_) {
            SendError(res, 500, "debug message injection is not wired up");
            return;
        }
        const json result = injectMessage_(chatId, content, senderId);
        if (result.contains("error")) {
            SendError(res, 400, result["error"].get<std::string>());
            return;
        }
        SendJson(res, result);
    });

    server_->listen("127.0.0.1", port);
}

void AdminServer::Stop() {
    if (server_) {
        server_->stop();
    }
}
