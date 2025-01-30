#include "graphics/shader_cache/shader_cache.hpp"
#include "graphics/vertex_geometry/vertex_geometry.hpp"
#include "graphics/window/window.hpp"
#include "graphics/transform/transform.hpp"
#include "graphics/batcher/generated/batcher.hpp"
#include "graphics/glfw_lambda_callback_manager/glfw_lambda_callback_manager.hpp"

#include "networking/client_networking/network.hpp"

#include <GLFW/glfw3.h>
#include <format>
#include <string>

#include "utility/fixed_frequency_loop/fixed_frequency_loop.hpp"
#include "utility/fixed_frequency_reprocessor/fixed_frequency_reprocessor.hpp"
#include "utility/periodic_signal/periodic_signal.hpp"
#include "utility/input_state/input_state.hpp"
#include "utility/rate_limited_function/rate_limited_function.hpp"

//
#include <GLFW/glfw3.h>
#include <iostream>

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

    unsigned int screen_width_px = 800;
    unsigned int screen_height_px = 800;
    bool start_in_fullscreen = false;
    bool start_with_mouse_captured = false;
    bool vsync = false;

    Transform client_only_transform;
    Transform server_only_transform;

    Transform transform;
    float acceleration = 10 * 0.01;
    float friction = 0.99;
    glm::vec2 current_velocity(0);

    std::unordered_map<int, glm::vec2> id_to_velocity;
    std::unordered_map<int, glm::vec2> id_to_position;
    std::unordered_map<int, KeyboardUpdate> id_to_keyboard_update;

    Window window;
    window.initialize_glfw_glad_and_return_window(screen_width_px, screen_height_px, "mwe_cpsr", start_in_fullscreen,
                                                  start_with_mouse_captured, vsync);

    InputState input_state;

    // TODO debugging why keys aren't being picked up for some reason
    std::function<void(unsigned int)> char_callback = [](unsigned int codepoint) {};
    std::function<void(int, int, int, int)> key_callback = [&](int key, int scancode, int action, int mods) {
        if (action == GLFW_PRESS || action == GLFW_RELEASE) {
            Key &active_key = *input_state.glfw_code_to_key.at(key);
            bool is_pressed = (action == GLFW_PRESS);
            active_key.pressed_signal.set_signal(is_pressed);
        }
    };
    std::function<void(double, double)> mouse_pos_callback = [](double xpos, double ypos) {};
    std::function<void(int, int, int)> mouse_button_callback = [](int button, int action, int mods) {};
    GLFWLambdaCallbackManager glcm(window.glfw_window, char_callback, key_callback, mouse_pos_callback,
                                   mouse_button_callback);

    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    std::vector<ShaderType> requested_shaders = {ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR};
    ShaderCache shader_cache(requested_shaders);
    Batcher batcher(shader_cache);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::debug);
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("network_logs.txt", true);
    file_sink->set_level(spdlog::level::info);

    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};
    std::string local_network = "localhost";
    std::string ubuntu_sfo = "147.182.197.23";
    Network network(ubuntu_sfo, 7777, sinks);
    network.initialize_network();
    network.attempt_to_connect_to_server();

    double velocity = 1;

    auto client_process = [&](int id, double dt, bool reprocessing_call) {
        KeyboardUpdate ku = id_to_keyboard_update[id];
        glm::vec2 input_vector(static_cast<int>(ku.right_pressed) - static_cast<int>(ku.left_pressed),
                               static_cast<int>(ku.forward_pressed) - static_cast<int>(ku.backwards_pressed));
        std::string result = std::format("Input Vector: ({}, {})", input_vector.x, input_vector.y);
        p(result);
        current_velocity += input_vector * acceleration * static_cast<float>(dt);
        current_velocity *= friction;
        p(std::format("Before processing: Client ID: {} with dt: {} - Position: ({}, {})", id, dt, transform.position.x,
                      transform.position.y));
        glm::vec3 xy_velocity(current_velocity, 0);
        transform.position += xy_velocity * (float)dt;

        if (not reprocessing_call) {
            client_only_transform.position += xy_velocity * (float)dt;
        }

        id_to_position[id] = transform.position;

        result = std::format("Client Processing ID: {} with dt: {} - New Position: ({}, {})", id, dt,
                             id_to_position[id].x, id_to_position[id].y);
        p(result);
    };

    auto reprocess_function = [&](int id) {
        /*if (id_to_position.find(id) != id_to_position.end()) {*/
        /*    client_position = id_to_position[id];*/
        /*    std::cout << "Client Reprocessing ID: " << id << " - Position set to " << client_position <<
         * std::endl;*/
        /*}*/
    };

    int curr_id = 0;

    std::vector<KeyboardUpdate> kus_since_last_cts_send;

    FixedFrequencyReprocessor client_physics(60, client_process, reprocess_function);
    PeriodicSignal send_signal(60);

    std::vector<glm::vec3> square_vertices = generate_square_vertices(0, 0, .5);
    std::vector<unsigned int> square_indices = generate_square_indices();

    shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR, ShaderUniformVariable::CAMERA_TO_CLIP,
                             glm::mat4(1.0f));
    shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR, ShaderUniformVariable::WORLD_TO_CAMERA,
                             glm::mat4(1.0f));
    shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR, ShaderUniformVariable::LOCAL_TO_WORLD,
                             glm::mat4(1.0f));

    std::function<bool()> termination = [&]() { return glfwWindowShouldClose(window.glfw_window); };
    std::function<void(double)> tick = [&](double dt) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        p("=== TICK START ===");
        client_physics.add_id(curr_id);
        KeyboardUpdate ku(curr_id, input_state.is_pressed(EKey::w), input_state.is_pressed(EKey::s),
                          input_state.is_pressed(EKey::a), input_state.is_pressed(EKey::d));

        id_to_keyboard_update[curr_id] = ku;
        kus_since_last_cts_send.push_back(ku);

        if (client_physics.attempt_to_process()) {
            p("^^^ just processed ^^^");
        }

        if (send_signal.process_and_get_signal()) {
            for (const auto &ku : kus_since_last_cts_send) {
                p("^^^ just processed ^^^");
                network.send_packet(&ku, sizeof(KeyboardUpdate));
            }
            kus_since_last_cts_send.clear();
        }

        std::vector<GameUpdate> game_updates_this_tick;
        std::vector<PacketWithSize> packets = network.get_network_events_received_since_last_tick();

        if (packets.size() >= 1) {
            p("=== ITERATING OVER NEW PACKETS START ===");
            for (PacketWithSize pws : packets) {
                GameUpdate game_update;
                std::memcpy(&game_update, pws.data.data(), sizeof(GameUpdate));
                game_updates_this_tick.push_back(game_update);
            }
            p("=== ITERATING OVER NEW PACKETS END ===");
        }

        if (game_updates_this_tick.size() >= 1) {
            p("=== RECEIVED GAME UPDATE AND NOW RECONCILING START ===");
            GameUpdate last_received_game_update = game_updates_this_tick.back();

            glm::vec2 our_client_position_at_server_id =
                id_to_position[last_received_game_update.last_id_used_to_produce_this_update];

            std::string result =
                std::format("our position at id {} was ({}, {}) now setting position to ({}, {})",
                            last_received_game_update.last_id_used_to_produce_this_update,
                            our_client_position_at_server_id.x, our_client_position_at_server_id.y,
                            last_received_game_update.position_x, last_received_game_update.position_y);
            p(result);

            server_only_transform.position.x = last_received_game_update.position_x;
            server_only_transform.position.y = last_received_game_update.position_y;

            result = std::format("predicted position was: ({}, {}) now setting position to ({}, {})",
                                 transform.position.x, transform.position.y, last_received_game_update.position_x,
                                 last_received_game_update.position_y);
            p(result);

            Transform predicted_transform = transform;
            double predicted_position_x = transform.position.x;
            double predicted_position_y = transform.position.y;
            // slam it in
            transform.position.x = last_received_game_update.position_x;
            transform.position.y = last_received_game_update.position_y;
            current_velocity.x = last_received_game_update.velocity_x;
            current_velocity.y = last_received_game_update.velocity_y;
            // reconcile
            client_physics.re_process_after_id(last_received_game_update.last_id_used_to_produce_this_update);

            result = std::format("position before reconciliation was: ({}, {}) after reconciliation was ({}, {})\n"
                                 "the delta (predicted - reconciled): {}\n"
                                 "=== RECEIVED GAME UPDATE AND NOW RECONCILING END ===",
                                 predicted_position_x, predicted_position_y, transform.position.x, transform.position.y,
                                 glm::length(predicted_transform.position - transform.position));
            p(result);
        }

        curr_id++;

        std::string result =
            std::format("=== TICK END ===\nclient sending at: {}bps", network.average_bits_per_second_sent());
        p(result);

        shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR, ShaderUniformVariable::RGBA_COLOR,
                                 glm::vec4(0, 1, 0, 1));
        shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR,
                                 ShaderUniformVariable::LOCAL_TO_WORLD, transform.get_transform_matrix());
        batcher.cwl_v_transformation_with_solid_color_shader_batcher.queue_draw(0, square_indices, square_vertices);

        batcher.cwl_v_transformation_with_solid_color_shader_batcher.draw_everything();

        shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR, ShaderUniformVariable::RGBA_COLOR,
                                 glm::vec4(1, 0, 0, 1));
        shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR,
                                 ShaderUniformVariable::LOCAL_TO_WORLD, server_only_transform.get_transform_matrix());
        batcher.cwl_v_transformation_with_solid_color_shader_batcher.queue_draw(1, square_indices, square_vertices);

        batcher.cwl_v_transformation_with_solid_color_shader_batcher.draw_everything();

        shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR, ShaderUniformVariable::RGBA_COLOR,
                                 glm::vec4(0, 0, 1, 1));
        shader_cache.set_uniform(ShaderType::CWL_V_TRANSFORMATION_WITH_SOLID_COLOR,
                                 ShaderUniformVariable::LOCAL_TO_WORLD, client_only_transform.get_transform_matrix());
        batcher.cwl_v_transformation_with_solid_color_shader_batcher.queue_draw(2, square_indices, square_vertices);

        batcher.cwl_v_transformation_with_solid_color_shader_batcher.draw_everything();

        TemporalBinarySignal::process_all();
        glfwSwapBuffers(window.glfw_window);
        glfwPollEvents();
    };

    FixedFrequencyLoop ffl;
    ffl.start(512, tick, termination);

    return 0;
}
