#include "openstrike/server/server_module.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/game/team_system.hpp"
#include "openstrike/network/network_session.hpp"

namespace openstrike
{
namespace
{
void log_network_event(const NetworkEvent& event)
{
    switch (event.type)
    {
    case NetworkEventType::ServerStarted:
        log_info("network server started on {}", event.remote.to_string());
        break;
    case NetworkEventType::ServerStopped:
        log_info("network server stopped");
        break;
    case NetworkEventType::ClientConnected:
        log_info("network client {} connected id={}", event.remote.to_string(), event.connection_id);
        break;
    case NetworkEventType::ClientDisconnected:
        log_info("network client {} disconnected id={}", event.remote.to_string(), event.connection_id);
        break;
    case NetworkEventType::ConnectedToServer:
        log_info("connected to server {} id={}", event.remote.to_string(), event.connection_id);
        break;
    case NetworkEventType::DisconnectedFromServer:
        log_info("disconnected from server {}", event.remote.to_string());
        break;
    case NetworkEventType::TextReceived:
        log_info("network text from {}: {}", event.remote.to_string(), event.text);
        break;
    case NetworkEventType::UserCommandReceived:
        log_info("network user command from {} tick={} bytes={}", event.remote.to_string(), event.tick, event.payload.size());
        break;
    case NetworkEventType::SnapshotReceived:
        log_info("network snapshot from {} tick={} bytes={}", event.remote.to_string(), event.tick, event.payload.size());
        break;
    case NetworkEventType::PacketDropped:
        log_warning("dropped network packet from {}", event.remote.to_string());
        break;
    case NetworkEventType::SocketError:
        log_warning("network socket error for {}: {}", event.remote.to_string(), event.text);
        break;
    }
}
}

const char* ServerModule::name() const
{
    return "server";
}

void ServerModule::on_start(const RuntimeConfig& config, EngineContext& engine)
{
    log_info("dedicated server module started");
    if (engine.network.start_server(config.network_port))
    {
        log_info("dedicated server listening on UDP port {}", engine.network.server().local_port());
    }
}

void ServerModule::on_fixed_update(const SimulationStep&, EngineContext&)
{
    ++simulated_ticks_;
}

void ServerModule::on_frame(const FrameContext& context, EngineContext& engine)
{
    engine.network.poll(context.tick_index);
    for (const NetworkEvent& event : engine.network.drain_events())
    {
        handle_team_network_event(engine, event);
        log_network_event(event);
    }
}

void ServerModule::on_stop(EngineContext& engine)
{
    engine.network.stop_server();
}
}
