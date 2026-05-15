#ifndef SIO_PARSER_H
#define SIO_PARSER_H

#include "sio_client.h"
#include "json/json_fwd.hpp"
#include <iostream>
#include <string>

using namespace nlohmann;
//using namespace sio;
using namespace std;

sio::message::ptr createObject(const nlohmann::json& o);
sio::message::ptr createArray(const nlohmann::json& o);
nlohmann::json createJson(sio::message::ptr sio);

#endif