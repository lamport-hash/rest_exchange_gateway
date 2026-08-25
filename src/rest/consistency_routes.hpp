#pragma once

#include <crow_all.h>
#include <nlohmann/json.hpp>

#include <mutex>
#include <utility>

namespace gateway::rest {

/// Shared snapshot of the latest consistency audit, written by the
/// gateway's audit scheduler thread and served by GET /consistency.
struct ConsistencyCache
{
    mutable std::mutex mutex;
    nlohmann::json last = nlohmann::json{{"status", "not_run_yet"}};

    void store(nlohmann::json a_snapshot)
    {
        const std::lock_guard lock(mutex);
        last = std::move(a_snapshot);
    }

    [[nodiscard]] auto get() const -> nlohmann::json
    {
        const std::lock_guard lock(mutex);
        return last;
    }
};

/// Register GET /consistency on a_app: the most recent audit result
/// (alerts included). Alert-only by design — no healing endpoint exists.
void register_consistency_routes(crow::SimpleApp& a_app, const ConsistencyCache& a_cache);

} // namespace gateway::rest
