// Loopback-only TURN fixture. Never exposed as a deployment service.
#include <juice/juice.h>
#include <iostream>
#include <string>
int main() {
    juice_server_credentials_t credentials{"haze-test","local-test-only",32};
    juice_server_config_t config{};
    config.credentials=&credentials; config.credentials_count=1;
    config.max_allocations=32; config.max_peers=32;
    config.bind_address="127.0.0.1"; config.external_address="127.0.0.1";
    config.port=3479; config.realm="haze-local-test";
    auto server=juice_server_create(&config);
    if(!server) return 1;
    std::cout << "TURN fixture ready " << juice_server_get_port(server) << std::endl;
    std::string line; std::getline(std::cin,line);
    juice_server_destroy(server);
}
