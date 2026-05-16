#include "openstrike/client/client_module.hpp"

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

const char* ClientModule::name() const
{
    return "client";
}

void ClientModule::on_start(const RuntimeConfig& config, EngineContext& engine)
{
    log_info("client module started with content root '{}'", config.content_root.string());
    log_info("GAME search path: {}", engine.filesystem.search_path_string("GAME"));
}

void ClientModule::on_frame(const FrameContext& context, EngineContext& engine)
{
    if (context.frame_index == 0)
    {
        log_info("client first frame tick={} alpha={}", context.tick_index, context.interpolation_alpha);
    }

    engine.network.poll(context.tick_index);
    engine.variables.set("net_status", std::string(to_string(engine.network.client().state())));
    for (const NetworkEvent& event : engine.network.drain_events())
    {
        handle_team_network_event(engine, event);
        log_network_event(event);
    }
}

void ClientModule::on_stop(EngineContext& engine)
{
    engine.network.disconnect_client(0);
    engine.network.stop_server();
}
}
