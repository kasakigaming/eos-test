uc-online2 payload
==================

Empty on purpose: no third party binaries are committed to this repo. Until you
fill this folder, the installer builds fine and reports uc-online2 as
"not bundled in this build".

  1. Download a release from https://github.com/UnionCrax-Team/uc-online2
  2. Extract it and copy the files that belong NEXT TO THE GAME EXE into this
     folder, keeping any subfolders the release uses.
  3. Run build.bat again.

Whatever ends up here is copied into the game folder with the same layout when
you pick uc-online2 in the installer.

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
