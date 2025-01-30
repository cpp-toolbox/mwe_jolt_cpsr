#include <iostream>
#include "networking/server_networking/network.hpp"
#include "utility/fixed_frequency_loop/fixed_frequency_loop.hpp"
#include "utility/fixed_frequency_processor/fixed_frequency_processor.hpp"
#include "utility/transform/transform.hpp"
#include <format>
#include <string>

struct GameUpdate {
    double position_x;
    double position_y;
    double velocity_x;
    double velocity_y;

    int last_id_used_to_produce_this_update;

    // Overloading the << operator
    friend std::ostream &operator<<(std::ostream &os, const GameUpdate &update) {
        os << "GameUpdate { position: " << update.position_x << ", " << update.position_y
           << ", last_id_used_to_produce_this_update: " << update.last_id_used_to_produce_this_update << " }";
        return os;
    }
};

struct KeyboardUpdate {
    int id;
    bool forward_pressed = false;
    bool backwards_pressed = false;
    bool left_pressed = false;
    bool right_pressed = false;

    friend std::ostream &operator<<(std::ostream &os, const KeyboardUpdate &update) {
        os << "KeyboardUpdate{id: " << update.id << ", forward_pressed: " << update.forward_pressed
           << ", backwards_pressed: " << update.backwards_pressed << ", left_pressed: " << update.left_pressed
           << ", right_pressed: " << update.right_pressed << "}";
        return os;
    }
};

constexpr bool printing_active = true;
void p(std::string s) {
    if (printing_active) {
        std::cout << s << std::endl;
    }
}

int main() {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::debug);
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("network_logs.txt", true);
    file_sink->set_level(spdlog::level::info);

    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};
    Network network(7777, sinks);
    network.initialize_network();

    int test_client_id = -1;

    std::function<void(unsigned int)> on_client_connect = [&](unsigned int client_id) {
        /*physics.create_character(client_id);*/
        /*connected_character_data[client_id] = {client_id};*/
        spdlog::info("just registered a client with id {}", client_id);
        // TODO no need to broadcast this to everyone, only used once on the connecting client
        /*network.reliable_send(client_id, &client_id, sizeof(unsigned int));*/
        test_client_id = client_id;
    };

    network.set_on_connect_callback(on_client_connect);

    std::unordered_map<int, KeyboardUpdate> id_to_keyboard_update;

    Transform transform;
    float acceleration = 10 * 0.01;
    float friction = 0.99;
    glm::vec2 current_velocity(0);

    std::function<void(int, double)> physics_tick = [&](int id, double dt) {
        // Retrieve the latest keyboard state for this id
        auto it = id_to_keyboard_update.find(id);
        if (it == id_to_keyboard_update.end()) {
            p(std::format("No input for id: {}", id));
            return;
        }

        const KeyboardUpdate &input = it->second;

        glm::vec2 input_vector(static_cast<int>(input.right_pressed) - static_cast<int>(input.left_pressed),
                               static_cast<int>(input.forward_pressed) - static_cast<int>(input.backwards_pressed));

        p(std::format("Input Vector: ({}, {})", input_vector.x, input_vector.y));

        current_velocity += input_vector * acceleration * static_cast<float>(dt);
        current_velocity *= friction;

        glm::vec3 xy_velocity(current_velocity, 0);
        transform.position += xy_velocity * static_cast<float>(dt);

        p(std::format("processing id: {} with dt: {} new position: {}", id, dt, transform.get_string_repr()));
    };

    FixedFrequencyProcessor physics_engine(60, physics_tick);
    PeriodicSignal send_signal(60);

    std::function<void(double)> tick = [&](double dt) {
        std::vector<PacketWithSize> received_packets = network.get_network_events_since_last_tick();
        for (const auto &packet : received_packets) {
            const KeyboardUpdate *received_keyboard_update =
                reinterpret_cast<const KeyboardUpdate *>(packet.data.data());

            p(std::format("keyboard update just received: {}", received_keyboard_update->id));
            physics_engine.add_id(received_keyboard_update->id);
            id_to_keyboard_update[received_keyboard_update->id] = *received_keyboard_update;
        }

        if (physics_engine.attempt_to_process()) {
            p("^^^ just processed ^^^");
        }

        if (test_client_id != -1) {
            if (physics_engine.processed_at_least_one_id and send_signal.process_and_get_signal()) {
                GameUpdate gu(transform.position.x, transform.position.y, current_velocity.x, current_velocity.y,
                              physics_engine.get_last_processed_id());
                std::cout << "sending game update: " << gu << std::endl;
                network.unreliable_send(test_client_id, &gu, sizeof(GameUpdate));
            }
        }
    };
    std::function<bool()> termination = [&]() { return false; };
    FixedFrequencyLoop ffl;
    ffl.start(512, tick, termination);
}
