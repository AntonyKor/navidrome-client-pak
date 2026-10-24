# Navidrome.pak

A native music player for the TrimUI Brick (NextUI) that streams from a [Navidrome](https://www.navidrome.org/) server, or any server that speaks the Subsonic API. It's a port of [PlexMusicPak](https://github.com/thwonp/PlexMusicPak) by Thwonp: the player, UI, offline mode and downloads come from that project, and the Plex layer is replaced with a Subsonic client.

It runs natively (SDL2, no interpreter), is cross-compiled for ARM aarch64, and shows up under Apps → Tools once installed.

## Features

**Browsing**
- Artists → Albums → Tracks, a flat Albums list sorted by release year, and Recently Added (online only)
- Album art next to every list (from Navidrome's `getCoverArt`, cached on the SD card)
- Multiple music folders: pick one under Settings → Library

**Playback**
- Plays MP3, FLAC, WAV, M4A/AAC, OGG Vorbis and Opus directly
- Optional server-side Opus transcoding (96–320 kbps) for streaming and for downloads
- Formats the device can't decode (ALAC, WMA, APE, …) are transcoded to Opus automatically
- Progressive playback: audio starts before the whole track has downloaded
- Shows "now playing" on the server and scrobbles at 50% (Navidrome forwards these to Last.fm / ListenBrainz if you've set that up)

**Favorites = Navidrome stars**
- Y on a track stars or unstars it on the server; ♥ marks starred tracks
- "Favorite Tracks" lists your starred songs and refreshes from the server when you open it
- Starring while offline is queued and sent the next time you go online

**Offline**
- Y on an album downloads it; Y on "Favorite Tracks" downloads all starred songs
- SELECT on the home screen switches between online and offline browsing

## Installation

1. Download `Navidrome.pak.zip` (or build it, see below).
2. Unzip it to `Tools/tg5040/Navidrome.pak/` on the SD card:
   ```
   Tools/tg5040/Navidrome.pak/
     launch.sh
     pak.json
     res/
     bin/tg5040/
       navidrome.elf
       libmsettings.so
   ```
3. Launch it from Apps → Tools.

## Signing in

On first launch you get a sign-in screen with three fields: **Server**, **Username** and **Password**. Press A on a field to type with the on-screen keyboard, then pick **Connect**. The app checks the credentials with the server before saving them.

Server URL examples: `http://192.168.1.10:4533`, `https://music.example.com`. If you leave out `http://`, it's added for you. Self-signed HTTPS certificates are accepted.

**On-screen keyboard**

| Button | Action |
|---|---|
| D-pad | Move |
| A | Type the selected key |
| B | Backspace |
| X | Space |
| Y | Switch abc / ABC / symbols |
| START | Done |
| SELECT | Cancel |

The bottom row also has a `:4533` key (Navidrome's default port).

**Skipping the keyboard:** you can also create the config file on the SD card yourself before launching:

```json
{
  "server_url": "http://192.168.1.10:4533",
  "username": "me",
  "password": "secret"
}
```

Save it as `/mnt/SDCARD/.userdata/shared/navidrome/config.json`.

To change server or account later: Settings → Server. To sign out: Settings → Sign Out (this forgets the password and keeps the server and username).

## Controls

**Browsing**

| Button | Action |
|---|---|
| D-pad | Navigate lists |
| A | Select / confirm |
| B | Back |
| L1 / R1 | Jump to previous/next letter or year |
| SELECT | Toggle offline mode |
| Y (on album, online) | Download album for offline use |
| Y (on album, offline) | Delete the downloaded album |
| Y (on a track) | Star / unstar |
| Y (on Favorite Tracks, online) | Download all starred songs |

**Now Playing**

| Button | Action |
|---|---|
| A | Play / Pause |
| B | Back to browse (music keeps playing) |
| D-pad left/right (tap) | Previous / Next track |
| D-pad left/right (hold) | Seek ±5 s |
| D-pad up/down | Seek ±30 s |
| L2 | Toggle shuffle |
| R2 | Cycle repeat (Off → All → One) |
| Y | Star / unstar |

**Global**

| Button | Action |
|---|---|
| MENU + START | Quit |
| START (short press) | Help |
| START (long press) | Quit confirm |

## Settings

- **Streaming / Download Quality**: `Original` (default) sends the file as-is. A bitrate asks Navidrome to transcode to Opus, which needs ffmpeg on the server.
- **Track Preload**: how many upcoming tracks to download ahead of time.
- **Screen timeout** and **Pocket Button Lock** (MENU+SELECT to wake).

## Files on the SD card

| What | Path |
|---|---|
| Log | `/mnt/SDCARD/.userdata/tg5040/logs/navidrome.txt` |
| Config (server, username, password, settings) | `/mnt/SDCARD/.userdata/shared/navidrome/config.json` |
| Favorites cache + pending star changes | `/mnt/SDCARD/.userdata/shared/navidrome/favorites.json` |
| Downloads | `/mnt/SDCARD/.userdata/shared/navidrome/downloads/` |
| Art cache | `/mnt/SDCARD/.userdata/shared/navidrome/art/` |

The password is stored in `config.json` in plain text (file mode 0600), because the Subsonic API needs it to compute each request's token. Anyone with the SD card can read it. Consider a dedicated Navidrome user for the device.

## How it talks to Navidrome

All requests go to `{server}/rest/{endpoint}` with Subsonic token auth (`u`, `t = md5(password + salt)`, `s`, `v=1.16.1`, `c=NavidromeMusicPak`, `f=json`). A fresh salt is used for every request, so the password itself is never sent.

| Feature | Endpoint |
|---|---|
| Sign-in check | `ping` |
| Library picker | `getMusicFolders` |
| Artists | `getArtists` |
| Artist's albums | `getArtist` |
| Album tracks | `getAlbum` |
| Albums by year / Recently added | `getAlbumList2` (`byYear`, `newest`) |
| Playback / downloads | `stream` (`format=raw`, or `format=opus&maxBitRate=N`) |
| Cover art | `getCoverArt` (`size=300`) |
| Now playing / scrobble | `scrobble` (`submission=false` / `true`) |
| Favorites | `star`, `unstar`, `getStarred2` |

## Building from source

You need Docker or Podman. The script pulls the `ghcr.io/loveretro/tg5040-toolchain` image and clones NextUI (to `../NextUI` unless you set `NEXTUI_ROOT`).

```bash
scripts/build.sh          # -> bin/tg5040/navidrome.elf + libmsettings.so
scripts/package.sh 0.1.0  # -> build/Navidrome.pak.zip
scripts/build.sh check    # fast compile check of the app sources (-Wall)
```

## Tests

```bash
tests/run_tests.sh        # API layer against recorded Navidrome responses (no network; gcc + ASan)
tests/run_live_test.sh    # real network/TLS against https://demo.navidrome.org (demo/demo)
tests/run_live_test.sh http://my-server:4533 user pass
```

The live test downloads a track (raw and Opus), fetches cover art, and stars then unstars a song.

## Known limitations

- ALAC inside `.m4a` can't be told apart from AAC by the file suffix, so it may fail to play in Original quality. Set a streaming bitrate to have the server transcode it.
- Opus audio stored in a `.ogg` file is reported by the server as `ogg` and sent to the Vorbis decoder, so it fails in Original quality. Same workaround: set a streaming bitrate.
- The favorites list holds up to 500 songs.
- The library picker selects one music folder at a time (no "all folders" option yet).

## Credits

Based on [PlexMusicPak](https://github.com/thwonp/PlexMusicPak) by Thwonp. Uses NextUI, SDL2, mbedTLS, parson, dr_libs, stb_vorbis, opusfile, fdk-aac, minimp4 and kissfft.
