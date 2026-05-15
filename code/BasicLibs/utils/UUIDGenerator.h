#ifndef _MGEN_UUID_GENERATOR_H_
#define _MGEN_UUID_GENERATOR_H_

#include <string>
#include <sstream>
#include <random>
#include <iomanip>
#include <memory>
#include <thread>
#include <regex>

namespace MGEN
{
    using UUIDType = std::string;
    class UUID final
    {
    private:
        UUID() = default;  // Singleton 방식

        uint32_t gen32()
        {
            return dist32(getRNG());
        }

        uint16_t gen16()
        {
            return dist16(getRNG());
        }

        // 스레드마다 독립적인 RNG를 생성
        std::mt19937& getRNG()
        {
            thread_local static std::mt19937 rng{ std::random_device{}() };
            return rng;
        }

        std::uniform_int_distribution<uint32_t> dist32{ 0, 0xFFFFFFFF };
        std::uniform_int_distribution<uint16_t> dist16{ 0, 0xFFFF };

    public:
        static std::shared_ptr<UUID> GetGenerator()
        {
            static std::shared_ptr<UUID> instance{ new UUID() };
            return instance;
        }

        static std::string Empty( void )
        {
            return std::string { "" };
        }

        std::string generate()
        {
            std::stringstream ss;

            uint32_t a = gen32();
            uint16_t b = gen16();
            uint16_t c = (gen16() & 0x0FFF) | 0x4000; // version 4
            uint16_t d = (gen16() & 0x3FFF) | 0x8000; // variant 1
            uint16_t e = gen16();
            uint32_t f = gen32();

            ss << std::hex << std::setfill('0')
               << std::setw(8) << a << "-"
               << std::setw(4) << b << "-"
               << std::setw(4) << c << "-"
               << std::setw(4) << d << "-"
               << std::setw(4) << e
               << std::setw(8) << f;

            return ss.str();
        }

        static bool isValidUUID(const std::string& uuid)
        {
            static const std::regex uuid_regex(
                R"([a-fA-F0-9]{8}-([a-fA-F0-9]{4}-){3}[a-fA-F0-9]{12})",
                std::regex::optimize
            );

            return std::regex_match(uuid, uuid_regex);
        }
    };

}

#endif
