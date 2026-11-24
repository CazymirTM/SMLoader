# SMContent: add items to the game (API v4, SMLoader 1.5.0)

Adds or edits the game's data files in memory: shape sets, tool sets,
projectiles, effects, recipes, item texts, icons, scripts. Nothing in the game
folder is changed, and any number of mods can edit the same file.

```c
int ContentAdd(const char* gamePath, const char* text, size_t len);
int ContentEdit(const char* gamePath, SMContentEditFn fn, void* user);
int ContentAddIcon(const char* itemUuid, const unsigned char* bgra96);
```

- Call them from `SMMod_Init` only, after checking `api->version >= 4`. They
  return non-zero if registered.
- `gamePath` is a game path: `$SURVIVAL_DATA/…`, `$GAME_DATA/…` or
  `$CHALLENGE_DATA/…`.
- `ContentAdd` serves a whole file, usually a new one. Binary files work too:
  meshes (`.dae`, `.fbx`), textures (`.png`, `.tga`). The game converts them on
  first use (a few seconds for a 2048 texture); the loader keeps the result in
  `SMLoader\content\cache` until the file changes.
- `ContentEdit` gets the file's text when the game reads it and returns the new
  text (or `NULL` to leave it). Edits run in registration order, each on the
  previous result.
- `ContentAddIcon` sets the inventory icon of a Survival item: 96×96 BGRA, top
  row first. Up to 420 icons in total.

If the result of an edit of a `.json` or `*set` file isn't valid JSON, that edit
is skipped and reported in `loader.log`. If the game version isn't recognised,
no content is added at all, which the log also says.

## A new item, step by step

1. `ContentAdd` your own set file, e.g.
   `$SURVIVAL_DATA/Objects/Database/ShapeSets/mymod.shapeset`.
2. `ContentEdit` the list that names the set files and insert yours:

   | What | List | Key |
   |---|---|---|
   | Parts | `$SURVIVAL_DATA/Objects/Database/shapesets.json` | `shapeSetList` |
   | Tools | `$SURVIVAL_DATA/Tools/toolsets.json` | `toolSetList` |
   | Projectiles | `$SURVIVAL_DATA/Projectiles/projectilesets.json` | `projectileSetList` |
   | Effects | `$SURVIVAL_DATA/Effects/Database/effectsets.json` | `effectSetList` (objects with `path`) |

3. Texts: insert an entry in
   `$SURVIVAL_DATA/Gui/Language/<Language>/inventoryDescriptions.json`.
4. Recipe: insert one in `$SURVIVAL_DATA/CraftingRecipes/craftbot/craftbot_core.json`.
   To unlock it from the start, add the uuid to `DefaultUnlockedItems` in
   `RecipeManager.lua` with `OnScriptLoad`.
5. A tool also needs a part to show when it lies on the ground: add it to
   `ToolItems` in `util.lua`.
6. Icon: `ContentAddIcon`.

Never change the uuid of an item you released: worlds store it. Everyone in a
co-op game needs the mod.
