#include <algorithm>
#include <glog/logging.h>
#include <glm/vec2.hpp>
#include <map>
#include <memory>
#include <vector>

#include "engine.hpp"
#include "game_window.hpp"
#include "gui/gui_manager.hpp"
#include "layer.hpp"
#include "map.hpp"
#include "map_viewer.hpp"
#include "object.hpp"
#include "object_manager.hpp"
#include "renderable_component.hpp"
#include "sprite.hpp"


MapViewer::MapViewer(GameWindow *window, GUIManager* gui_manager):
    gui_manager(CHECK_NOTNULL(gui_manager)),
    window(CHECK_NOTNULL(window)) {

        resize();

        // Set background color and clear buffers
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        // Disable writing to the alpha channel.
        // Fixes a bug where EGL layer becomes transparent.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        // L./eave this here!!!
        //Disable back face culling.
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}


MapViewer::~MapViewer() {}

void MapViewer::resize() {
    LOG(INFO) << "Map resizing";
    std::pair<int, int> size(window->get_size());

    // Adjust the view to show only tiles the user can see
    set_display_width (float(size.first)  / Engine::get_actual_tile_size());
    set_display_height(float(size.second) / Engine::get_actual_tile_size());

    // Set the viewable fragments
    glScissor(0, 0, size.first, size.second);
    glViewport(0, 0, size.first, size.second);

    if (get_map()) {
        // Readjust the map focus
        refocus_map();
    }
}

void MapViewer::render() {
    CHECK_NOTNULL(map);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    render_map();
    render_objects();
    render_sprites();
    render_gui();
}

void MapViewer::render_map() {
    //Focus onto the player
    refocus_map();

    //Calculate the projection and modelview matrix for the map
    std::pair<int, int> size = window->get_size();
    glm::mat4 projection_matrix = glm::ortho(0.0f, float(size.first), 0.0f, float(size.second), 0.0f, 1.0f);
    glm::mat4 model = glm::mat4(1.0f);
    glm::vec3 translate = glm::vec3(-get_display_x()*32.0f, -get_display_y()*32.0f, 0.0f);
    glm::mat4 translated = glm::translate(model, translate);

    //Draw all the layers, from base to top to get the correct draw order
    int layer_num = 0;
    for(auto layer: map->get_layers()) {
        RenderableComponent* layer_render_component = layer->get_renderable_component();
        Shader* layer_shader = layer_render_component->get_shader().get();

        //Set the matrices
        layer_render_component->set_projection_matrix(projection_matrix);
        layer_render_component->set_modelview_matrix(translated);

        layer_render_component->bind_shader();

        //TODO: I don't want to actually expose the shader, put these into wrappers in the shader object
        glUniformMatrix4fv(glGetUniformLocation(layer_shader->get_program(), "mat_projection"), 1, GL_FALSE,glm::value_ptr(layer_render_component->get_projection_matrix()));
        glUniformMatrix4fv(glGetUniformLocation(layer_shader->get_program(), "mat_modelview"), 1, GL_FALSE, glm::value_ptr(layer_render_component->get_modelview_matrix()));


        layer_render_component->bind_vbos();

        layer_render_component->bind_textures();

        //Calculate the offsets for drawing
        //maps are built left to right, bottom to top
        //        int offset = map->get_tile_texture_vbo_offset(layer_num, map->get_display_x(), 0);

         //      int length = map->get_tile_texture_vbo_offset(layer_num, map->get_display_x()+map->get_display_width() -1, 0);
         //    glDrawArrays(GL_TRIANGLES, offset, (length -offset) / 2); // no of vetices, divide by 2 dimenions
        glDrawArrays(GL_TRIANGLES, 0, layer_render_component->get_num_vertices_render());
        //        std::cout <<" OOF " << offset << " " << length << std::endl;
        //Release the vertex buffers and texppptures
        layer_render_component->release_textures();
        layer_render_component->release_vbos();

        layer_render_component->release_shader();

        //next layer
        layer_num ++;
    }
}

void MapViewer::render_sprites() {
    //Calculate the projection matrix
    std::pair<int, int> size = window->get_size();
    glm::mat4 projection_matrix = glm::ortho(0.0f, float(size.first), 0.0f, float(size.second), 0.0f, 1.0f);
    //Draw the sprites
    const std::vector<int>& sprites = map->get_sprites();
    ObjectManager& object_manager = ObjectManager::get_instance();
    for(auto it = sprites.begin(); it != sprites.end(); ++it) {
        if(*it != 0) {
            std::shared_ptr<Sprite> sprite = object_manager.get_object<Sprite>(*it);

            RenderableComponent* sprite_render_component = sprite->get_renderable_component();

            //Move sprite to the required position
            glm::mat4 model1 = glm::mat4(1.0f);
            glm::vec3 translate1(
                32.0f * (sprite->get_position().x - get_display_x()),
                32.0f * (sprite->get_position().y - get_display_y()),
                 0.0f
            );
            glm::mat4 translated1 = glm::translate(model1, translate1);
            sprite_render_component->set_modelview_matrix(translated1);
            sprite_render_component->set_projection_matrix(projection_matrix);

            sprite_render_component->bind_shader();

            Shader* shader = sprite_render_component->get_shader().get();

            if(shader == nullptr) {
                LOG(ERROR) << "MapViewer::render_map: Shader (sprite_render_component->get_shader()) should not be null";
                return;
            }

            //TODO: I don't want to actually expose the shader, put these into wrappers in the shader object
            glUniformMatrix4fv(glGetUniformLocation(shader->get_program(), "mat_projection"), 1, GL_FALSE,glm::value_ptr(sprite_render_component->get_projection_matrix()));

            glUniformMatrix4fv(glGetUniformLocation(shader->get_program(), "mat_modelview"), 1, GL_FALSE, glm::value_ptr(sprite_render_component->get_modelview_matrix()));

            sprite_render_component->bind_vbos();
            sprite_render_component->bind_textures();

            glDrawArrays(GL_TRIANGLES, 0, sprite_render_component->get_num_vertices_render());

            sprite_render_component->release_textures();
            sprite_render_component->release_vbos();
            sprite_render_component->release_shader();
        }
    }
}
void MapViewer::render_objects() {
    //Calculate the projection matrix
    std::pair<int, int> size = window->get_size();
    glm::mat4 projection_matrix = glm::ortho(0.0f, float(size.first), 0.0f, float(size.second), 0.0f, 1.0f);
    //Draw the objects
    const std::vector<int>& objects = map->get_map_objects();
    ObjectManager& object_manager = ObjectManager::get_instance();
    for(auto it = objects.begin(); it != objects.end(); ++it) {
        if(*it != 0) {
            std::shared_ptr<MapObject> object = object_manager.get_object<MapObject>(*it);

            RenderableComponent* object_render_component = object->get_renderable_component();

            //Move object to the required position
            glm::mat4 model1 = glm::mat4(1.0f);
            glm::vec3 translate1(
                32.0f * (object->get_position().x - get_display_x()),
                32.0f * (object->get_position().y - get_display_y()),
                 0.0f
            );
            glm::mat4 translated1 = glm::translate(model1, translate1);
            object_render_component->set_modelview_matrix(translated1);
            object_render_component->set_projection_matrix(projection_matrix);

            object_render_component->bind_shader();

            Shader* shader = object_render_component->get_shader().get();
            if(shader == nullptr) {
                LOG(ERROR) << "MapViewer::render_map: Shader (object_render_component->get_shader()) should not be null";
                return;
            }

            //TODO: I don't want to actually expose the shader, put these into wrappers in the shader object
            glUniformMatrix4fv(glGetUniformLocation(shader->get_program(), "mat_projection"), 1, GL_FALSE,glm::value_ptr(object_render_component->get_projection_matrix()));

            glUniformMatrix4fv(glGetUniformLocation(shader->get_program(), "mat_modelview"), 1, GL_FALSE, glm::value_ptr(object_render_component->get_modelview_matrix()));

            object_render_component->bind_vbos();
            object_render_component->bind_textures();

            glDrawArrays(GL_TRIANGLES, 0, object_render_component->get_num_vertices_render());

            object_render_component->release_textures();
            object_render_component->release_vbos();
            object_render_component->release_shader();
        }
    }
}
void MapViewer::render_gui() {
    //Calculate the projection matrix
    std::pair<int, int> size = window->get_size();
    glm::mat4 projection_matrix = glm::ortho(0.0f, float(size.first), 0.0f, float(size.second), 0.0f, 1.0f);

    //TODO: Hacky method, clean it up
    RenderableComponent* gui_render_component = gui_manager->get_renderable_component();

    //Move gui_manager to the required position
    glm::mat4 model2 = glm::mat4(1.0f);
    gui_render_component->set_modelview_matrix(model2);
    gui_render_component->set_projection_matrix(projection_matrix);

    gui_render_component->bind_shader();

    Shader* gui_shader = gui_render_component->get_shader().get();
    if(gui_shader == nullptr) {
        LOG(ERROR) << "ERROR: Shader is NULL in MapViewer::render_map";
        return;
    }

    //TODO: I don't want to actually expose the shader, put these into wrappers in the shader object
    glUniformMatrix4fv(glGetUniformLocation(gui_shader->get_program(), "mat_projection"), 1, GL_FALSE,glm::value_ptr(gui_render_component->get_projection_matrix()));

    glUniformMatrix4fv(glGetUniformLocation(gui_shader->get_program(), "mat_modelview"), 1, GL_FALSE, glm::value_ptr(gui_render_component->get_modelview_matrix()));

    gui_render_component->bind_vbos();
    gui_render_component->bind_textures();

    glDrawArrays(GL_TRIANGLES, 0, gui_render_component->get_num_vertices_render());

    gui_render_component->release_textures();
    gui_render_component->release_vbos();
    gui_render_component->release_shader();

}

///
/// Take a line of a given size (length) and a point offset on that line (point):
///
/// ← length    →
/// ├───────•───┤
/// ← point →
///
/// Also takes a display of a given size (bound):
///
/// ← bound→
/// ┼─────────────┼
///
/// If bound == length:
///
///     It places the boxes over eachother:
///
///     ┼─────────────┼
///     ├─────────•───┤
///
/// If length > bound:
///
///     It centres the box on the point:
///
///            ┼─────────────┼
///     ├─────────────•───┤
///
///     Then moves the box inside the bounds, if needed:
///
///         ┼─────────────┼
///     ├─────────────•───┤
///
/// If bound > length:
///
///     It centres the line inside the box:
///
///     ┼─────────────┼
///           |•────────┤
///
///     It then moves the line inside the box, if needed:
///
///     ┼─────────────┼
///         |•────────┤
///
/// Then it returns the distance from the start of length to the start of bound:
///
///     For example,
///
///         ────→
///             ┼─────────────┼
///         ├─────────────•───┤
///
///     which is positive, or
///
///         ←────
///         ┼─────────────┼
///             |•────────┤
///
///     which is negative.
///

float centre_point_in_range(float point, float length, float bound) {
    // First case is a union of the other two
    // and both cases have same first step.
    float bound_offset = point - bound / 2.0f;

    // Crop to valid range: bound inside length or length inside bound
    // Note order of min/max
    if (length >= bound) {
        // bound_offset positive by no more than | length - bound |
        bound_offset = std::min(std::max(bound_offset, 0.0f), length - bound);
    }
    else if (bound >= length) {
        // bound_offset negative by no more than | length - bound |
        bound_offset = std::max(std::min(bound_offset, 0.0f), length - bound);
    }

    return bound_offset;
}

void MapViewer::refocus_map() {
    //Get the object
    ObjectManager& object_manager = ObjectManager::get_instance();

    if(map_focus_object == 0) {
        LOG(INFO) << "MapViewer::refocus_map: No focus.";
        return;
    }

    std::shared_ptr<Sprite> sprite = object_manager.get_object<Sprite>(map_focus_object);

    // If such an sprite exists, move the map to it
    if (sprite) {
        set_display_x(centre_point_in_range(
            // half-tile offset to take centre of sprite
            /* point  */ float(sprite->get_position().x) + 0.5f,
            /* length */ float(map->get_width()),
            /* bound  */ get_display_width()
        ));

        set_display_y(centre_point_in_range(
            // half-tile offset to take centre of sprite
            /* point  */ float(sprite->get_position().y) + 0.5f,
            /* length */ float(map->get_height()),
            /* bound  */ get_display_height()
        ));
    } else {
        LOG(INFO) << "MapViewer::refocus_map: No sprites have focus.";
    }
    Engine::text_updater();
}

void MapViewer::set_map(Map* new_map) {
    map = new_map;
}

void MapViewer::set_map_focus_object(int object_id) {
    //Set the focus to the object if this is a valid object and it is on the map
    if(ObjectManager::is_valid_object_id(object_id)) {
        //        const std::vector<int>& sprites = map->get_sprites();
        map_focus_object = object_id;
        refocus_map();

        //TODO: add this in again
        //If the object is on the map
        /*        if(std::find(sprites.begin(), sprites.end(),object_id) != sprites.end()) {
            //focus on it
            map_focus_object = object_id;
            }*/
    }
    else {
        LOG(ERROR) << "MapViewer::set_map_focus_object: Invalid focus object";
    }
}


Map* MapViewer::get_map() {
    return map;
}


glm::vec2 MapViewer::pixel_to_tile(glm::ivec2 pixel_location) {
    float scale(Engine::get_actual_tile_size());

    glm::vec2 display_position(get_display_x(), get_display_y());

    return (glm::vec2(pixel_location) / scale) + display_position;
}

glm::ivec2 MapViewer::tile_to_pixel(glm::vec2 tile_location) {
    float scale(Engine::get_actual_tile_size());

    glm::vec2 display_position(get_display_x(), get_display_y());

    // Screen offset is reduced by the offset of the display
    return (tile_location - display_position) * scale;
}
