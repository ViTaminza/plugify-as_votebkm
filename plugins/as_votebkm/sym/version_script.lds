{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* admin_system binding slots (defined by the generated admin_system.cpp, filled by the
           plugify module from the admin_system plugin's exported methods) */
        __admin_system_*;

        /* hudmenu binding slots (GetClientMenuType; optional dependency). Without the plugin the
           globals stay null and the menu type uses the "hud if registered, else button" fallback. */
        __hudmenu_*;

        /* No exported methods: as_votebkm exposes nothing to other plugins. */

    local: *;
};
