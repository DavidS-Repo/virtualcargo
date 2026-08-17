class CfgPatches
{
    class Clippy_VirtualCargo
    {
        units[] = {"ClippyVirtualCargoCrate"};
        weapons[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data", "DZ_Scripts", "DZ_Gear_Containers"};
    };
};

class CfgMods
{
    class Clippy_VirtualCargo
    {
        dir = "Clippy_VirtualCargo";
        name = "Clippy's Virtual Cargo";
        author = "Clippy-1";
        version = "0.5.0";
        type = "mod";
        dependencies[] = {"Game", "World", "Mission"};

        class defs
        {
            class gameScriptModule
            {
                value = "";
                files[] = {"Clippy_VirtualCargo/Scripts/3_Game"};
            };
            class worldScriptModule
            {
                value = "";
                files[] = {"Clippy_VirtualCargo/Scripts/4_World"};
            };
            class missionScriptModule
            {
                value = "";
                files[] = {"Clippy_VirtualCargo/Scripts/5_Mission"};
            };
        };
    };
};

class CfgVehicles
{
    class WoodenCrate;

    class ClippyVirtualCargoCrate: WoodenCrate
    {
        scope = 1;
        displayName = "Virtual Cargo Test Crate";
        descriptionShort = "Native drag-and-drop test container backed by Clippy Virtual Cargo.";
        itemsCargoSize[] = {10,10};
    };

};
