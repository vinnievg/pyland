// //////////////////////////////////////////////////////////////
// //////////////////////// CURRENT BUGS ////////////////////////
// //////////////////////////////////////////////////////////////
//  Shader caches are not cleaned up when a graphics context ends.
//      Not really a problem unless using multiple contexts.
//  Shader caches are created twice and destroyed once due to copying
//      when inserting into the map (pair?). This doesn't do any harm,
//      it's just weird.

#include <glog/logging.h>
#include <iostream>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "shader.hpp"
#include "graphics_context.hpp"



std::map<GraphicsContext*, std::shared_ptr<Shader::ShaderCache>> Shader::shader_caches;



// Need to inherit constructors manually.
// NOTE: This will, and are required to, copy the message.
Shader::LoadException::LoadException(const char *message): std::runtime_error(message) {}
Shader::LoadException::LoadException(const std::string &message): std::runtime_error(message) {}



static char* load_file(std::string filename) {
    char* content;
    std::ifstream file;
    
    file.open(filename);
    
    // If you ever need [more than] 2GiB, you are doing something
    // seriously wrong.
    file.seekg(0, std::ios_base::end);
    int file_size = (int)file.tellg();
    file.seekg(0, std::ios_base::beg);
    
    content = new char[file_size+1];
    file.read(content, file_size);
    if (file.gcount() != file_size) {
        LOG(ERROR) << "Unable to load shader file \"" << filename << "\".";
        throw Shader::LoadException("Unable to load shader file");
    }
    
    file.close();

    // Add null terminator.
    content[file_size] = '\0';
    
    return content;
}



Shader::ShaderCache::ShaderCache() {
    LOG(INFO) << "Created shader cache " << this;
}


Shader::ShaderCache::~ShaderCache() {
    // We do not clean up the shaders here. This will be done by shared
    // pointers after they are no longer needed.
    LOG(INFO) << "Destroyed shader cache " << this;
}


std::shared_ptr<Shader> Shader::ShaderCache::get_shader(const std::string program_name) {
    LOG(INFO) << "Getting shader \"" << program_name << "\" from cache " << this;
    if (shaders.count(program_name) == 0) {
        // First-time load.
        try {
            std::shared_ptr<Shader> shader = std::make_shared<Shader>(program_name);
            shaders.insert(std::make_pair(program_name, shader));
            shader->cache = this;
            return shader;
        }
        catch (Shader::LoadException e) {
            LOG(ERROR) << "Error creating shared shader \"" << program_name << "\": " << e.what();
            throw e;
        }
    }
    else {
        // Get from cache.
        std::shared_ptr<Shader> shader = shaders.find(program_name)->second.lock();
        shader->cache = this;
        // Some say we should check the pointer, but we don't keep dead
        // weak pointers lying around for us to care.
        return shader;
    }
}


void Shader::ShaderCache::remove_shader(const std::string program_name) {
    LOG(INFO) << "Removing shader \"" << program_name << "\" from cache " << this;
    shaders.erase(program_name);
}



std::shared_ptr<Shader> Shader::get_shared_shader(const std::string program_name) {
    GraphicsContext* context = GraphicsContext::get_current();
    
    if (shader_caches.count(context) == 0) {
        // Create a new ShaderCache as this is the first of its context.
        std::shared_ptr<ShaderCache> shader_cache = std::make_shared<ShaderCache>();
        shader_caches.insert(std::make_pair(context, shader_cache));
        context->register_resource_releaser(std::function<void()>([context] () {Shader::shader_caches.erase(context);}));
    }

    return shader_caches.find(context)->second->get_shader(program_name);
}


Shader::Shader(const std::string program_name):
    Shader(
#ifdef USE_GLES
           program_name + ".glesv",
           program_name + ".glesf"
#endif
#ifdef USE_GL
           program_name + ".glv",
           program_name + ".glf"
#endif
           ) {
}

Shader::Shader(const std::string vs, const std::string fs) {
    GLint linked;

    char* vs_src = load_file(vs);
    char* fs_src = load_file(fs);
    
    //Load the fragment and vertex shaders
    vertex_shader = load_shader(GL_VERTEX_SHADER, vs_src);
    fragment_shader = load_shader(GL_FRAGMENT_SHADER, fs_src);

    delete[] vs_src;
    delete[] fs_src;

    //Create the program object
    program_obj = glCreateProgram();
    
    if(program_obj == 0) {
        LOG(ERROR) << "Shader creation: Could not create program object.";
        LOG(ERROR) << "Flag: " << glGetError();
        throw Shader::LoadException("Unable to create shader program");
    }

    glAttachShader(program_obj, vertex_shader);
    glAttachShader(program_obj, fragment_shader);
    
    // Temporary hack before restructuring.
    glBindAttribLocation(program_obj,0 /* VERTEX_POS_INDX */, "a_position");
    glBindAttribLocation(program_obj, 1/*VERTEX_TEXCOORD0_INDX*/, "a_texCoord");
    
    //Link the program
    glLinkProgram(program_obj);
    
    //Check to see if we have any log info
    glGetProgramiv(program_obj, GL_LINK_STATUS, &linked);
    
    if(!linked) {
        GLint info_len = 0;
        
        glGetProgramiv(program_obj, GL_INFO_LOG_LENGTH, &info_len);
        
        if(info_len > 1) {
            char* info_log = new char[sizeof(char)*info_len];
        
            glGetProgramInfoLog(program_obj, info_len, nullptr, info_log);
            LOG(ERROR) << "Program linking:\n" << info_log;
            delete []info_log;
        }
        glDeleteShader(fragment_shader);
        glDeleteShader(vertex_shader);
        glDeleteProgram(program_obj);
        throw Shader::LoadException("Unable to link shader program");
    }

    loaded = true;
}


Shader::~Shader() {
    if (cache != nullptr) {
        cache->remove_shader(program_name);
    }
    glDeleteShader(fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteProgram(program_obj);  
}


GLuint Shader::load_shader(GLenum type, const std::string src) {
    GLuint shader;
    GLint compiled = 0;
    
    // Create the shader object 
    shader = glCreateShader(type);

    if(shader == 0) {
        // Couldn't create the shader
        return 0;
    }

    // Load shader source code
    const char* source = src.c_str();
    glShaderSource(shader, 1, &source, nullptr);

    // Compile the shader
    glCompileShader(shader);

    // Check for errors
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    // Handle the errors
    if(!compiled) {
        GLint info_len = 0;
        
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        
        if(info_len > 1) {
            char* info_log = new char[sizeof(char) * info_len];

            glGetShaderInfoLog(shader, info_len, nullptr, info_log);
            LOG(ERROR) << "Shader loading failed:\n"<< info_log;
            delete []info_log;
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;

} 


void Shader::bind_location_to_attribute(GLuint location, const char* variable) {
    glBindAttribLocation(program_obj, location, variable);
}


void Shader::link() {
    glLinkProgram(program_obj);
}
