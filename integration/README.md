# Native inventory context-menu integration

DropExplorer registers its Auction House actions with two exports added to
GWToolbox++'s `InventoryManager`. This makes the actions appear in the same
right-click menu as Store Item, Destroy, and Hide this when selling even when
the Drop Explorer window is closed or collapsed.

Apply `InventoryManagerContextMenu.patch` to the matching GWToolbox++ source,
then build and deploy both `GWToolboxdll.dll` and `DropExplorer.dll`. A plugin
DLL by itself cannot extend InventoryManager's private context menu.

The callback returns `false` after handling a menu action so InventoryManager
closes the popup. DropExplorer unregisters its callback before it unloads.
