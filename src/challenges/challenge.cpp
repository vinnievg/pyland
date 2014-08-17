#include <glog/logging.h>
#include <glm/vec2.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stdlib.h>
#include <string>

#include "api.hpp"
#include "challenge.hpp"
#include "challenge_data.hpp"
#include "engine.hpp"
#include "entitythread.hpp"
#include "interpreter.hpp"
#include "make_unique.hpp"
#include "map.hpp"
#include "map_object.hpp"
#include "map_viewer.hpp"
#include "notification_bar.hpp"
#include "object_manager.hpp"
#include "sprite.hpp"


Challenge::Challenge(ChallengeData* _challenge_data) :
    challenge_data(_challenge_data), map(nullptr) {
        map = new Map(challenge_data->map_name);
        MapViewer* map_viewer = Engine::get_map_viewer();
        if(map_viewer == nullptr) {
            throw std::logic_error("MapViewer is not intialised in Engine. In Challenge()");
        }
        map_viewer->set_map(map);

        //Build a sprite for the player
        int sprite_id = make_sprite(glm::ivec2(7, 15), "John", Walkability::BLOCKED, 9,"../resources/characters_1_64.png");

        // WTF
        std::string bash_command =
            std::string("cp python_embed/scripts/long_walk_challenge.py python_embed/scripts/John_")
            + std::to_string(sprite_id) + std::string(".py");
        system(bash_command.c_str());
}

Challenge::~Challenge() {

    //Remove all sprites
    for(int sprite_id : sprite_ids) {
        ObjectManager::get_instance().remove_object(sprite_id);
    }

    //Remove all map objects
    for(int map_object_id : map_object_ids) {
        ObjectManager::get_instance().remove_object(map_object_id);
    }

    Engine::get_notification_bar()->clear_text();
    Engine::get_map_viewer()->set_map(nullptr);

    //Delete the map
    delete map;
    //All threads created for the challenge should have terminated now
    LOG(INFO) << " CHALLENGE DESTROYED ";
}

int Challenge::make_map_object(glm::vec2 position,
                               std::string name,
                               Walkability walkability,
                               int sheet_id,
                               std::string sheet_name) {

    auto new_object(std::make_shared<MapObject>(position, name, walkability, sheet_id, sheet_name));
    ObjectManager::get_instance().add_object(new_object);

    auto new_object_id(new_object->get_id());

    LOG(INFO) << "created new_object with id: " << new_object_id;
    map_object_ids.push_back(new_object_id);
    map->add_map_object(new_object_id);

    return new_object_id;
}

int Challenge::make_sprite(glm::vec2 position, std::string name, Walkability walkability, int sheet_id, std::string sheet_name) {
    LOG(INFO) << "Creating sprite";

    // Registering new sprite with game engine
    auto new_sprite(std::make_shared<Sprite>(position, name, walkability, sheet_id, sheet_name));

    LOG(INFO) << "Adding sprite";
    ObjectManager::get_instance().add_object(new_sprite);

    int sprite_id = new_sprite->get_id();
    sprite_ids.push_back(sprite_id);


    map->add_sprite(sprite_id);
    Engine::get_map_viewer()->set_map_focus_object(sprite_id);
    LOG(INFO) << "Creating sprite wrapper";
    LOG(INFO) << "ID " << sprite_id;

    // Register user controled sprite
    // Yes, this is a memory leak. Deal with it.
    auto *a_thing(new Entity(position, name, sprite_id));

    LOG(INFO) << "Registering sprite";
    new_sprite->daemon = std::make_unique<LockableEntityThread>(challenge_data->interpreter->register_entity(*a_thing));
    LOG(INFO) << "Done!";

    return sprite_id;
}

/*
void Challenge::create_gui() {

    //TODO : REMOVE THIS HACKY EDIT - done for the demo tomorrow
    TextFont buttonfont = Engine::get_game_font();
    Text stoptext(&window, buttonfont, true);
    Text runtext(&window, buttonfont, true);
    stoptext.set_text("Stop");
    runtext.set_text("Run");
    // referring to top left corner of text window
    stoptext.move(105, 240 + 20);
    runtext.move(5, 240 + 20);
    stoptext.resize(window.get_size().first-20, 80 + 20);
    runtext.resize(window.get_size().first-20, 80 + 20);

    std::shared_ptr<GUIWindow> sprite_window = std::make_shared<GUIWindow>();;
    sprite_window->set_width_pixels(300);
    sprite_window->set_height_pixels(300);
    std::shared_ptr<Button> run_button = std::make_shared<Button>();
    run_button->set_text("Run");
    run_button->set_on_click([&] () { LOG(ERROR) << "RUN"; callbackstate.restart(); });
    run_button->set_width(0.2f);
    run_button->set_height(0.2f);
    run_button->set_y_offset(0.8f);
    run_button->set_x_offset(0.0f);

    std::shared_ptr<Button> stop_button = std::make_shared<Button>();
    stop_button->set_text("Stop");
    stop_button->set_on_click([&] () {LOG(ERROR) << "STOP";  callbackstate.stop(); });
    stop_button->set_width(0.2f);
    stop_button->set_height(0.2f);
    stop_button->set_y_offset(0.8f);
    stop_button->set_x_offset(0.8f);

    // build navigation bar buttons
    NotificationBar notification_bar;
    Engine::set_notification_bar(&notification_bar);
    SpriteSwitcher sprite_switcher;

    sprite_window->add(run_button);
    sprite_window->add(stop_button);
    for (auto button: notification_bar.get_navigation_buttons()) {
        sprite_window->add(button);
    }

    Engine::set_gui_window(sprite_window);
    gui_manager.set_root(sprite_window);

    // quick fix so buttons in correct location in initial window before gui_resize_func callback
    auto original_window_size = game_window.get_size();
    sprite_window->set_width_pixels(original_window_size.first);
    sprite_window->set_height_pixels(original_window_size.second);

    gui_manager.parse_components();

    std::function<void(GameWindow*)> gui_resize_func = [&] (GameWindow* game_window) {
        LOG(INFO) << "GUI resizing";
        auto window_size = (*game_window).get_size();
        sprite_window->set_width_pixels(window_size.first);
        sprite_window->set_height_pixels(window_size.second);
        gui_manager.parse_components();
    };
    Lifeline gui_resize_lifeline = window.register_resize_handler(gui_resize_func);


    // WARNING: Fragile reference capture
    Lifeline map_resize_lifeline = window.register_resize_handler([&] (GameWindow *) {
        map_viewer.resize();
    });
}
*/
