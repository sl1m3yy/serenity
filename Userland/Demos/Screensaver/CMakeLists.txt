serenity_component(
    Screensaver
    TARGETS Screensaver
)

set(SOURCES
    Screensaver.cpp
)

serenity_app(Screensaver ICON app-screensaver)
target_link_libraries(Screensaver PRIVATE LibGUI LibCore LibGfx LibMain)
