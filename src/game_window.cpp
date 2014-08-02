// Behaviour modifiers (defines):
//  STATIC_OVERSCAN:
//      Set overscan compensation to the default Raspbian values.
//  OVERSCAN_LEFT, OVERSCAN_TOP:
//      Hard code a fixed overscan (overridden by STATIC_OVERSCAN).
//  GAME_WINDOW_DISABLE_DIRECT_RENDER:
//      Never render directly to the screen - always use a PBuffer.
//      This is primarily for debugging purposes. It will decrease
//      performance signifficantly.



#include <glog/logging.h>
#include <iostream>
#include <map>
#include <utility>

// Include position important.
#include "game_window.hpp"
#include "input_manager.hpp"

extern "C" {
#include <SDL2/SDL.h>

#ifdef USE_GLES
#include <SDL2/SDL_syswm.h>
#include <bcm_host.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#endif
}

#include "callback.hpp"
#include "callback_registry.hpp"
#include "lifeline.hpp"
#include "lifeline_controller.hpp"



#ifdef USE_GLES
#ifdef STATIC_OVERSCAN
#define OVERSCAN_LEFT 24
#define OVERSCAN_TOP  16
#else
#ifndef OVERSCAN_LEFT
#define OVERSCAN_LEFT 0
#endif
#ifndef OVERSCAN_TOP
#define OVERSCAN_TOP  0
#endif
#endif
int GameWindow::overscan_left = OVERSCAN_LEFT;
int GameWindow::overscan_top  = OVERSCAN_TOP;
#endif



std::map<Uint32,GameWindow*> GameWindow::windows = std::map<Uint32,GameWindow*>();
GameWindow* GameWindow::focused_window = nullptr;


// Need to inherit constructors manually.
// NOTE: This will, and are required to, copy the message.
GameWindow::InitException::InitException(const char *message): std::runtime_error(message) {}
GameWindow::InitException::InitException(const std::string &message): std::runtime_error(message) {}



GameWindow::GameWindow(int width, int height, bool fullscreen) {
    visible = false;
#ifdef GAME_WINDOW_DISABLE_DIRECT_RENDER
    foreground = false;
#else
    foreground = true;
#endif
    was_foreground = foreground;
    resizing = false;
    window_x = 0;
    window_y = 0;
    window_width  = width;
    window_height = height;
    close_requested = false;
    input_manager = new InputManager(this);
    
    if (windows.size() == 0) {
        init_sdl(); // May throw InitException
    }

    // SDL already uses width,height = 0,0 for automatic
    // resolution. Sets maximized if not in fullscreen and given
    // width,height = 0,0.
    window = SDL_CreateWindow ("Project Zygote",
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               width,
                               height,
                               (fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE)
                               | ( (!fullscreen && width == 0 && height == 0) ?
                                   SDL_WINDOW_MAXIMIZED : 0 )
#ifdef USE_GL
			       | SDL_WINDOW_OPENGL
#endif
                               );
    if (window == nullptr) {
        LOG(ERROR) << "Failed to create SDL window.";
        deinit_sdl();
        throw GameWindow::InitException("Failed to create SDL window");
    }

#ifdef USE_GLES
    SDL_GetWindowWMInfo(window, &wm_info);
#endif

#ifdef USE_GLES
    dispmanDisplay = vc_dispmanx_display_open(0);
#endif

#ifdef USE_GLES
    // Currently has no use with a desktop GL setup.
    // renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    sdl_window_surface = SDL_GetWindowSurface(window);
    background_surface = nullptr;
#endif
    
    try {
        init_gl();
    }
    catch (InitException e) {
#ifdef USE_GLES
        vc_dispmanx_display_close(dispmanDisplay);
        // SDL_DestroyRenderer (renderer);
#endif
        SDL_DestroyWindow (window);
        if (windows.size() == 0) {
            deinit_sdl();
        }
        throw e;
    }

    windows[SDL_GetWindowID(window)] = this;
}

GameWindow::~GameWindow() {
    deinit_gl();

#ifdef USE_GLES
    vc_dispmanx_display_close(dispmanDisplay); // (???)
    // SDL_DestroyRenderer (renderer);
#endif
    // window_count--;
    windows.erase(SDL_GetWindowID(window));
    
    SDL_DestroyWindow (window);
    if (windows.size() == 0) {
        deinit_sdl();
    }

    callback_controller.disable();
    
    delete input_manager;
}


void GameWindow::init_sdl() {
    int result;
    
#ifdef USE_GLES
    bcm_host_init();
#endif
    
    LOG(INFO) << "Initializing SDL...";
    result = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  
    if (result != 0) {
        throw GameWindow::InitException("Failed to initialize SDL");
    }

#ifdef USE_GLES
    SDL_VERSION(&wm_info.version);
#endif
    
    LOG(INFO) << "SDL initialized.";
}


void GameWindow::deinit_sdl() {
    LOG(INFO) << "Deinitializing SDL...";
    
    // Should always work.
    SDL_Quit ();
    
    LOG(INFO) << "SDL deinitialized.";
}


void GameWindow::init_gl() {
#ifdef USE_GLES  
    EGLBoolean result;
  
    static const EGLint attribute_list[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_NONE
    };

    static const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    // Get an EGL display connection

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        throw GameWindow::InitException("Error getting display");
    }

    // Initialize EGL display connection
    result = eglInitialize(display, nullptr, nullptr);
    if (result == EGL_FALSE) {
        eglTerminate(display);
        throw GameWindow::InitException("Error initializing display connection");
    }

    // Get frame buffer configuration.
    result = eglChooseConfig(display, attribute_list, &config, 1, &configCount);
    if (result == EGL_FALSE) {
        eglTerminate(display);
        throw GameWindow::InitException("Error getting window frame buffer configuration");
    }

    //Should I use eglBindAPI? It is auomatically ES anyway.

    // Create EGL rendering context
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        eglTerminate(display);
        throw GameWindow::InitException("Error creating rendering context");
    }

    // Surface initialization is done here as it can be called multiple
    // times after main initialization.
    init_surface();
#endif
#ifdef USE_GL
    sdl_gl_context = SDL_GL_CreateContext(window);
#endif
}


void GameWindow::deinit_gl() {
#ifdef USE_GLES
    // Release EGL resources
    deinit_surface();
    eglDestroyContext(display, context);
    eglTerminate(display);
#endif
#ifdef USE_GL
    SDL_GL_DeleteContext(sdl_gl_context);
#endif
}


void GameWindow::init_surface() {
    int x, y, w, h;

#ifdef USE_GLES
    // It turns out that SDL's window position information is not good
    // enough, as it reports for the window border, not the rendering
    // area. For the time being, we shall be using LibX11 to query the
    // window's position.
    
    // child is just a place to put something. We don't need it.
    Window child;
    XTranslateCoordinates(wm_info.info.x11.display,
                          wm_info.info.x11.window,
                          XDefaultRootWindow(wm_info.info.x11.display),
                          0,
                          0,
                          &x,
                          &y,
                          &child);
#endif
    // SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &w, &h);
#ifdef USE_GL
    // We don't care in desktop GL.
    x = y = 0;
#endif
    init_surface(x, y, w, h);
}


void GameWindow::init_surface(int x, int y, int w, int h) {
    deinit_surface();
    // Because deinit clears this.
    change_surface = InitAction::DO_INIT;
#ifdef USE_GLES
    EGLSurface new_surface;
    
    EGLBoolean result;

    if (foreground) {
        // Rendering directly to screen.
        
        VC_RECT_T destination;
        VC_RECT_T source;
  
        static EGL_DISPMANX_WINDOW_T nativeWindow;

        LOG(INFO) << "Initializing window surface.";
  
        // Create EGL window surface.

        destination.x = x + GameWindow::overscan_left;
        destination.y = y + GameWindow::overscan_top;
        destination.width = w;
        destination.height = h;

        LOG(INFO) << "New surface: " << w << "x" << h << " at (" << x << "," << y << ").";

        source.x = 0;
        source.y = 0;
        source.width  = w << 16; // (???)
        source.height = h << 16; // (???)

        DISPMANX_UPDATE_HANDLE_T dispmanUpdate;
        dispmanUpdate  = vc_dispmanx_update_start(0); // (???)

        dispmanElement = vc_dispmanx_element_add(dispmanUpdate, dispmanDisplay,
                                                 0/*layer*/, &destination, 0/*src*/,
                                                 &source, DISPMANX_PROTECTION_NONE,
                                                 0 /*alpha*/, 0/*clamp*/, (DISPMANX_TRANSFORM_T)0/*transform*/); // (???)

        nativeWindow.element = dispmanElement;
        nativeWindow.width = w; // (???)
        nativeWindow.height = h; // (???)
        vc_dispmanx_update_submit_sync(dispmanUpdate); // (???)
    
        new_surface = eglCreateWindowSurface(display, config, &nativeWindow, nullptr);
        if (new_surface == EGL_NO_SURFACE) {
            std::stringstream hex_error_code;
            hex_error_code << std::hex << eglGetError();

            throw GameWindow::InitException("Error creating window surface: " + hex_error_code.str());
        }
    } else {
        EGLint attribute_list[] = {
            EGL_WIDTH, w,
            EGL_HEIGHT, h,
            EGL_NONE
        };
        
        LOG(INFO) << "New surface: " << w << "x" << h << " (Pixel Buffer).";

        new_surface = eglCreatePbufferSurface(display, config, attribute_list);
        if (new_surface == EGL_NO_SURFACE) {
            std::stringstream hex_error_code;
            hex_error_code << std::hex << eglGetError();

            throw GameWindow::InitException("Error creating pbuffer surface: " + hex_error_code.str());
        }

        // We'll now want an image for aiding the display of our
        // background window's content. This has a good chance of having
        // an out-of-memory error.
        // try {
        //     // w by h image, non opengl as we don't want power of two.
        //     background = Image(w, h, false);
        // } catch (std::bad_alloc e) {
        //     LOG(ERROR) << "Failed to create image for window background." << e.what();
        //     background = Image();
        // }

        sdl_window_surface = SDL_GetWindowSurface(window);
        
        // Create an SDL surface for background blitting. RGBX
        background_surface = SDL_CreateRGBSurface(0,
                                                  sdl_window_surface->w,
                                                  sdl_window_surface->h,
                                                  32,
#if SDL_BYTE_ORDER == SDL_BIG_ENDIAN
                                                  0xff000000,
                                                  0x00ff0000,
                                                  0x0000ff00,
                                                  0x00000000
#else
                                                  0x000000ff,
                                                  0x0000ff00,
                                                  0x00ff0000,
                                                  0x00000000
#endif
                                                  );
        SDL_SetSurfaceBlendMode(background_surface, SDL_BLENDMODE_NONE);
    }
    surface = new_surface;

    // Connect the context to the surface.
    result = eglMakeCurrent(display, surface, surface, context);
    if (result == EGL_FALSE) {
        throw GameWindow::InitException("Error connecting context to surface");
    }

    // Clean up any garbage in the SDL window.
    // SDL_RenderClear(renderer);
    // SDL_RenderPresent(renderer);
#endif

    was_foreground = foreground;
    visible = true;
    change_surface = InitAction::DO_NOTHING;
    // Only set these if the init was successful.
    window_x = x;
    window_y = y;
    window_width = w;
    window_height = h;
}


void GameWindow::deinit_surface() {
#ifdef USE_GLES
    if (visible) {
        int result;

        if (was_foreground) {
            DISPMANX_UPDATE_HANDLE_T dispmanUpdate;
            dispmanUpdate  = vc_dispmanx_update_start(0); // (???)
            vc_dispmanx_element_remove (dispmanUpdate, dispmanElement);
            vc_dispmanx_update_submit_sync(dispmanUpdate); // (???)
        } else {
            if (background_surface != nullptr) {
                SDL_FreeSurface(background_surface);
                background_surface = nullptr;
            }
        }
        
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        result = eglDestroySurface(display, surface);
        if (result == EGL_FALSE) {
            throw GameWindow::InitException("Error destroying EGL surface");
        }
    }
    visible = false;
    change_surface = InitAction::DO_NOTHING;
#endif
}


void GameWindow::update() {
    SDL_Event event;
    bool close_all = false;
    
    for (auto pair : windows) {
        GameWindow* window = pair.second;
        window->input_manager->clean();
    }
    
    while (SDL_PollEvent(&event)) {
        GameWindow* window;
        switch (event.type) {
        case SDL_QUIT: // Primarily used for killing when we become blind.
            close_all = true;
            break;
        case SDL_WINDOWEVENT:
            window = windows[event.window.windowID];

            // Instead of reinitialising on every event, do it ater we have
            // scanned the event queue in full.
            // Should focus events be included?
            switch (event.window.event) {
            case SDL_WINDOWEVENT_CLOSE:
                window->request_close();
                break;
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_MAXIMIZED:
            case SDL_WINDOWEVENT_RESTORED:
                window->resizing = true;
                LOG(INFO) << "Need surface reinit (resize)";
                window->change_surface = InitAction::DO_INIT;
                focused_window = window;
                break;
            case SDL_WINDOWEVENT_MOVED:
                LOG(INFO) << "Need surface reinit (moved)";
                window->change_surface = InitAction::DO_INIT;
                focused_window = window;
                break;
            case SDL_WINDOWEVENT_SHOWN:
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                LOG(INFO) << "Need surface reinit (gained focus)";
#ifndef GAME_WINDOW_DISABLE_DIRECT_RENDER
                window->foreground = true;
#endif
                window->change_surface = InitAction::DO_INIT;
                focused_window = window;
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
            case SDL_WINDOWEVENT_MINIMIZED:
            case SDL_WINDOWEVENT_HIDDEN:
                LOG(INFO) << "Need surface reinit (lost focus)";
                window->foreground = false;
                // This used to deinit, but that is actually bad.
                window->change_surface = InitAction::DO_INIT;
                if (focused_window == window) {
                    focused_window = nullptr;
                }
                break;
            default:
                LOG(WARNING) << "Unhandled WM event.";
                break;
            }
            break;
        }
        // Let the input manager use the event (even if we used it).
        if (focused_window) {
            focused_window->input_manager->handle_event(&event);
        }
    }

    // Perform updates to windows which may be required following
    // events, such as reinitializing surface, closing, callbacks.
    for (auto pair : windows) {
        GameWindow* window = pair.second;

#ifdef USE_GLES
        // Hacky fix: The events don't quite chronologically work, so
        // check the window position to start any needed surface update.
        int x, y;
        Window child;
        XTranslateCoordinates(window->wm_info.info.x11.display,
                              window->wm_info.info.x11.window,
                              XDefaultRootWindow(window->wm_info.info.x11.display),
                              0,
                              0,
                              &x,
                              &y,
                              &child);
        // Ensure that the window focus is correct. Once again, don't
        // rely on SDL events.
        // Currently, we test for window focus via input grab.
        // if (window->change_surface == InitAction::DO_NOTHING) {
        //     if (SDL_GetWindowFlags(window->window) & SDL_WINDOW_INPUT_FOCUS) {
        //         if (window->foreground == false) {
        //             LOG(INFO) << "Need surface reinit (gained focus).";
        //             window->foreground = true;
        //             window->change_surface = InitAction::DO_INIT;
        //         }
        //     } else {
        //         if (window->foreground == true) {
        //             LOG(INFO) << "Need surface reinit (lost focus).";
        //             window->foreground = false;
        //             window->change_surface = InitAction::DO_INIT;
        //         }
        //     }
        // }
        if ((window->window_x != x || window->window_y != y) && window->visible) {
            LOG(INFO) << "Need surface reinit (moved).";
            window->change_surface = InitAction::DO_INIT;
        }
#endif
        
        switch (window->change_surface) {
        case InitAction::DO_INIT:
            try {
                window->init_surface();
            }
            catch (InitException e) {
                LOG(WARNING) << "Surface reinit failed: " << e.what();
            }
            break;
        case InitAction::DO_DEINIT:
            window->deinit_surface();
            break;
        case InitAction::DO_NOTHING:
            // Do nothing - hey, I don't like compiler warnings.
            break;
        }

        if (window->resizing) {
            window->resize_callbacks.broadcast(window);
            window->resizing = false;
        }
        window->input_manager->run_callbacks();
        
        if (close_all) {
            window->request_close();
        }
    }
}


void GameWindow::request_close() {
    close_requested = true;
}


void GameWindow::cancel_close() {
    close_requested = false;
}


bool GameWindow::check_close() {
    return close_requested;
}


std::pair<int, int> GameWindow::get_size() {
    return std::pair<int, int>(window_width, window_height);
}


void GameWindow::use_context() {
#ifdef USE_GLES
    if (visible) {
        eglMakeCurrent(display, surface, surface, context);
    }
#endif
#ifdef USE_GL
    SDL_GL_MakeCurrent(window, sdl_gl_context);
#endif
    // In theory, not required:
    // glViewport(0, 0, w, h);
}


void GameWindow::disable_context() {
#ifdef USE_GLES
    if (visible) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
#endif
#ifdef USE_GL
    SDL_GL_MakeCurrent(window, NULL);
#endif
}


void GameWindow::swap_buffers() {
#ifdef USE_GLES
    if (visible) {
        if (foreground) {
            // Only "direct"-to-screen rendering is double buffered.
            eglSwapBuffers(display, surface);
        } else {
            // Render the content of the pixel buffer to the SDL window.

            // Copy the render into a compatible surface.
            glReadPixels(0,
                         0,
                         background_surface->w,
                         background_surface->h,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         background_surface->pixels);

            // Copy (blit) the surface (whilst flipping) to the SDL window's surface.
            SDL_Rect dst;
            SDL_Rect src;
            dst.x = src.x = 0;
            dst.w = src.w = sdl_window_surface->w;
            dst.h = src.h = 1;
            for (int y = 0; y < background_surface->h; y++) {
                src.y = sdl_window_surface->h - y - 1;
                dst.y = y;
                SDL_BlitSurface(background_surface, &src, sdl_window_surface, &dst);
            }
            SDL_UpdateWindowSurface(window);
        }
    }
#endif
#ifdef USE_GL
    SDL_GL_SwapWindow(window);
#endif
}


InputManager* GameWindow::get_input_manager() {
    return input_manager;
}


std::pair<float,float> GameWindow::get_ratio_from_pixels(std::pair<int,int> pixels) {
    return std::pair<float,float>((float)pixels.first / (float)window_width, (float)pixels.second / (float) window_height);
}


void GameWindow::register_resize_handler(Callback<void, GameWindow*> callback) {
    resize_callbacks.register_callback(callback);
}

Lifeline GameWindow::register_resize_handler(std::function<void(GameWindow*)> func) {
    Callback<void, GameWindow*> callback(func);
    resize_callbacks.register_callback(callback);
    return Lifeline([this, callback] () {
            resize_callbacks.unregister_callback(callback);
        },
        callback_controller);
}
