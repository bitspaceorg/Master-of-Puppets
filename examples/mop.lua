-- Master of Puppets — Default Configuration
-- Place this file next to the executable, or pass the path to mop_config_load.
-- Edit keymaps, viewport, and camera settings to taste.

viewport = {
    width  = 960,
    height = 720,
    clear_color = { 0.12, 0.12, 0.16, 1.0 },
}

camera = {
    distance = 4.5,
    yaw      = 0.6,
    pitch    = 0.4,
    target   = { 0, 0.4, 0 },
    fov      = 60,
}

-- Key names: lowercase letters (a-z), "escape", "space", "tab"
-- MOP actions: translate, rotate, scale, wireframe, reset_view, deselect
-- App actions: any string — the app handles unknown actions itself
keymap = {
    t      = "translate",
    g      = "rotate",
    e      = "scale",
    w      = "wireframe",
    r      = "reset_view",
    escape = "deselect",
    space  = "toggle_auto_rotate",
    s      = "spawn_cube",
    i      = "import_obj",
    q      = "quit",
}
