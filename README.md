# RplaceBot
A discord bot written using C libraries concord, curl and libpng for displaying rplace canvas backups, and carrying out a few other rplace-related utility commands.

## Configuring
Create a config.json that looks like the following
```json
{
    "logging": {
        "level": "warn",
        "filename": "bot.log",
        "quiet": false,
        "overwrite": true,
        "use_color": true,
        "http": {
            "enable": true,
            "filename": "http.log"
        },
        "disable_modules": ["USER_AGENT"]
    },
    "discord": {
        "token": "YOUR DISCORD BOT TOKEN GOES HERE",
        "default_prefix": {
            "enable": true,
            "prefix": "r/"
        }
    }
}
```

Create rplace_bot.json for bot-specific functionality like the following
```json
{
    "mod_roles": [ "961684196387070022", "977717911764488242", "960971746842935297" ],
    "view_canvases": {
        "canvas1": {
            "socket": "wss://server.rplace.live:443",
            "http": "https://raw.githubusercontent.com/rplacetk/canvas1/main/place"
        },
        "canvas2": {
            "socket": "wss://server.poemanthology.org/ws",
            "http": "https://server.poemanthology.org/place"
        },
        "turkeycanvas": {
            "socket": "wss://server.poemanthology.org/turkeyws",
            "http": "https://server.poemanthology.org/turkeyplace"
        }
    }
}
```
Note:
 - mod_roles must be an array of strings due to parsing library limitations.