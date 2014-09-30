#include "NetworkInterface.h"
#include "PlayerJoinCommand.h"

#define PACKAGE_TERMINATOR 0xCD

NetworkInterface::NetworkInterface() {
    add_network_command(new PlayerJoinCommand());
}

std::vector<unsigned char> NetworkInterface::encode_command(NetworkCommand command) {
    std::vector<unsigned char> encoded(3);
    encoded[0] = command.get_command_nr();

    auto payload = command.encode_payload();
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    encoded.push_back(PACKAGE_TERMINATOR);

    unsigned short size = encoded.size();
    encoded[1] = size & 0xFF;
    encoded[2] = (size >> 8) & 0xFF;

    return encoded;
}

std::shared_ptr<NetworkCommand> NetworkInterface::decode_command(std::vector<unsigned char> command_data) {
    unsigned short size = 0;
    size |= command_data[1];
    size |= command_data[2] << 8;

    std::shared_ptr<NetworkCommand> command = network_commands[command_data[0]];
    if(command == nullptr) throw std::runtime_error("invalid command nr");
    if(command_data.size() == size) throw std::runtime_error("invalid package size");
    if(command_data[size - 1] == PACKAGE_TERMINATOR) throw std::runtime_error("invalid package terminator");

    // remove terminator & header
    command_data.erase(command_data.begin() + (size - 1));
    command_data.erase(command_data.begin(), command_data.begin() + 3);

    command->decode_payload(command_data);

    return command;
}

void NetworkInterface::add_network_command(NetworkCommand *command) {
    network_commands[command->get_command_nr()] = std::shared_ptr<NetworkCommand>(command);
}