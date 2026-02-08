#include "ConsoleModule.h"
#include "MeshService.h"
#include "MeshModule.h"
#include "Telemetry/EnvironmentTelemetry.h"
#include "NodeDB.h"
#include "configuration.h"
#include "graphics/Screen.h"

ConsoleModule *consoleModule;


void ConsoleModule::sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    p->to = dest;
    p->channel = channel;
    p->want_ack = true;
    p->decoded.payload.size = strlen(message);
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);

    LOG_INFO("Send message id=%d, dest=%x, msg=%.*s", p->id, p->to, p->decoded.payload.size, p->decoded.payload.bytes);

    service->sendToMesh(
        p, RX_SRC_LOCAL,
        true); // send to mesh, cc to phone. Even if there's no phone connected, this stores the message to match ACKs
}


ProcessMessage ConsoleModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    LOG_INFO("Console module: text msg from=0x%0x, id=0x%x, channel=%d,  msg=%.*s", mp.from, mp.id, mp.channel, p.payload.size, p.payload.bytes);
#endif
    // se il messaggio proviene da una delle chiavi autorizzate,
    // si prosegue

    const bool authorized =
        (config.security.admin_key[0].size == 32 &&
         memcmp(mp.public_key.bytes, config.security.admin_key[0].bytes, 32) == 0) ||
        (config.security.admin_key[1].size == 32 &&
         memcmp(mp.public_key.bytes, config.security.admin_key[1].bytes, 32) == 0) ||
        (config.security.admin_key[2].size == 32 &&
         memcmp(mp.public_key.bytes, config.security.admin_key[2].bytes, 32) == 0);

    if (!authorized) {
        LOG_INFO("message from no-authorized keys");
        return ProcessMessage::CONTINUE;
    }
    LOG_INFO("message from authorized keys");

    if (strncmp(reinterpret_cast<const char *>(p.payload.bytes),"+++",3)==0) {
        command_state = !command_state;
    }
    if (!command_state) return ProcessMessage::CONTINUE;

    if (p.payload.size>0 and p.payload.bytes[0]=='H') {
        this->sendText(mp.from,0, "Hello from firmware!", false);
    } else if (p.payload.size>0 and p.payload.bytes[0]=='N') {
        LOG_INFO("Nodes");
        // 0 = ourself
        uint16_t max_nodes=4;
        std::string route = "Nodes:\n";
        for (size_t i = 1; i < nodeDatabase.nodes.size(); i++) {
            const auto &entry = nodeDatabase.nodes[i];

            if (entry.hops_away == 0) {
                route += vformat("%s: '%s' snr: %f\n", entry.user.short_name, entry.user.long_name, entry.snr);
                if (! --max_nodes) break;
            }
        }
        this->sendText(mp.from,0, route.c_str(), false);
    } else if (p.payload.size>0 and p.payload.bytes[0]=='T') {
        // telemetry
        meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
        m.which_variant = meshtastic_Telemetry_environment_metrics_tag;
        // c'è il modulo ?

        MeshModule *mesh_module;
        mesh_module = MeshModule::getModule("EnvironmentTelemetry");
        if (mesh_module == nullptr) {
            this->sendText(mp.from,0, "telemetry not enabled!", false);
            return ProcessMessage::CONTINUE;
        }
        EnvironmentTelemetryModule * environment_telemetry_module;
        environment_telemetry_module = (EnvironmentTelemetryModule *) mesh_module;
        if (environment_telemetry_module->extGetEnvironmentTelemetry(&m)) {
            std::string msg = "Telemetry:\n";
            if (m.variant.environment_metrics.has_temperature) {
                msg+=vformat("T : %f\n", m.variant.environment_metrics.temperature);
            }
            if (m.variant.environment_metrics.has_relative_humidity) {
                msg+=vformat("RH: %f\n", m.variant.environment_metrics.relative_humidity);
            }
            if (m.variant.environment_metrics.has_barometric_pressure) {
                msg+=vformat("RH: %f\n", m.variant.environment_metrics.barometric_pressure);
            }
            this->sendText(mp.from,0, msg.c_str(), false);
        }
    } else if (p.payload.bytes[0]=='C') {
        // nodi preferiti C? = lista, C+<id>, aggiunge, C-<id> toglie
        LOG_INFO("Favorites");
        if (p.payload.size==1) {
            // only C = list
            uint16_t max_nodes=4;
            std::string msg = "Favorites:\n";
            for (size_t i = 1; i < nodeDatabase.nodes.size(); i++) {
                const auto &entry = nodeDatabase.nodes[i];
                if (entry.is_favorite) {
                    msg += vformat("!%08x: '%s'\n", entry.num, entry.user.short_name);
                }
            }
            this->sendText(mp.from,0, msg.c_str(), false);
        } else if (p.payload.size==10) {
            // C+<hex id>
            // parse hex id
            auto new_favorite_id =  static_cast<uint32_t>(strtoul(reinterpret_cast<const char *>(p.payload.bytes+2), nullptr, 16));
            std::string msg;
            if (p.payload.bytes[1]=='+') {
                LOG_INFO("Setting %d as favorite", new_favorite_id);
                msg=vformat("%d set as favorite",new_favorite_id);
                nodeDB->set_favorite(true, new_favorite_id);
                this->sendText(mp.from,0, msg.c_str(), false);
            }
            if (p.payload.bytes[1]=='-') {
                LOG_INFO("Unsetting %d as favorite", new_favorite_id);
                msg=vformat("%d unset as favorite",new_favorite_id);
                nodeDB->set_favorite(false, new_favorite_id);
                this->sendText(mp.from,0, msg.c_str(), false);
            }

        }
    }

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

bool ConsoleModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}
