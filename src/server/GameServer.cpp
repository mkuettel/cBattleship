#include "GameServer.h"
#include <boost/log/trivial.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/thread.hpp> 
#include <algorithm>
#include "GameServerUtil.h"

GameServer::GameServer()
    : state_machine(CHECK_FOR_CONNECTIONS, *this), server(boost::bind(&GameServer::handle_connection, this, _1)) {
    BOOST_LOG_TRIVIAL(info) << "cbattleship-server listening on 0.0.0.0:13477 ...";
}

GameServer::~GameServer() {
}

GameServer::StateMachineType::StateMap GameServer::get_state_map() {
    StateMachineType::StateMap map;
    map[CHECK_FOR_CONNECTIONS] =  &GameServer::check_for_connections;
    map[SETUP_GAME] = &GameServer::setup_game;
    map[TURN_WAIT] = &GameServer::turn_wait;
    return map;
}

PlayerNetworkPackage GameServer::get_input() {
    while(true) {
        auto read_something = [this]() -> bool {
            std::lock_guard<std::mutex> lock(queue_lock);
            return !input_queue.empty();
        };
        if(read_something()) {
            std::lock_guard<std::mutex> lock(queue_lock);
            PlayerNetworkPackage player_command = input_queue.front();
            input_queue.pop();
            return player_command;
        } else {
            boost::this_thread::sleep(boost::posix_time::milliseconds(5));
        }
    }
}

void GameServer::handle_connection(Connection *connection) {
    if(can_handle_new_connection()) {
        register_new_connection(connection);
    } else {
        connection->disconnect();
    }
}

void GameServer::register_new_connection(Connection *connection) {
    Player *player = new Player(connection);
    players[connection->get_id()] = std::unique_ptr<Player>(new Player(connection));
    handle_player_connection(*player);
}

void GameServer::handle_player_connection(Player &player) {
    auto & connection = player.get_connection();
    connection.read([this, &player, &connection](NetworkPackage& command) {
        BOOST_LOG_TRIVIAL(debug) << "add command #" << (int)command.get_package_nr() << " from '" << player.get_name() << "' to input queue";
        if(is_authenticated(command, player)) {
            std::lock_guard<std::mutex> lock(queue_lock);
            PlayerNetworkPackage pcmd(command, player);
            input_queue.push(pcmd);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "dropping, command not properly authenticated";
        }
        handle_player_connection(player);
    });
}

bool GameServer::is_authenticated(NetworkPackage & command, Player & player) {
    AuthenticatedNetworkPackage* authenticated_package = dynamic_cast<AuthenticatedNetworkPackage*>(&command);
    if(authenticated_package == nullptr) return true;
    return authenticated_package->get_identity() == player.get_identity();
}

bool GameServer::can_handle_new_connection() {
    return players.size() < 2;
}

void GameServer::run() {
    while(!state_machine.has_terminated()) {
        auto input = get_input();
        state_machine.run_state(input);
    }
}

void GameServer::next_player() {
    if(current_player != players_playing.end()) {
        current_player++;
    }
    if(current_player == players_playing.end()) {
        current_player = players_playing.begin();
    }
}

Player& GameServer::get_enemy() {
    auto current = current_player;
    next_player();
    auto enemy = current_player;
    current_player = current;
    return **enemy;
}

void GameServer::request_turn(bool enemy_hit, position_t position) {
    next_player();
    BOOST_LOG_TRIVIAL(info) << "requesting turn from " << (*current_player)->get_name();
    TurnRequestPackage turn_request_package;
    turn_request_package.set_enemy_hit(enemy_hit);
    turn_request_package.set_position(position);
    (*current_player)->get_connection().write(turn_request_package);
}


GameServerState GameServer::check_for_connections(PlayerNetworkPackage player_package) {
    BOOST_LOG_TRIVIAL(debug) << "entering state " << __FUNCTION__;
    Player& player = player_package.get_player();
    NetworkPackage& package = player_package.get_package();

    if(is_package_of_type<PlayerJoinPackage>(package)) {
        PlayerJoinPackage & p = cast_package<PlayerJoinPackage>(package);
        BOOST_LOG_TRIVIAL(info) << "Player '" << p.get_player_name() << "' joined the game";
        player.set_name(p.get_player_name());

        std::string identity = GameServerUtil::generate_identity();
        player.set_identity(identity);
        players_playing.push_back(&player);

        PlayerJoinAnswerPackage answer;
        answer.set_identity(identity);
        player.get_connection().write(answer);
    }

    if(players_playing.size() == 2) {
        auto last_player = players_playing.end();
        last_player--;
        current_player = last_player;
        auto player_before = last_player;
        do {
            player_before = current_player;
            next_player();
            GameReadyPackage package;
            package.set_enemy_name((*player_before)->get_name());
            (*current_player)->get_connection().write(package);
        } while((*current_player)->get_identity() != (*last_player)->get_identity());
        return SETUP_GAME;
    } else {
        return CHECK_FOR_CONNECTIONS;
    }
}

GameServerState GameServer::setup_game(PlayerNetworkPackage player_package) {
    BOOST_LOG_TRIVIAL(debug) << "entering state " << __FUNCTION__;
    Player& player = player_package.get_player();
    NetworkPackage& package = player_package.get_package();

    if(is_package_of_type<ShipPlacementPackage>(package)) {
        auto &ship_placement_package = cast_package<ShipPlacementPackage>(package);
        auto ship_data = ship_placement_package.get_ship_data();
        ShipPlacementResponsePackage response;
        try {
            player.get_battle_field().clear();
            std::for_each(ship_data.begin(), ship_data.end(), [&player](ShipData &ship) {
                BOOST_LOG_TRIVIAL(debug) << player.get_name() << ": place ship(" << ship.length << ") or: " << ship.orientation << ", y: " << ship.start_position.y << ", x: " << ship.start_position.x;
                player.get_battle_field().add_ship(ship.length, ship.orientation, ship.start_position);
            });
            response.set_out_of_bounds(false);
            response.set_ships_overlap(false);
            if(player.get_battle_field().all_ships_placed()) {
                response.set_valid(true);
                response.set_remaining_ships(0);
                player.set_ready_to_start(true);
            } else {
                BOOST_LOG_TRIVIAL(warning) << player.get_name() << ": not all ships placed, retrying ... ";
                response.set_valid(false);
                response.set_remaining_ships(1);
            }
        } catch(std::out_of_range &ex) {
            BOOST_LOG_TRIVIAL(warning) << player.get_name() << ": " << ex.what() << ", retrying ... ";
            response.set_valid(false);
            response.set_out_of_bounds(true);
            response.set_ships_overlap(false);
            response.set_remaining_ships(0);
        } catch(std::invalid_argument &ex) {
            BOOST_LOG_TRIVIAL(warning) << player.get_name() << ": " << ex.what() << ", retrying ... ";
            response.set_valid(false);
            response.set_out_of_bounds(false);
            if(player.get_battle_field().all_ships_placed()) {
                response.set_ships_overlap(true);
            } else {
                response.set_remaining_ships(-1);
            }
        }
        player.get_connection().write(response);
    }

    if(GameServerUtil::players_ready_to_start(players_playing)) {
        request_turn(false, position());
        return TURN_WAIT;
    }
    return SETUP_GAME;
}

GameServerState GameServer::turn_wait(PlayerNetworkPackage player_package) {
    BOOST_LOG_TRIVIAL(debug) << "entering state " << __FUNCTION__;
    Player& player = player_package.get_player();
    NetworkPackage& package = player_package.get_package();

    if(is_package_of_type<TurnPackage>(package)) {
        if(&player == *current_player) {
            TurnPackage & p = cast_package<TurnPackage>(package);
            position_t position = p.get_position();
            try {
                auto field = get_enemy().get_battle_field().get_field(position);
                field->set_hit();

                // everything ok with this turn, send feedback
                TurnResponsePackage turn_response;
                turn_response.set_valid(true);
                turn_response.set_ship_hit(field->is_ship_part());
                int ship_of_length_destroyed = 0;
                auto ship_hit = get_enemy().get_battle_field().get_ship_at_position(position);
                if(ship_hit.get() != nullptr && ship_hit->is_destroyed()) {
                    turn_response.set_ship_of_length_destroyed(ship_hit->get_length());
                }
                (*current_player)->get_connection().write(turn_response);

                // does a player not have any ships anymore?
                if(std::any_of(players_playing.begin(), players_playing.end(),
                    [](Player *player) { return player->get_battle_field().all_ships_destroyed(); })) {
                    // well then, finish up the game
                    auto current = current_player;
                    do {
                        next_player();
                        GameEndedPackage game_ended_package;
                        game_ended_package.set_won(!(*current_player)->get_battle_field().all_ships_destroyed());
                        game_ended_package.set_enemy_ships(get_enemy().get_battle_field().get_ship_data());
                        (*current_player)->get_connection().write(game_ended_package);
                    } while(current != current_player);
                    players_playing.clear();
                    players.clear();
                    return CHECK_FOR_CONNECTIONS;
                }

                // request next turn from other player
                request_turn(true, position);
            } catch(std::out_of_range &ex) {
                BOOST_LOG_TRIVIAL(info) << "player sent out of range position, asking again...";
                TurnResponsePackage turn_response;
                turn_response.set_valid(false);
                turn_response.set_ship_hit(false);
                (*current_player)->get_connection().write(turn_response);
            }
        }
    }
    return TURN_WAIT;
}
