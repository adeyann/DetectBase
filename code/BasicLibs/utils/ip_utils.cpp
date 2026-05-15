#include "ip_utils.h"

#include <iostream>
#include <string>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <regex>

namespace MGEN
{
    std::string getPhysicalIPAddress( void )
    {
        struct ifaddrs *ifaddr, *ifa;
        char ip[INET_ADDRSTRLEN];

        // F-I3-07: regex 1회 컴파일 (호출마다 재컴파일 비용 회피)
        static const std::regex iface_pattern( "^(eth|eno|ens|enp|wlan)[0-9a-zA-Z]*$" );

        if( getifaddrs(&ifaddr) == -1 ){
            perror("getifaddrs");
            return "";
        }

        for( ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next ){
            if( ifa->ifa_addr == nullptr )
                continue;

            if( ifa->ifa_addr->sa_family == AF_INET ){

                std::string ifname(ifa->ifa_name);

                // 가상 인터페이스 제외
                if( ifname == "lo" ||
                    ifname.find("docker") != std::string::npos ||
                    ifname.find("br-") != std::string::npos ||
                    ifname.find("veth") != std::string::npos )
                    continue;

                // 물리 네트워크 인터페이스 이름인지 확인
                if( !std::regex_match(ifname, iface_pattern) )
                    continue;

                void *addr_ptr = &reinterpret_cast<struct sockaddr_in*>( ifa->ifa_addr )->sin_addr;
                inet_ntop(AF_INET, addr_ptr, ip, INET_ADDRSTRLEN);

                freeifaddrs(ifaddr);
                return std::string(ip);
            }
        }

        freeifaddrs(ifaddr);
        return "";
    }

} // namespace MGEN
