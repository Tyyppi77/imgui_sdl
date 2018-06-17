# imgui_sdl

Implements an SDL2 based renderer for Dear Imgui. Uses SDL2's new hardware accelerated renderer interface (SDL_Renderer).

# Current Rendering Result

![Render Result](https://i.imgur.com/9o8s9tU.png)

As you can see, there are quite a bit of rendering artifacts that arise from incorrect triangle rasterization, but as a whole it's already in a usable state. There are quite a bit of small things left to do, however.

## Links

* [SDL2](https://www.libsdl.org/)
* [Dear Imgui](https://github.com/ocornut/imgui)