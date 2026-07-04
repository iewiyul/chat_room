#include "message.h"

namespace chat::server{
    nlohmann::json Message::toJson() const{
        nlohmann::json j;
        j["from"]    = from;
        j["room"]    = room;
        j["content"] = content;
        j["time"]    = time;
        return j;
    }
}