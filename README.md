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