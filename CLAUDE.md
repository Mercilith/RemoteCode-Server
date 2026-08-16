# Working Instructions for Claude

## Commit discipline

This repo is set up so you (Claude) should commit frequently as you work — not just at the end of a session.

**Commit at every natural stopping point**, including:
- After completing a discrete feature, function, or fix
- After a test suite passes following changes
- Before switching to a different file, module, or task
- Before any risky or exploratory change (so it's easy to roll back)
- Any time you're about to context-switch or the user pauses/steps away
- Before ending a session, even if the work is incomplete

**Delegate every commit to the `commit-writer` subagent.** Don't write the commit message or run `git commit` yourself in the main conversation — invoke the `commit-writer` subagent instead, e.g. "Use the commit-writer subagent to commit these changes." That subagent is pinned to a fixed model and effort level (see `.claude/agents/commit-writer.md`), so committing costs the same regardless of which model is doing the actual coding in this session. This applies every time you commit, not just at the end.

**Do not wait for explicit permission to commit.** Committing is a safe, reversible action — treat it as part of normal workflow, not something to ask about each time. Delegate to commit-writer as soon as you hit a stopping point above. Only pause before force-pushing, rewriting history, or pushing to a shared/protected branch.

## Branch

The default branch is `main`. Work directly on `main` unless the user asks for a feature branch.
