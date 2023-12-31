#include "PlayerJoinPackage.h"


PlayerJoinPackage::PlayerJoinPackage() {
}

PlayerJoinPackage::~PlayerJoinPackage() {
}

package_nr_t PlayerJoinPackage::get_package_nr() {
    return 1;
}

std::vector<unsigned char> PlayerJoinPackage::encode_payload() {
    return std::vector<unsigned char>(player_name.begin(), player_name.end());
}

void PlayerJoinPackage::decode_payload(std::vector<unsigned char> package_data) {
    player_name = std::string(package_data.begin(), package_data.end());
}

void PlayerJoinPackage::set_player_name(std::string player_name) {
    this->player_name = player_name;
}

std::string PlayerJoinPackage::get_player_name() {
    return player_name;
}
