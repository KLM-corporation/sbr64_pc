# Game data (not in the repository)

The port runs the original Android game, so it needs files from your own copy of
**Run Sackboy! Run!** for Android, version 1.0.4. They belong to Sony Interactive Entertainment
and are excluded from git (`.gitignore`).

Fill this folder with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\import_game_data.ps1 -Apk <game.apk> -ObbDir <folder with the OBB files>
```

| Path | Taken from |
|---|---|
| `lib\libRSBR_arm32.so` | APK `lib/armeabi-v7a/libRSBR.so` (SHA-256 `80ccb30e…cec77b09` for v1.0.4) |
| `assets\fonts\*.ttf` | APK `assets/fonts/` |
| `music\*.mp3` | APK `res/raw/` (6 tracks) |
| `obb\main.25.com.playstation.runsackboyrun.obb` | the game's main expansion file (255 MB) |
| `obb\patch.42.com.playstation.runsackboyrun.obb` | the game's patch expansion file (30 MB) |
