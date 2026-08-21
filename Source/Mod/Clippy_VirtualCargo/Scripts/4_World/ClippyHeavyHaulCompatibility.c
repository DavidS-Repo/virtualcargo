#ifdef Clippy_Heavy_Haul
// Heavy Haul 2.4 classifies a trader-spawned barrel with nonzero liquid quantity
// as loaded. Its Barrel_ColorBase override then rejects every hierarchy parent,
// including the player's own hands and vehicle cargo/attachment locations. DayZ's
// inventory UI calls CanPutIntoHands merely to decide whether the current item is
// draggable, so that rejection traps the barrel even though ground moves are valid.
modded class Barrel_ColorBase
{
    override bool CanPutIntoHands(EntityAI parent)
    {
        InventoryLocation location = new InventoryLocation;
        if (parent && GetInventory() && GetInventory().GetCurrentInventoryLocation(location) && location.IsValid())
        {
            if (location.GetType() == InventoryLocationType.HANDS && location.GetParent() == parent)
                return true;

            if ((location.GetType() == InventoryLocationType.CARGO || location.GetType() == InventoryLocationType.PROXYCARGO || location.GetType() == InventoryLocationType.ATTACHMENT) && ClippyHeavyHaul.HasStoredContents(this))
            {
                PlayerBase player = PlayerBase.Cast(parent);
                if (!player || !ClippyHeavyHaulBaseCanPutIntoHands(parent) || !ClippyHeavyHaul.CanCarryLoadedStorage(player, this))
                    return false;
                if (IsBeingPlaced() || IsSetForDeletion() || IsLocked())
                    return false;
                if (location.GetType() == InventoryLocationType.ATTACHMENT && GetNumberOfItems() > 0)
                    return false;
                return IsTakeable() || location.GetType() == InventoryLocationType.ATTACHMENT;
            }
        }

        return super.CanPutIntoHands(parent);
    }
}
#endif
