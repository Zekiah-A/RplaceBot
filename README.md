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
    "max_mod_purge_per_hr": 300,
    "view_canvases": {
        "canvas1": {
            "socket": "wss://server.rplace.live:443",
            "http": "https://raw.githubusercontent.com/rplacetk/canvas1/main/place",
            "width": 1000,
            "height": 1000
        },
        "canvas2": {
            "socket": "wss://server.poemanthology.org/ws",
            "http": "https://server.poemanthology.org/place",
            "width": 500,
            "height": 500
        },
        "turkeycanvas": {
            "socket": "wss://server.poemanthology.org/turkeyws",
            "http": "https://server.poemanthology.org/turkeyplace",
            "width": 250,
            "height": 250
        }
    }
}
```
## Build instructions:
 - You will have to install concord separately unfortunately as the library itself
does not implement a cmakelists.txt to be compiled alongside this project as a gitmodule

 - You will also have to self compile CURL with websocket support if you receive 'curl_easy_perform() failed: Unsupported protocol'
error messages. To check if your cURL has support, run curl --version and check for ws/wss protocols present.

 - This project can be compiled easily with `gcc main.c lib/parson.c lib/parson.h -o RplaceBot -pthread -ldiscord -lcurl -lpng -lsqlite3 -Wall -Wextra -Wno-unused-parameter` and ran with ./RplaceBot.

 - This project can be debugged with flags `-Wall -Wextra -Wno-unused-parameter` / `-g -fsanitize=address` and gdb.

## Notes:
 - The parameter mod_roles in rplace_bot.json must be an array of strings due to parsing library limitations.
 - Bot data is stored in a SQLITE DB file called `rplace_bot.db`.
 - A helper script `edit-db.py` is provided to help with quickly editing the database.
It requires pip package `sqlite3` and `prompt_toolkit` the latter can be installed with
`python3 -m pip install prompt_toolkit`.
 - The command `sqlite3` can also be used to edit the bot database if you have it installed. It is reccomended you
use`.open rplace_bot.db` with `.mode box --wrap 50` for the best experience when viewing the database. More help
can be found at https://sqlite.org/cli.html.