// DayZ 1.29 omits two ordinary sharp tools from the otherwise broad fire-barrel
// recipe list. Register their base class names without replacing the vanilla recipe
// or the RaG BaseItems check that excludes its purpose-built water barrel.
modded class PokeHolesBarrel
{
    override void Init()
    {
        super.Init();
        InsertIngredient(1, "BoneKnife");
        InsertIngredient(1, "HandSaw");
    }
}
