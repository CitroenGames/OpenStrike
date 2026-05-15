#include "openstrike/server/server_module.hpp"

#include "openstrike/core/log.hpp"

namespace openstrike
{
const char* ServerModule::name() const
{
    return "server";
}

void ServerModule::on_start(const RuntimeConfig&, EngineContext&)
{
    log_info("dedicated server module started");
}

void ServerModule::on_fixed_update(const SimulationStep&, EngineContext&)
{
    ++simulated_ticks_;
}
}
