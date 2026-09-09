uc-online2 payload
==================

No third party binaries are committed to this repo, so this folder holds only
the config. setup.exe downloads steam_api64.dll from the uc-online2 release at
install time and puts it beside the game's own copy; union-crax.ini rides along
from here, once beside that dll and once - the "@exe" copy - beside the game
executable, which is where uc-online2 actually reads it from. Without it the
AppID falls back to 480 and the ticket is issued for the wrong app.

If you want the emulator itself baked into the exe instead of downloaded:

  1. Download a release from https://github.com/UnionCrax-Team/uc-online2
  2. Extract it and copy the files that belong NEXT TO THE GAME EXE into this
     folder, keeping any subfolders the release uses.
  3. Run build.bat again.

Whatever ends up here is copied beside the emulator with the same layout when
you pick uc-online2 in the installer, and anything under "@exe" goes next to
the game executable instead.

Any .ini .txt .cfg .yaml .yml .json .xml file in here is a template, so if the
release ships a config that wants the AppID, edit it once and put a placeholder
where the number goes:

    {APPID}          Steam AppID of the game, detected from the install
    {ACCOUNT_NAME}   your Steam account name, from the registry
    {STEAM_ID}       your SteamID64
    {GAME_NAME}      folder name of the game
    {GAME_DIR}       full path the files were installed into

For example an OnlineFix style ini becomes:

    [Settings]
    AppId={APPID}
    Name={ACCOUNT_NAME}

Files already present in the game folder are parked as <name>.eosbak before
being overwritten, and put back when you remove the backend.
