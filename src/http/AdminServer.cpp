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
    AgentStore& agentStore, AgentSessionStore& agentSessionStore, std::wstring dbPath,
    std::string claudeConfigDir, std::wstring logDir)
    : agentStore_(agentStore),
      agentSessionStore_(agentSessionStore),
      dbPath_(std::move(dbPath)),
      claudeConfigDir_(std::move(claudeConfigDir)),
      logDir_(std::move(logDir)) {}

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
        agentSessionStore_.Get(alex.id, reviseChatId, resumeSessionId);

        const AgentTurnResult result = AgentTurn::Run(
            alex, {contextMessage}, dbPath_, claudeConfigDir_, reviseChatId, resumeSessionId, logDir_);
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

    server_->listen("127.0.0.1", port);
}

void AdminServer::Stop() {
    if (server_) {
        server_->stop();
    }
}
