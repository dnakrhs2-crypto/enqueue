#pragma once

#include <array>

namespace gocue
{

enum class ReopenLastProjectPolicy { ask, always, never };
inline constexpr std::array<ReopenLastProjectPolicy, 3> reopenLastProjectPolicies {
    ReopenLastProjectPolicy::ask, ReopenLastProjectPolicy::always, ReopenLastProjectPolicy::never
};

struct ReopenLastProjectContext
{
    bool openedFromCommandLine = false;
    bool reopenedAfterUpdate = false;
    bool safeMode = false;
    bool hasLastSessionProject = false;
    bool lastSessionProjectExists = false;
    bool hasOpenProject = false;
};

struct ReopenLastProjectDecision
{
    enum class Action { none, prompt, open };
    Action action = Action::none;
    bool clearLastSessionProject = false;
};

/** Pure startup decision: the caller supplies file existence and applies the result. */
constexpr ReopenLastProjectDecision decideReopenLastProject (ReopenLastProjectPolicy policy,
                                                            ReopenLastProjectContext context) noexcept
{
    ReopenLastProjectDecision result;
    result.clearLastSessionProject = context.hasLastSessionProject && ! context.lastSessionProjectExists;

    if (context.openedFromCommandLine || context.reopenedAfterUpdate || context.safeMode
        || policy == ReopenLastProjectPolicy::never || ! context.hasLastSessionProject
        || ! context.lastSessionProjectExists || context.hasOpenProject)
        return result;

    result.action = policy == ReopenLastProjectPolicy::always ? ReopenLastProjectDecision::Action::open
                                                            : ReopenLastProjectDecision::Action::prompt;
    return result;
}

} // namespace gocue
