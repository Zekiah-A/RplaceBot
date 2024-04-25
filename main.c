// RplaceBot (c) Zekiah-A
#include <bits/types/siginfo_t.h>
#include <concord/discord_codecs.h>
#include <stdio.h>
#include <stdlib.h>
#include <concord/discord.h>
#include <concord/log.h>
#include <curl/curl.h>
#include <png.h>
#include <string.h>
#include <sys/param.h>
#include <pthread.h>
#include <alloca.h>
#include <regex.h>
#include <sqlite3.h>
#include <signal.h>
#include <time.h>
#include "lib/parson.h"
#include "lib/telebot/include/telebot.h"
#include <errno.h>
#include <sys/stat.h>

struct memory_fetch {
    size_t size;
    uint8_t* memory;
    int error;
    const char* error_message;
};

struct rplace_config {
    struct view_canvas* view_canvases;
    int view_canvases_count;

    int max_hourly_mod_purge;

    u64snowflake* mod_roles;
    int mod_roles_count;
};

struct view_canvas {
    char* name;
    char* socket;
    char* http;
};

struct censor {
    u64snowflake member_id;
    time_t end_date;
};

struct parsed_timescale {
    int period_s;
    char* period_unit;
    int period_original;
};

struct period_timer_info {
    struct discord* client;
    timer_t* timer_id;
    char* http_root_url;
    u64snowflake channel_id;
};

#define GENERATION_ERROR_NONE 0
#define GENERATION_FAIL_METADATA 1
#define GENERATION_FAIL_FETCH 2
#define GENERATION_FAIL_DRAW 3

typedef uint8_t colour[3];

struct canvas_metadata {
    int width;
    int height;
    colour* palette;
    char palette_length;
    int error;
    char* error_msg;
};

struct downloaded_backup {
    int size;
    uint8_t* data;
    int error;
    char* error_msg;
};

struct canvas_image {
    int error;
    char* error_msg;
    int length;
    uint8_t* data;
};

struct censor* active_censors;
int active_censors_size = 0;
int active_censors_capacity = 0;

struct rplace_config* rplace_bot_config = NULL;

colour default_palette[32] = {
    {109, 0, 26},
    {190, 0, 57},
    {255, 69, 0},
    {255, 168, 0},
    {255, 214, 53},
    {255, 248, 184},
    {0, 163, 104},
    {0, 204, 120},
    {126, 237, 86},
    {0, 117, 111},
    {0, 158, 170},
    {0, 204, 192},
    {36, 80, 164},
    {54, 144, 234},
    {81, 233, 244},
    {73, 58, 193},
    {106, 92, 255},
    {148, 179, 255},
    {129, 30, 159},
    {180, 74, 192},
    {228, 171, 255},
    {222, 16, 127},
    {255, 56, 129},
    {255, 153, 170},
    {109, 72, 47},
    {156, 105, 38},
    {255, 180, 112},
    {0, 0, 0},
    {81, 82, 82},
    {137, 141, 144},
    {212, 215, 217},
    {255, 255, 255}
};

pthread_mutex_t fetch_lock;

sqlite3* bot_db;
struct discord* _discord_client;
telebot_handler_t _telegram_client;
pthread_t telegram_bot_thread;
int requested_sigint = 0;

void handle_sigint(int signum)
{
    if (requested_sigint)
    {
        printf("\rForce quitting!\n");
        abort();
    }
    if (signum == SIGINT)
    {
        requested_sigint = 1;
        printf("\nPerforming cleanup. Wait a sec!\n");
        sqlite3_close(bot_db);
        discord_shutdown(_discord_client);
    }
}

struct parsed_timescale parse_timescale(char* arg) {
    // We assume it is in seconds naturally
    int period_multiplier = 1;
    int period_original = 0;
    char* period_unit = "seconds"; 
    int period_len = strlen(arg);

    if (arg[period_len - 1] == 'm')
    {
        period_multiplier = 60;
        period_unit = "minutes";
    }
    else if (arg[period_len - 1] == 'h')
    {
        period_multiplier = 3600;
        period_unit = "hours";
    }
    else if (arg[period_len - 1] == 'd')
    {
        period_multiplier = 86400;
        period_unit = "days";
    }
    arg[period_len - 1] = '\0';
    period_original = atoi(arg);
    int period_s = period_original * period_multiplier;
    return (struct parsed_timescale) { period_s, period_unit, period_original };
}

void send_action_blocked(char* title, struct discord* client, const struct discord_message* event)
{
    struct discord_embed embed = {
        .title = title,
        .color = 0xFF4500,
        .footer = &(struct discord_embed_footer) {
            .text = "https://rplace.live, bot by Zekiah-A",
            .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"}};

    embed.description = "Sorry. You need moderator or higher permissions to be able to use this command!";
    embed.image = &(struct discord_embed_image) {
        .url = "https://media.tenor.com/qmSIzc-H7vIAAAAC/1984.gif" };

    struct discord_create_message params = {
        .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
    discord_create_message(client, event->channel_id, &params, NULL);
}

int check_member_has_mod(struct discord* client, u64snowflake guild_id, u64snowflake member_id)
{
    struct discord_guild_member guild_member = { .roles = NULL };
    struct discord_ret_guild_member guild_member_ret = {.sync = &guild_member};
    discord_get_guild_member(client, guild_id, member_id, &guild_member_ret);

    if (guild_member.roles == NULL || rplace_bot_config == NULL || rplace_bot_config->mod_roles == NULL)
    {
        return 0;
    }

    for (int i = 0; i < guild_member.roles->size; i++)
    {
        for (int j = 0; j < rplace_bot_config->mod_roles_count; j++)
        {
            if (guild_member.roles->array[i] == rplace_bot_config->mod_roles[j])
            {
                return 1;
            }
        }
    }

    return 0;
}

// Returns NULL if invalid channel
struct discord_channel* resolve_channel_mention(struct discord* client, const char* mention_string)
{
    int str_id_len = strlen(mention_string);
    char* str_id = NULL;

    // There should never be a member id this small in theory, but whatever
    if (str_id_len > 3 && mention_string[0] == '<' && mention_string[1] == '#')
    {
        int member_id_len = str_id_len - 3;
        str_id = malloc(member_id_len + 1);
        memcpy(str_id, mention_string + 2, member_id_len);
        str_id[member_id_len] = '\0';
    }
    else if (str_id_len > 0)
    {
        str_id = strdup(mention_string);
    }
    else
    {
        return NULL;
    }

    u64snowflake channel_id = strtoull(str_id, NULL, 10);
    free(str_id);

    if (channel_id == 0)
    {
        return NULL;
    }

    // TODO: Stack allocate this instead
    struct discord_channel* channel = malloc(sizeof(struct discord_channel));
    struct discord_ret_channel ret_channel = { .sync = channel };
    if (discord_get_channel(client, channel_id, &ret_channel) != CCORD_OK)
    {
        return NULL;
    }

    return channel;
}

// Returns NULL if invalid user
struct discord_user* resolve_user_mention(struct discord* client, const char* mention_string)
{
    int str_id_len = strlen(mention_string);
    char* str_id = NULL;

    // There should never be a member id this small in theory, but whatever
    if (str_id_len > 3 && mention_string[0] == '<' && mention_string[1] == '@')
    {
        int member_id_len = str_id_len - 3;
        str_id = malloc(member_id_len + 1);
        memcpy(str_id, mention_string + 2, member_id_len);
        str_id[member_id_len] = '\0';
    }
    else if (str_id_len > 0)
    {
        str_id = strdup(mention_string);
    }
    else
    {
        return NULL;
    }

    u64snowflake member_id = strtoull(str_id, NULL, 10);
    free(str_id);

    if (member_id == 0)
    {
        return NULL;
    }

    struct discord_user* user = malloc(sizeof(struct discord_user));
    struct discord_ret_user ret_user = { .sync = user };
    if (discord_get_user(client, member_id, &ret_user) != CCORD_OK)
    {
        return NULL;
    }

    return user;
}

void on_mod_help(struct discord* client, const struct discord_message* event)
{
    struct discord_embed embed = {
        .title = "Moderator commands",
        .color = 0xFF4500,
        .footer = &(struct discord_embed_footer) {
            .text = "https://rplace.live, bot by Zekiah-A",
            .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"}};

    struct discord_embed_image image_1984 = (struct discord_embed_image) {
            .url = "https://media.tenor.com/qmSIzc-H7vIAAAAC/1984.gif" };

    if (check_member_has_mod(client, event->guild_id, event->author->id))
    {
        embed.description = "**r/1984** `member` `period(s|m|h|d)` `reason`\nHide all the messages a user sends for a given period of time\n\n"
            "**r/archive** `canvas1/...` `channel` `period(m|h)` \n*Automatically send image backups of the the given board at a specified interval*\n\n"
            "**r/purge** `message count` `member (optional)`\nClear 'n' message history of a user, or of a channel (if no member_id, max: 100 messages per 2 hours)\n\n"
            "**r/modhistory**\nDisplay a history of all actively and past used moderation commands\n\n";
    }
    else
    {
        embed.description = "Sorry. You need moderator or higher permissions to be able to use this command!";
        embed.image = &image_1984;
    }

    struct discord_create_message params = {
        .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};

    discord_create_message(client, event->channel_id, &params, NULL);
}

void add_active_censor(u64snowflake member_id, time_t censor_end)
{
    active_censors_size++;
    if (active_censors_size > active_censors_capacity)
    {
        if (!active_censors_capacity)
        {
            active_censors_capacity++;
        }
        while (active_censors_size > active_censors_capacity)
        {
            active_censors_capacity *= 2;
        }

        active_censors = realloc(active_censors, active_censors_capacity * sizeof(struct censor));
    }

    active_censors[active_censors_size - 1] = (struct censor)
        { .member_id = member_id, .end_date = censor_end };
}

void on_1984(struct discord* client, const struct discord_message* event)
{
    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        send_action_blocked("Moderation action - 1984 user", client, event);
        return;
    }

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    if (arg == NULL)
    {
        on_mod_help(client, event);
        return;
    }

    struct discord_user* member = resolve_user_mention(client, arg);
    if (member == NULL)
    {
        struct discord_create_message params = { .content = 
            "Sorry. Can't find the user that you are trying to 1984. ¯\\_(ツ)_/¯" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
        
    if (check_member_has_mod(client, event->guild_id, member->id))
    {
        free(member);
        struct discord_create_message params = { .content = 
            "Sorry. You can not 1984 another moderator!" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    arg = strtok_r(NULL, " ", &count_state);
    if (arg == NULL)
    {
        free(member);
        on_mod_help(client, event);
        return;
    }
    struct parsed_timescale timescale = parse_timescale(arg);

    if (timescale.period_s > 31540000) // 365 days
    {
        free(member);
        struct discord_create_message params = { .content = 
            "Sorry. That 1984 is too massive! (Maximum 365 days)." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    else if (timescale.period_s <= 0)
    {
        free(member);
        struct discord_create_message params = { .content = 
            "You can't 1984 for that period of time? (Minimum 1 second)." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    char* reason = NULL;
    if (strlen(count_state) > 300)
    {
        count_state[300] = '\0';
    }
    reason = strdup(count_state);

    const char* query_existing_1984 = "SELECT * FROM CensorsHistory WHERE member_id = ?";
    sqlite3_stmt* existing_cmp_statement; // Compiled query statement
    if (sqlite3_prepare_v2(bot_db, query_existing_1984, -1, &existing_cmp_statement, 0) != SQLITE_OK)
    {
        free(member);
        fprintf(stderr, "Could not prepare existing censor: %s\n", sqlite3_errmsg(bot_db));
        
        struct discord_create_message params = { .content = 
            "Failed to 1984 user. Internal bot error occured :skull:" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    sqlite3_bind_int64(existing_cmp_statement, 1, member->id);

    // Existing, so we need to delete before making new (too lazy to update)
    if (sqlite3_step(existing_cmp_statement) == SQLITE_ROW)
    {
        char* query_delete_1984 = sqlite3_mprintf("DELETE FROM CensorsHistory WHERE member_id='%llu'", member->id);
        char* db_err_msg;
        int db_err = sqlite3_exec(bot_db, query_delete_1984, NULL, NULL, &db_err_msg);
        if (db_err != SQLITE_OK)
        {
            free(member);
            sqlite3_finalize(existing_cmp_statement);
            fprintf(stderr, "Could not remove existing censor: %s\n", db_err_msg);
            sqlite3_free(db_err_msg);

            struct discord_create_message params = { .content = 
                "Failed to 1984 user. Internal bot error occured :skull:" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
    }
    sqlite3_finalize(existing_cmp_statement);

    const char* query_insert_1984 = "INSERT INTO CensorsHistory (member_id, moderator_id, censor_start, censor_end, reason) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* insert_cmp_statement;
    time_t current_time = time(NULL); // Unix timestamp since epoch (s)

    if (sqlite3_prepare_v2(bot_db, query_insert_1984, -1, &insert_cmp_statement, 0) == SQLITE_OK)
    {
        sqlite3_bind_int64(insert_cmp_statement, 1, member->id);
        sqlite3_bind_int64(insert_cmp_statement, 2, event->author->id);
        sqlite3_bind_int64(insert_cmp_statement, 3, current_time);
        sqlite3_bind_int64(insert_cmp_statement, 4, current_time + timescale.period_s);
        sqlite3_bind_text(insert_cmp_statement, 5, reason, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insert_cmp_statement) != SQLITE_DONE)
        {
            free(member);
            fprintf(stderr, "Could not insert censor: %s\n", sqlite3_errmsg(bot_db));
            
            struct discord_create_message params = { .content = 
                "Failed to 1984 user. Internal bot error occured :skull:" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        sqlite3_finalize(insert_cmp_statement);
    }
    else
    {
        free(member);
        fprintf(stderr, "Could not prepare insert censor: %s\n", sqlite3_errmsg(bot_db));
        
        struct discord_create_message params = { .content = 
            "Failed to 1984 user. Internal bot error occured :skull:" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    add_active_censor(member->id, current_time + timescale.period_s);
    
    const char* raw_str_1984 = "Successfully 1984ed user **%s** for **%d %s** (reason: **%s**).";
    size_t str_1984_len = snprintf(NULL, 0, raw_str_1984, member->username, timescale.period_original, timescale.period_unit, reason) + 1;
    char* str_1984 = malloc(str_1984_len);
    snprintf(str_1984, str_1984_len, raw_str_1984, member->username, timescale.period_original, timescale.period_unit, reason);
    
    struct discord_create_message params = { .content = 
        str_1984 };
    discord_create_message(client, event->channel_id, &params, NULL);

    free(reason);
    free(str_1984);
}

void on_purge(struct discord* client, const struct discord_message* event)
{
    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        send_action_blocked("Moderation action - Purge messages", client, event);
        return;
    }

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    if (arg == NULL)
    {
        on_mod_help(client, event);
        return;
    }

    int count = atoi(arg);
    if (count < 1)
    {
        struct discord_create_message params = { .content =
            "Sorry. You can't purge less than one message!" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    arg = strtok_r(NULL, " ", &count_state);
    struct discord_user* member;

    if (arg != NULL)
    {
        member = resolve_user_mention(client, arg);
        if (member == NULL)
        {
            struct discord_create_message params = { .content =
                "Sorry. Can't find the user that you are trying to purge. (-_- )" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
    }

    // Block mods from abusing to purge insane message counts
    int moderator_hourly_purge = 0;
    const char* query_get_purges = "SELECT message_count FROM PurgesHistory WHERE moderator_id = ? AND purge_date > ?";
    sqlite3_stmt* get_cmp_statement;
    if (sqlite3_prepare_v2(bot_db, query_get_purges, -1, &get_cmp_statement, NULL) != SQLITE_OK)
    {
        if (member != NULL)
        {
            free(member);
        }
        fprintf(stderr, "Could not prepare get rate limit purges: %s\n", sqlite3_errmsg(bot_db));

        struct discord_create_message params = { .content =
            "Failed to purge messages. An internal bot error occured :skull:" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    time_t current_time = time(NULL); // time since unix epoch (s)

    sqlite3_bind_int64(get_cmp_statement, 1, event->author->id);
    sqlite3_bind_int64(get_cmp_statement, 2, current_time - 3600);

    int step = sqlite3_step(get_cmp_statement);
    while (step == SQLITE_ROW)
    {
        int message_count = sqlite3_column_int(get_cmp_statement, 1);
        if ((moderator_hourly_purge += message_count) > rplace_bot_config->max_hourly_mod_purge)
        {
            if (member != NULL)
            {
                free(member);
            }

            const char* raw_str_purge_cooldown = "Calm down! You can't purge more than %i messages within an hour.";
            size_t str_purge_cooldown_len = snprintf(NULL, 0, raw_str_purge_cooldown, rplace_bot_config->max_hourly_mod_purge) + 1;
            char* str_purge_cooldown = malloc(str_purge_cooldown_len);
            snprintf(str_purge_cooldown, str_purge_cooldown_len, raw_str_purge_cooldown, rplace_bot_config->max_hourly_mod_purge);
            
            struct discord_create_message params = { .content = str_purge_cooldown };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        step = sqlite3_step(get_cmp_statement);
    }

    if (step != SQLITE_DONE)
    {
        if (member != NULL)
        {
            free(member);
        }
        fprintf(stderr, "Could not get purges history: %s\n", sqlite3_errmsg(bot_db));

        struct discord_create_message params = { .content = 
            "Failed to purge messages. An internal bot error occured :skull:" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    sqlite3_finalize(get_cmp_statement);

    // Purge messages
    const char* raw_delete_reason = "Part of %i message purge issued by %s.";
    size_t delete_reason_len = snprintf(NULL, 0, raw_delete_reason, count, event->author->username) + 1;
    char* delete_reason = malloc(delete_reason_len);
    snprintf(delete_reason, delete_reason_len, raw_delete_reason, count, event->author->username);

    // Channel purge
    if (member == NULL)
    {
        struct discord_get_channel_messages params = { .limit = count };
        struct discord_messages messages = { };
        struct discord_ret_messages ret_messages = { .sync = &messages };
        if (discord_get_channel_messages(client, event->channel_id, &params, &ret_messages) != CCORD_OK)
        {
            fprintf(stderr, "Could not get purge guild channel messages: %s\n", sqlite3_errmsg(bot_db));
            
            struct discord_create_message params = { .content = 
                "Failed to purge user messages. Internal bot error occured :skull:" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        for (int message_i = 0; message_i < messages.size; message_i++)
        {
            if (messages.array[message_i].content != NULL)
            {
                struct discord_delete_message delete_info = { .reason = delete_reason };
                discord_delete_message(client, event->channel_id, messages.array[message_i].id, &delete_info, NULL);
            }
        }

        discord_messages_cleanup(&messages);
    }
    else // Member msg purge
    {
        struct discord_channels channels = { };
        struct discord_ret_channels ret_channels = { .sync = &channels };
        if (discord_get_guild_channels(client, event->guild_id, &ret_channels) != CCORD_OK)
        {
            free(member);
            fprintf(stderr, "Could not get purge guild channels: %s\n", sqlite3_errmsg(bot_db));

            struct discord_create_message params = { .content = 
                "Failed to purge messages. Internal bot error occured :skull:" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        struct discord_get_channel_messages params = { .limit = count };
        for (int channel_i = 0; channel_i < channels.size; channel_i++)
        {
            // Initially get most recent messages
            params.before = 0;
    
            int message_i = 0;
            while (message_i < params.limit)
            {
                struct discord_messages messages = { };
                struct discord_ret_messages ret_messages = { .sync = &messages };
                if (discord_get_channel_messages(client, channels.array[channel_i].id, &params, &ret_messages) != CCORD_OK)
                {
                    fprintf(stderr, "Could not get purge guild channel messages: %s\n", sqlite3_errmsg(bot_db));
                    continue;
                }

                for (message_i = 0; message_i < messages.size; message_i++)
                {
                    struct discord_delete_message delete_info = { .reason = delete_reason };
                    discord_delete_message(client, channels.array[channel_i].id, messages.array[message_i].id, &delete_info, NULL);
                }

                if (message_i != 0)
                {
                    params.before = messages.array[message_i - 1].id;
                }

                discord_messages_cleanup(&messages);
            }
        }
    
        discord_channels_cleanup(&channels);
    }

    // Update database history with latest purge
    const char* query_inset_purge = "INSERT INTO PurgesHistory (member_id, channel_id, moderator_id, message_count, purge_date) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* insert_cmp_statement;

    if (sqlite3_prepare_v2(bot_db, query_inset_purge, -1, &insert_cmp_statement, NULL) != SQLITE_OK)
    {
        if (member != NULL)
        {
            free(member);
        }

        fprintf(stderr, "Could not prepare insert purge: %s\n", sqlite3_errmsg(bot_db));
        return;
    }

    if (member == NULL)
    {
        sqlite3_bind_null(insert_cmp_statement, 1);
        sqlite3_bind_int64(insert_cmp_statement, 2, event->channel_id);
    }
    else
    {
        sqlite3_bind_int64(insert_cmp_statement, 1, member->id);   
        sqlite3_bind_null(insert_cmp_statement, 2);
    }
    sqlite3_bind_int64(insert_cmp_statement, 3, event->author->id);
    sqlite3_bind_int(insert_cmp_statement, 4, count);
    sqlite3_bind_int64(insert_cmp_statement, 5, current_time);
    
    if (sqlite3_step(insert_cmp_statement) != SQLITE_DONE)
    {
        fprintf(stderr, "Could not insert latest purge to history: %s\n", sqlite3_errmsg(bot_db));
    }

    if (member != NULL)
    {
        free(member);
    }
}

void ensure_tables_capacity(char** tables, int* tables_used, int* tables_len, int new_table_len)
{
    (*tables_used) += new_table_len;
    if (*tables_used > *tables_len)
    {
        int realloc_size = *tables_len;
        while (realloc_size < *tables_used)
        {
            realloc_size *= 2;
        }

        (*tables) = realloc(*tables, realloc_size);
        *tables_len = realloc_size;
    }
}

void on_mod_history(struct discord* client, const struct discord_message* event)
{
    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        send_action_blocked("Moderation action - View history", client, event);
        return;
    }

    const char* tables_censors_title = "**__Censors history:__**\n"; 
    const char* tables_purges_title = "\n**__Purges history:__**\n";

    int tables_len = strlen(tables_censors_title) + strlen(tables_purges_title) + 1;
    int tables_used = tables_len;
    char* tables = malloc(tables_len);
    strcpy(tables, tables_censors_title);

    const char* query_get_censors = "SELECT * FROM CensorsHistory;";
    sqlite3_stmt* censors_cmp_statement;
    int db_err = sqlite3_prepare_v2(bot_db, query_get_censors, -1, &censors_cmp_statement, NULL);

    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "Could not prepare get censors: %s\n", sqlite3_errmsg(bot_db));
        return;
    }

    while (sqlite3_step(censors_cmp_statement) == SQLITE_ROW)
    {
        const uint64_t start_date_i = sqlite3_column_int64(censors_cmp_statement, 2);
        const uint64_t int_member_id = sqlite3_column_int64(censors_cmp_statement, 0);
        const unsigned char* str_member_id = sqlite3_column_text(censors_cmp_statement, 0);
        const uint64_t int_moderator_id = sqlite3_column_int64(censors_cmp_statement, 1);
        const unsigned char* str_moderator_id = sqlite3_column_text(censors_cmp_statement, 1);
        const uint64_t end_date_i = sqlite3_column_int64(censors_cmp_statement, 3);
        const unsigned char* reason = sqlite3_column_text(censors_cmp_statement, 4);

        char start_date[32];
        struct tm* start_date_t = localtime(&start_date_i);
        size_t sd_s = strftime(start_date, sizeof(start_date), "%d/%m/%Y %H:%M", start_date_t);
        start_date[sd_s] = '\0';

        char end_date[32];
        struct tm* end_date_t = localtime(&end_date_i);
        size_t ed_s = strftime(end_date, sizeof(end_date), "%d/%m/%Y %H:%M", end_date_t);
        end_date[ed_s] = '\0';

        const char* member_name = NULL;
        struct discord_user* member = malloc(sizeof(struct discord_user));
        struct discord_ret_user ret_member = { .sync = member };
        if (discord_get_user(client, int_member_id, &ret_member) == CCORD_OK)
        {
            member_name = member->username;
        }
        if (member_name == NULL)
        {
            member_name = str_member_id;
        }

        const char* mod_name = NULL;
        struct discord_user* mod = malloc(sizeof(struct discord_user));
        struct discord_ret_user ret_mod = { .sync = mod };
        if (discord_get_user(client, int_moderator_id, &ret_mod) == CCORD_OK)
        {
            mod_name = mod->username;
        }
        if (mod_name == NULL)
        {
            mod_name = str_moderator_id;
        }

        const char* raw_new_table = "**Start date:** %s\n \
            **Member:** %s\n \
            **Moderator:** %s\n \
            **End date:** %s\n \
            **Reason:** %s\n \
            ------------------------\n";
        const char* raw_new_table_active = "**Start date:** %s\n \
            **Member:** %s\n \
            **Moderator:** %s\n \
            **End date:** %s ✅ _currently active_\n \
            **Reason:** %s\n \
            ------------------------\n";
        const char* selected_new_table = end_date_i > time(NULL) ? raw_new_table_active : raw_new_table;

        size_t new_table_len = snprintf(NULL, 0, selected_new_table, start_date, member_name, mod_name, end_date, reason);
        char* new_table = malloc(new_table_len + 1);
        snprintf(new_table, new_table_len, selected_new_table, start_date, member_name, mod_name, end_date, reason);

        ensure_tables_capacity(&tables, &tables_used, &tables_len, new_table_len);
        strcat(tables, new_table);
    }
    sqlite3_finalize(censors_cmp_statement);
    strcat(tables, tables_purges_title);

    const char* query_get_purges = "SELECT * FROM PurgesHistory;";
    sqlite3_stmt* purges_cmp_statement;
    db_err = sqlite3_prepare_v2(bot_db, query_get_purges, -1, &purges_cmp_statement, NULL);

    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "Could not prepare get censors: %s\n", sqlite3_errmsg(bot_db));
        return;
    }

    while (sqlite3_step(purges_cmp_statement) == SQLITE_ROW)
    {
        const char* str_moderator_id = sqlite3_column_text(purges_cmp_statement, 2);
        const uint64_t int_moderator_id = sqlite3_column_int64(purges_cmp_statement, 2);
        const int message_count = sqlite3_column_int(purges_cmp_statement, 3);
        const uint64_t purge_date_i = sqlite3_column_int64(purges_cmp_statement, 4);

        char purge_date[32];
        struct tm* purge_date_t = localtime(&purge_date_i);
        size_t sd_s = strftime(purge_date, sizeof(purge_date), "%d/%m/%Y %H:%M", purge_date_t);
        purge_date[sd_s] = '\0';

        char* raw_new_table = NULL;
        size_t new_table_len = 0;
        char* new_table = NULL;

        const char* mod_name = NULL;
        struct discord_user* mod = malloc(sizeof(struct discord_user)); // TODO: These do not need to be malloced!
        struct discord_ret_user ret_mod = { .sync = mod };
        if (discord_get_user(client, int_moderator_id, &ret_mod) == CCORD_OK)
        {
            mod_name = mod->username;
        }
        if (mod_name == NULL)
        {
            mod_name = str_moderator_id;
        }

        if (sqlite3_column_type(purges_cmp_statement, 0) != SQLITE_NULL)
        {
            const uint64_t int_member_id = sqlite3_column_int64(purges_cmp_statement, 0);
            const char* str_member_id = sqlite3_column_text(purges_cmp_statement, 0);

            const char* member_name = NULL;
            struct discord_user* member = malloc(sizeof(struct discord_user)); // TODO: These do not need to be malloced!
            struct discord_ret_user ret_member = { .sync = member };
            if (discord_get_user(client, int_member_id, &ret_member) == CCORD_OK)
            {
                member_name = member->username;
            }
            if (member_name == NULL)
            {
                member_name = str_member_id;
            }

            raw_new_table = "**Type:** Member purge\n \
                **Member:** %s\n \
                **Moderator:** %s\n \
                **Purge date:** %s\n \
                **Message count:** %d\n \
                ------------------------\n";
            new_table_len = snprintf(NULL, 0, raw_new_table, member_name, mod_name, purge_date, message_count) + 1;
            new_table = malloc(new_table_len);
            snprintf(new_table, new_table_len, raw_new_table, member_name, mod_name, purge_date, message_count);
        }
        else
        {
            const uint64_t int_channel_id = sqlite3_column_int64(purges_cmp_statement, 1);
            raw_new_table = "**Type:** Channel purge\n \
                **Channel:** <#%llu>\n \
                **Moderator:** %s\n \
                **Purge date:** %s\n \
                **Message count:** %d\n \
                ------------------------\n";
            new_table_len = snprintf(NULL, 0, raw_new_table, int_channel_id, mod_name, purge_date, message_count) + 1;
            new_table = malloc(new_table_len);
            snprintf(new_table, new_table_len, raw_new_table, int_channel_id, mod_name, purge_date, message_count);
        }

        ensure_tables_capacity(&tables, &tables_used, &tables_len, new_table_len);
        strcat(tables, new_table);
    }
    sqlite3_finalize(purges_cmp_statement);

    struct discord_create_message params = { .content = 
        tables };
    discord_create_message(client, event->channel_id, &params, NULL);
}

void on_help(struct discord* client, const struct discord_message* event)
{
    struct discord_embed embed = {
        .title = "Commands",
        .description =
            "**r/view** `canvas1/...` `x` `y` `width` `height` `z` \n*Create an image from a region of the canvas*\n\n"
            "**r/help**, **r/?**, **r/** \n*Displays information about this bot*\n\n"
            "**r/status** `canvas1/...` \n*Displays if the provided canvas is online or not*\n\n"
            "**r/modhelp** \n*Displays help information for moderator actions*\n\n",
        .color = 0xFF4500,
        .footer = &(struct discord_embed_footer){
            .text = "https://rplace.live, bot by Zekiah-A",
            .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"}};

    struct discord_create_message params = {
        .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};

    discord_create_message(client, event->channel_id, &params, NULL);
}

size_t fetch_memory_callback(void* contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct memory_fetch* fetch = (struct memory_fetch*) userp;

    void* new_memory = realloc(fetch->memory, fetch->size + realsize);
    if (new_memory == NULL)
    {
        fetch->size = 0;
        fprintf(stderr, "Not enough memory to continue fetch (realloc returned NULL)\n");
        return 0;
    }
    fetch->memory = new_memory;

    memcpy(&(fetch->memory[fetch->size]), contents, realsize);
    fetch->size += realsize;

    return realsize;
}

struct memory_fetch fetch(char* url)
{
    pthread_mutex_lock(&fetch_lock);
    CURL* curl = curl_easy_init();
    CURLcode result;
    struct memory_fetch chunk = { };
    chunk.memory = malloc(1);
    chunk.size = 0;
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK)
    {
        if (chunk.memory != NULL)
        {
            free(chunk.memory);
        }
        chunk.error = 1;
        chunk.error_message = curl_easy_strerror(result);
        return chunk;
    }
    pthread_mutex_unlock(&fetch_lock);
    return chunk;
}

struct canvas_metadata download_canvas_metadata(char* metadata_url)
{
    struct canvas_metadata metadata = { };
    struct memory_fetch metadata_response = fetch(metadata_url);

    if (metadata_response.error)
    {
        fprintf(stderr, "Error fetching file: %s\n", metadata_response.error_message);
        metadata.error = GENERATION_FAIL_METADATA;
        metadata.error_msg = "Sorry, an unexpected network error occurred and I can't fetch that canvas, "
             "please try again later.";
        return metadata;
    }

    char* json_string = malloc(metadata_response.size + 1);
    memcpy(json_string, metadata_response.memory, metadata_response.size);
    json_string[metadata_response.size] = '\0';

    JSON_Value* root = json_parse_string(json_string);
    JSON_Object* root_obj = json_value_get_object(root);
    JSON_Value* palette_value = json_object_get_value(root_obj, "palette");
    JSON_Array* palette_array = json_value_get_array(palette_value);
    char palette_length = (char) json_array_get_count(palette_array);
    metadata.palette_length = palette_length;
    metadata.palette = malloc(3 * palette_length);
    for (int i = 0; i < palette_length; i++)
    {
        uint32_t colour_int = (uint32_t) json_array_get_number(palette_array, i);
        metadata.palette[i][0] = (uint8_t)(colour_int >> 24);
        metadata.palette[i][1] = (uint8_t)(colour_int >> 16);
        metadata.palette[i][2] = (uint8_t)(colour_int >> 8);
    }
    JSON_Value* width_value = json_object_get_value(root_obj, "width");
    metadata.width = (int) json_value_get_number(width_value);
    JSON_Value* height_value = json_object_get_value(root_obj, "height");
    metadata.height = (int) json_value_get_number(height_value);

    return metadata;
}

struct downloaded_backup download_canvas_backup(char* canvas_url)
{
    // TODO: Use fetch instead
    struct downloaded_backup download_result = { .error = GENERATION_ERROR_NONE }; 
    pthread_mutex_lock(&fetch_lock);
    CURL* curl = curl_easy_init();
    CURLcode result;
    struct memory_fetch chunk = { };
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curl, CURLOPT_URL, canvas_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK)
    {
        if (chunk.memory != NULL)
        {
            free(chunk.memory);
        }

        fprintf(stderr, "Error fetching canvas backup: %s\n", curl_easy_strerror(result));
        download_result.error = GENERATION_FAIL_FETCH;
        download_result.error_msg = "Sorry, an unexpected network error occurred and I can't fetch that canvas, "
            "please try again later.";
    }

    download_result.data = (uint8_t*) chunk.memory;
    download_result.size = chunk.size;
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&fetch_lock);
    return download_result;
}

struct downloaded_backup rle_decode_board(int canvas_width, int canvas_height, uint8_t** ref_board, int* size)
{
    // Then this is a new format (RLE encoded) board that must be decoded
    int decoded_size = canvas_width * canvas_height;
    uint8_t* board = *ref_board; 
    uint8_t* decoded_board = malloc(decoded_size);
    int boardI = 0;
    uint8_t colour = 0;

    for (int i = 0; i < *size; i++)
    {
        // Then it is a palette value
        if (i % 2 == 0)
        {
            colour = board[i];
            continue;
        }
        // After the board_colour, we koop until we unpack all repeats, since we never have zero
        // repeats, we use 0 as 1 so we treat everything as i + 1 repeats.
        for (int j = 0; j < board[i] + 1; j++)
        {
            decoded_board[boardI] = colour;
            boardI++;
        }
    }

    free(board);
    *ref_board = decoded_board;
    *size = decoded_size;
}

struct region_info {
    int start_x;
    int start_y;
    int scale;
};

struct canvas_image generate_canvas_image(int canvas_width, int canvas_height, struct region_info region, uint8_t* board, int size, colour* palette)
{
    struct canvas_image gen_result = { };
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL)
    {
        gen_result.error = GENERATION_FAIL_DRAW;
        gen_result.error_msg = "Sorry, an unexpected drawing error ocurred and I can't create an image of that canvas, "
            "please try again later.";
        png_destroy_write_struct(&png_ptr, NULL);
        return gen_result;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL)
    {
        gen_result.error = GENERATION_FAIL_DRAW;
        gen_result.error_msg = "Sorry, an unexpected drawing error ocurred and I can't create an image of that canvas, "
            "please try again later.";
        // Cleanup resources
        png_destroy_write_struct(&png_ptr, NULL);
        return gen_result;
    }

    char* stream_buffer = NULL;
    size_t stream_length = 0;
    FILE* memory_stream = open_memstream(&stream_buffer, &stream_length);
    if (!region.scale)
    {
        // Ensure default value
        region.scale = 1;
    }
    int region_width = canvas_width - 1 - region.start_x;
    int region_height = canvas_height - 1 - region.start_y;
    int region_scaled_width = region_width * region.scale;
    int region_scaled_height = region_height * region.scale;

    png_init_io(png_ptr, memory_stream);
    png_set_IHDR(png_ptr, info_ptr, region_scaled_width, region_scaled_height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    png_bytep row_pointers[region_scaled_height]; // 2D ptr array

    for (int i = 0; i < region_scaled_height; i++)
    {
        row_pointers[i] = (png_bytep) malloc(3 * region_scaled_width);
    }

    int i = canvas_width * region.start_y + region.start_x;
    while (i < canvas_width * canvas_height)
    {
        // Copy over board_colour to image
        int x = i / canvas_width - region.start_y;       // image x (assuming image scale 1:1 with canvas)
        int y = 3 * (i % canvas_width - region.start_x); // image y (assuming image scale 1:1 with canvas + accounting for 3 byte board_colour)

        if (region.scale == 1)
        {
            uint8_t* position = &row_pointers[x][y];

            for (int p = 0; p < 3; p++)
            {
                position[p] = palette[board[i]][p]; // board_colour
            }
        }
        else
        {
            for (int sx = 0; sx < region.scale; sx++)
            {
                for (int sy = 0; sy < region.scale; sy++)
                {
                    uint8_t* position = &row_pointers
                        [x * region.scale + sx]      // We project X to upscaled X position
                        [y * region.scale + sy * 3]; // We project Y to upscaled Y position

                    for (int p = 0; p < 3; p++)
                    {
                        position[p] = palette[board[i]][p]; // board_colour
                    }
                }
            }
        }
        i++;

        // If we exceed width, go to next row, otherwise keep drawing on this row
        if (i % canvas_width < region.start_x + region_width)
        {
            continue;
        }

        // If we exceed end bottom, we are done drawing this
        if (i / canvas_width >= region.start_y + region_height - 1)
        {
            break;
        }

        i += canvas_width - region_width;
    }

    png_write_image(png_ptr, row_pointers);

    for (int i = 0; i < region_scaled_height; i++)
    {
        free(row_pointers[i]);
    }
    
    png_write_end(png_ptr, NULL);
    fflush(memory_stream);
    fclose(memory_stream);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    
    gen_result.data = (uint8_t*) stream_buffer;
    gen_result.length = stream_length;
    return gen_result;
}

void on_canvas_mention(struct discord* client, const struct discord_message* event)
{
    if (event->author->bot) {
        return;
    }

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    if (arg == NULL)
    {
        on_help(client, event);
        return;
    }
    char* canvas_name = arg;
    char* http_root_url = NULL;

    // Read parameters from message
    for (int i = 0; i < rplace_bot_config->view_canvases_count; i++)
    {
        struct view_canvas view_canvas = rplace_bot_config->view_canvases[i];

        if (strcmp(arg, view_canvas.name) == 0)
        {
            http_root_url = view_canvas.http;
            break;
        }
    }

    if (http_root_url == NULL)
    {
        struct discord_create_message params = {.content =
            "At the moment, custom canvases URLs are not supported.\n"
            "Format: r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`\n"
            "Try: r/view canvas1 10 10 100 100 2x"};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    // Fetch board metadata
    char* metadata_url = strcat(http_root_url, "/metadata.json");
    struct canvas_metadata metadata = download_canvas_metadata(metadata_url);
    if (metadata.error)
    {
        // TODO: Free stuff
        struct discord_create_message params = { .content = metadata.error_msg };
        discord_create_message(client, event->channel_id, &params, NULL);
        free(metadata_url);
        return;
    }
    free(metadata_url);

    int start_x = 0;
    int start_y = 0;
    int region_width = metadata.width - 1;
    int region_height = metadata.height - 1;
    int scale = 1;

    // No arguments is allowed as it will just do a 1:1 full canvas preview
    arg = strtok_r(NULL, " ", &count_state);
    if (arg != NULL)
    {
        start_x = MAX(0, MIN(metadata.width - 1, atoi(arg)));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL) {
            struct discord_create_message params = { .content =
                "Start Y argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        start_y = MAX(0, MIN(metadata.height - 1, atoi(arg)));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL)
        {
            struct discord_create_message params = { .content =
                "Width argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        region_width = MIN(metadata.width - 1 - start_x, atoi(arg));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL)
        {
            struct discord_create_message params = { .content =
                "Height argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`"};
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        region_height = MIN(metadata.height - 1 - start_y, atoi(arg));

        if (region_width <= 0 || region_height <= 0)
        {
            struct discord_create_message params = { .content =
                "Height or width can not be zero, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`"};
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        arg = strtok_r(NULL, " ", &count_state);
        if (arg != NULL)
        {
            int len = strlen(arg);
            if (arg[len - 1] == 'x')
            {
                arg[len - 1] = '\0';
            }

            scale = MAX(1, MIN(10, atoi(arg)));
        }
    }
    // Reassure client that we have stared before we do any heavy lifting
    discord_create_reaction(client, event->channel_id, event->id,
        0, "✅", NULL);

    // Fetch decode board
    char* canvas_url = strcat(http_root_url, "/place");
    struct downloaded_backup backup = download_canvas_backup(canvas_url);
    if (backup.error)
    {
        struct discord_create_message params = { .content = backup.error_msg };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    if (backup.size < metadata.width * metadata.height)
    {
        // It is likely a RLE compressed new format board
        rle_decode_board(metadata.width, metadata.height, &backup.data, &backup.size);
    }
    struct region_info region = {
        .scale = scale,
        .start_x = start_x,
        .start_y = start_y
    };
    struct canvas_image canvas_image = generate_canvas_image(metadata.width, metadata.height, region, backup.data, backup.size, metadata.palette);
    free(backup.data);
    if (canvas_image.error)
    {
        struct discord_create_message params = { .content = backup.error_msg };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    // At minimum, may be "Image at 65535 65535 on ``, source: " (length 36 + 1 (\0))
    const char* raw_response_str = "Image at %d %d on `%s`, source: %s";
    size_t max_response_length = snprintf(NULL, 0, raw_response_str, start_x, start_y, canvas_name, canvas_url);
    char* response = alloca(max_response_length);
    snprintf(response, max_response_length, raw_response_str, start_x, start_y, canvas_name, canvas_url);
    free(canvas_url);

    struct discord_create_message params = {
        .content = response,
        .attachments = &(struct discord_attachments){
            .size = 1,
            .array = &(struct discord_attachment){
                .content = (char*) canvas_image.data,
                .size = canvas_image.length,
                .filename = "place.png" },
        }};
    discord_create_message(client, event->channel_id, &params, NULL);
    free(canvas_image.data);
}

// Allows the bot user to define a channel wherein the bot shall send periodic archives to
// this method is called on an interval specified by end user
void send_periodic_archive(int sig_no, siginfo_t* sig_info, void* unused_data)
{
    struct period_timer_info* archive_info = ((struct period_timer_info*) sig_info->si_value.sival_ptr);
    
    // If channel doesn't exist anymore, reject rendering backup, remove from timer loop and SQLite DB
    struct discord_channel channel = { };
    struct discord_ret_channel ret_channel = { .sync = &channel };
    if (discord_get_channel(archive_info->client, archive_info->channel_id, &ret_channel) != CCORD_OK)
    {
        const char* delete_periodic_query = "DELETE FROM PeriodicArchives WHERE channel_id = ? AND http_root_url = ?;";
        sqlite3_stmt* delete_cmp_statement;
        
        int db_err = sqlite3_prepare_v2(bot_db, delete_periodic_query, -1, &delete_cmp_statement, NULL);
        if (db_err != SQLITE_OK)
        {
            fprintf(stderr, "Could not prepare delete periodic archive: %s\n", sqlite3_errmsg(bot_db));
        }
        else
        {
            sqlite3_bind_int64(delete_cmp_statement, 1, archive_info->channel_id);
            sqlite3_bind_text(delete_cmp_statement, 2, archive_info->http_root_url, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(delete_cmp_statement) != SQLITE_DONE)
            {
                fprintf(stderr, "Could not delete periodic archive: %s\n", sqlite3_errmsg(bot_db));
            }
            sqlite3_finalize(delete_cmp_statement);
        }

        timer_delete(archive_info->timer_id);
        free(archive_info->timer_id);
        free(archive_info);
        return;
    }

    char* metadata_url = strcat(archive_info->http_root_url, "/metadata.json");
    struct canvas_metadata metadata = download_canvas_metadata(metadata_url);
    if (metadata.error)
    {
        // TODO: Free stuff
        fprintf(stderr, "Failed to create periodic canvas backup in channel %llu."
            "Metadata fetch failed with error code %i: %s\n",
            (unsigned long long) archive_info->channel_id, metadata.error, metadata.error_msg);
        free(metadata_url);
        return;
    }
    free(metadata_url);

    // Fetch decode board
    struct downloaded_backup backup = download_canvas_backup(archive_info->http_root_url);
    if (backup.error)
    {
        fprintf(stderr, "Failed to create periodic canvas backup in channel %llu."
            "Fetch failed with error code %i: %s\n",
            (unsigned long long) archive_info->channel_id, backup.error, backup.error_msg);
        return;
    }
    struct region_info region = {
        .scale = 1,
        .start_x = 0,
        .start_y = 0
    };

    struct canvas_image canvas_image = generate_canvas_image(500, 500, region, backup.data, backup.size, default_palette);
    free(backup.data);
    if (canvas_image.error)
    {
        fprintf(stderr, "Failed to create periodic canvas backup in channel %llu. Fetch failed with error code %i: %s\n",
            (unsigned long long) archive_info->channel_id, backup.error, backup.error_msg);
        return;
    }

    int max_content_length = strlen(archive_info->http_root_url) + 48;
    char* message_content = alloca(max_content_length + 1);
    snprintf(message_content, max_content_length, ":alarm_clock:  Automatic canvas backup. Source %s",
        archive_info->http_root_url);
    struct discord_create_message params = {
        .content = message_content,
        .attachments = &(struct discord_attachments){
            .size = 1,
            .array = &(struct discord_attachment){
                .content = (char*) canvas_image.data,
                .size = canvas_image.length,
                .filename = "place.png"},
        }};
    discord_create_message(archive_info->client, archive_info->channel_id, &params, NULL);
    free(canvas_image.data);
}

void create_periodic_archive(struct discord* client, char* canvas_url, int period_s, u64snowflake channel_id)
{
    // Signal handler
    struct sigaction sa = {
        .sa_flags = SA_SIGINFO,
        .sa_sigaction = &send_periodic_archive,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN, &sa, NULL);

    // Set up timer
    // canvas_url : timer id
    timer_t* timer_id = malloc(sizeof(timer_t));
    struct period_timer_info* handler_info = malloc(sizeof(struct period_timer_info));
    handler_info->client = client;
    handler_info->timer_id = timer_id;
    handler_info->http_root_url = canvas_url;
    handler_info->channel_id = channel_id;

    struct sigevent sev = {
        .sigev_notify = SIGEV_SIGNAL,
        .sigev_signo = SIGRTMIN,
        .sigev_value.sival_ptr = handler_info
    };
    timer_create(CLOCK_REALTIME, &sev, timer_id);

    // TODO: If bot restarts are frequent, then save last send in DB and set
    // TODO: it_value to a value that maintains interval seamlessly
    struct itimerspec its = {
        .it_interval = { .tv_sec = period_s, .tv_nsec = 0 },
        .it_value = { .tv_sec = 1  }
    };
    timer_settime(*timer_id, 0, &its, NULL);  
}

void start_all_periodic_archives(struct discord* client)
{
    const char* query_get_archives = "SELECT * FROM PeriodicArchives;";
    sqlite3_stmt* archives_cmp_statement;
    int db_err = sqlite3_prepare_v2(bot_db, query_get_archives, -1, &archives_cmp_statement, NULL);

    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "Could not prepare get PeriodicArchives: %s\n", sqlite3_errmsg(bot_db));
        return;
    }

    while (sqlite3_step(archives_cmp_statement) == SQLITE_ROW)
    {
        const uint64_t channel_id = sqlite3_column_int64(archives_cmp_statement, 0);
        const int period_s = sqlite3_column_int(archives_cmp_statement, 1);
        const unsigned char* http_root_url_text = sqlite3_column_text(archives_cmp_statement, 2);


        char* http_root_url = strdup((char*) http_root_url_text);
        create_periodic_archive(client, http_root_url, period_s, channel_id);
    }
    sqlite3_finalize(archives_cmp_statement);
}

void on_archive(struct discord* client, const struct discord_message* event)
{
    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        send_action_blocked("Moderation action - Automatic canvas archives", client, event);
        return;
    }

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    
    char* canvas_name = arg;
    if (canvas_name == NULL)
    {
        on_mod_help(client, event);
        return;
    }

    char* http_root_url = NULL;
    char* metadata_url = NULL;
    for (int i = 0; i < rplace_bot_config->view_canvases_count; i++)
    {
        struct view_canvas view_canvas = rplace_bot_config->view_canvases[i];
        if (strcmp(canvas_name, view_canvas.name) == 0)
        {
            http_root_url = view_canvas.http;
            break;
        }
    }
    if (http_root_url == NULL)
    {
        // inbuilt_canvas = 0;
        struct discord_create_message params = { .content =
            "Sorry. At the moment, custom canvases URLs are not supported." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    arg = strtok_r(NULL, " ", &count_state);
    char* channel_name = arg;
    if (channel_name == NULL)
    {
        on_mod_help(client, event);
        return;
    }
    struct discord_channel* channel = resolve_channel_mention(client, channel_name);
    if (channel == NULL)
    {
        struct discord_create_message params = { .content =
            "Could not create automatic canvas archives. Specified channel could not be found." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    // Resolve channel
    arg = strtok_r(NULL, " ", &count_state);
    if (arg == NULL)
    {
        on_mod_help(client, event);
        return;
    }
    struct parsed_timescale timescale = parse_timescale(arg);
    if (timescale.period_s < 300 || timescale.period_s > 172800)
    {
        const char* warning_template = "Sorry. Canvas archive periods must be between **5 minutes** and **2 days**, "
            "but you specified %i %s. Please try again.";
        int warning_len = snprintf(NULL, 0, warning_template, timescale.period_original, timescale.period_unit) + 1;
        char* warning = malloc(warning_len);
        snprintf(warning, warning_len, warning_template, timescale.period_original, timescale.period_unit);
        struct discord_create_message params = { .content = warning };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    const char* query_inset_purge = "INSERT INTO PeriodicArchives (channel_id, period_s, board_url, metadata_url) VALUES (?, ?, ?)";
    sqlite3_stmt* insert_cmp_statement;

    if (sqlite3_prepare_v2(bot_db, query_inset_purge, -1, &insert_cmp_statement, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Could not prepare insert periodic archive: %s\n", sqlite3_errmsg(bot_db));
        struct discord_create_message params = { .content =
            "Could not create automatic canvas archives. Internal bot error occurred :skull:" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    sqlite3_bind_int64(insert_cmp_statement, 1, channel->id);   
    sqlite3_bind_int(insert_cmp_statement, 2, timescale.period_s);
    sqlite3_bind_text(insert_cmp_statement, 3, http_root_url, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(insert_cmp_statement) != SQLITE_DONE)
    {
        fprintf(stderr, "Could not insert periodic archive: %s\n", sqlite3_errmsg(bot_db));
        struct discord_create_message params = { .content =
            "Could not create automatic canvas archives. Internal bot error occurred :skull:" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    create_periodic_archive(client, http_root_url, timescale.period_s, channel->id);

    const char* success_template = "Successfully set up automatic canvas archives at interval *%i %s* in channel %s.";
    int success_len = snprintf(NULL, 0, success_template, timescale.period_original, timescale.period_unit, channel_name) + 1;
    char* success = malloc(success_len);
    snprintf(success, success_len, success_template, timescale.period_original, timescale.period_unit, channel_name);
    struct discord_create_message params = { .content = success };
    discord_create_message(client, event->channel_id, &params, NULL);
}

void on_status(struct discord* client, const struct discord_message* event)
{
    char* count_state = NULL;
    char* canvas_name = strtok_r(event->content, " ", &count_state);
    char* ws_url = NULL;
    char online = 0;
    char inbuilt_canvas = 1;
    if (canvas_name == NULL)
    {
        on_help(client, event);
        return;
    }

    for (int i = 0; i < rplace_bot_config->view_canvases_count; i++)
    {
        struct view_canvas view_canvas = rplace_bot_config->view_canvases[i];

        if (strcmp(canvas_name, view_canvas.name) == 0)
        {
            ws_url = view_canvas.socket;
            break;
        }
    }
    if (ws_url == NULL)
    {
        // inbuilt_canvas = 0;
        struct discord_create_message params = {.content =
            "At the moment, custom canvases URLs are not supported.\n"
            "Format: r/status `canvas1/canvas2/turkeycanvas/...`\n"
            "Try: r/status canvas1"};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    pthread_mutex_lock(&fetch_lock);
    CURL* curl = curl_easy_init();
    CURLcode result;

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Origin: https://rplace.live");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, ws_url);
    curl_easy_setopt(curl, CURLOPT_WS_OPTIONS, CURLWS_RAW_MODE);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);

    result = curl_easy_perform(curl);
    online = result == CURLE_OK;

    if (result != CURLE_OK)
    {
        online = 0;
        log_error("Status grab failed: curl_easy_perform() %s", curl_easy_strerror(result));
    }
    else
    {
        long close_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &close_code);
        if (close_code != 0)
        {
            online = 0;
        }
    }

    char* response = NULL;
    if (inbuilt_canvas)
    {
        int max_response_length = strlen(canvas_name) + strlen(ws_url) + 47; //  17 + 25 + 1
        response = alloca(max_response_length);
        snprintf(response, max_response_length, "Status of %s: %s\n\n(%s)", canvas_name,
            online ? "**Online** :white_check_mark:" : "**Offline** :x:", ws_url);
    }
    else
    {
        int max_response_length = strlen(ws_url) + 52; // 26 + 25 + 1
        response = alloca(max_response_length);
        snprintf(response, max_response_length, "Custom canvas status: %s\n\n(%s)",
            online ? "**Online** :white_check_mark:" : "**Offline** :x:", ws_url);
    }

    struct discord_embed embed = {
        .title = "Status",
        .description = response,
        .color = 0xFF4500,
        .footer = NULL};

    struct discord_create_message params = {
        .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
    discord_create_message(client, event->channel_id, &params, NULL);

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    pthread_mutex_unlock(&fetch_lock);
}

regex_t rplace_over_regex;

void on_message(struct discord* client, const struct discord_message* event)
{
    // Execute the regular expression
    int reti = regexec(&rplace_over_regex, event->content, 0, NULL, 0);
    if (!reti)
    {
        struct discord_create_message params = {.content = "rplace.live is forever"};
        discord_create_message(client, event->channel_id, &params, NULL);
    }
    else if (reti != REG_NOMATCH)
    {
        char error_buffer[100];
        regerror(reti, &rplace_over_regex, error_buffer, sizeof(error_buffer));
        fprintf(stderr, "Regex match failed: %s\n", error_buffer);
    }

    time_t current_time = time(NULL);
    for (int i = 0; i < active_censors_size; i++)
    {
        if (active_censors[i].member_id != event->author->id)
        {
            continue;
        }

        if (active_censors[i].end_date > current_time)
        {
            struct discord_delete_message delete_info = { };
            discord_delete_message(client, event->channel_id, event->id, &delete_info, NULL);
        }
        else
        {
            // Remove this from active censors
            memmove(active_censors + (sizeof(struct censor) * i),
                (&active_censors[i + 1]),
                sizeof(struct censor) * (active_censors_capacity - i));
            active_censors_size--;
        }
    }
}

void on_discord_ready(struct discord* client, const struct discord_ready* event)
{
    log_info("\x1b[32;1mRplace canvas bot succesfully connected to Discord as %s#%s!\x1b[0m\n",
                event->user->username, event->user->discriminator);
    start_all_periodic_archives(client);
}

int msleep(unsigned long milliseconds)
{
    struct timespec time_span;

    if (milliseconds < 0)
    {
        errno = EINVAL;
        return -1;
    }

    time_span.tv_sec = milliseconds / 1000;
    time_span.tv_nsec = (milliseconds % 1000) * 1000000;

    int result;
    do
    {
        result = nanosleep(&time_span, &time_span);
    } while (result == -1 && errno == EINTR);

    return result;
}

// TODO: Inject telebot handle into this method instead of using global object
void telegram_listen_message(void* data)
{
    int index = 0;
    int count = 0;
    int offset = -1;
    telebot_error_e telebot_error;
    telebot_message_t message;
    telebot_update_type_e update_types[] = { TELEBOT_UPDATE_TYPE_MESSAGE };

    // Runs on a tick system. We will query every 300ms
    while (1)
    {
        telebot_update_t* updates;
        telebot_error = telebot_get_updates(_telegram_client, offset, 20, 0, update_types, 0, &updates, &count);
        if (telebot_error != TELEBOT_ERROR_NONE)
        {
            continue;
        }

        for (index = 0; index < count; index++)
        {
            message = updates[index].message;
            if (message.text)
            {
                if (strstr(message.text, "/view"))
                {
                    // TODO: Implement view command
                }

                if (telebot_error != TELEBOT_ERROR_NONE)
                {
                    fprintf(stderr, "Failed to send telegram message: %d \n", telebot_error);
                }
            }

            offset = updates[index].update_id + 1;
        }

        telebot_put_updates(updates, count);
        msleep(300);
    }

    telebot_destroy(_telegram_client);
}

void parse_view_canvases(const char* key, JSON_Value* value, struct view_canvas* canvas)
{
    JSON_Object* obj = json_value_get_object(value);
    canvas->name = strdup(key);
    canvas->socket = strdup(json_object_get_string(obj, "socket"));
    canvas->http = strdup(json_object_get_string(obj, "http"));
}

void parse_mod_roles(const char* key, JSON_Value* value, u64snowflake** roles, int* count)
{
    JSON_Array* arr = json_value_get_array(value);
    *count = json_array_get_count(arr);
    *roles = (u64snowflake*) malloc(*count * sizeof(u64snowflake));

    for (int i = 0; i < *count; i++) {
        JSON_Value* item = json_array_get_value(arr, i);
        // This library internally uses doubles when reading JSONdebugging numbers which results in the values
        // being really miniscully truncated and rounded when casted into u64snowflakes. So now we have to read these
        // numbers as a string and then parse it to u64snowflake after. Example, atrocious this is, 960971746842935297
        // was being read as 960971746842935296. Seriously? Thanks a lot, parson creators.
        const char* num_char_slice = json_value_get_string(item);
        size_t num_str_len = json_value_get_string_len(item);
        char num_str[num_str_len + 1];
        memcpy(num_str, num_char_slice, num_str_len);
        num_str[num_str_len] = '\0';
        (*roles)[i] = strtoull(num_str, NULL, 10);
    }
}

void process_rplace_config_json(const char* json_string, struct rplace_config* config)
{
    JSON_Value* root = json_parse_string(json_string);
    JSON_Object* root_obj = json_value_get_object(root);
    JSON_Value* mod_roles_value = json_object_get_value(root_obj, "mod_roles");
    JSON_Value* max_mod_purge_per_hr_value = json_object_get_value(root_obj, "max_mod_purge_per_hr");
    JSON_Object* view_canvases_obj = json_object_get_object(root_obj, "view_canvases");

    parse_mod_roles("mod_roles", mod_roles_value, &config->mod_roles, &config->mod_roles_count);

    config->max_hourly_mod_purge = (int) json_value_get_number(max_mod_purge_per_hr_value);
    config->view_canvases_count = json_object_get_count(view_canvases_obj);
    config->view_canvases = (struct view_canvas*) malloc(config->view_canvases_count * sizeof(struct view_canvas));

    for (int canvas_index = 0; canvas_index < config->view_canvases_count; canvas_index++)
    {
        const char* canvas_name = json_object_get_name(view_canvases_obj, canvas_index);
        JSON_Value* canvas_value = json_object_get_value_at(view_canvases_obj, canvas_index);
        
        parse_view_canvases(canvas_name, canvas_value, &config->view_canvases[canvas_index]);
    }

    json_value_free(root);
}

telebot_handler_t process_telegram_config_json(const char* json_string)
{
    JSON_Value* root = json_parse_string(json_string);
    JSON_Object* root_obj = json_value_get_object(root);
    JSON_Object* telegram_obj = json_object_get_object(root_obj, "telegram");
    if (telegram_obj == NULL)
    {
        return NULL;
    }

    JSON_Value* telegram_token_value = json_object_get_value(telegram_obj, "token");
    const char* telegram_token_const = json_value_get_string(telegram_token_value);
    char* telegram_token = strcpy(malloc(strlen(telegram_token_const) + 1), telegram_token_const);
    telebot_handler_t handle = NULL;
    if (telebot_create(&handle, telegram_token) != TELEBOT_ERROR_NONE)
    {
        free(telegram_token);
        return NULL;
    }

    //free(telegram_token);
    return handle;
}

long get_file_length(FILE* file)
{
    struct stat file_stat;
    if (fstat(fileno(file), &file_stat) == -1)
    {
        perror("Error getting file status");
        return -1;
    }
    return file_stat.st_size;
}

int main(int argc, char* argv[])
{
    const char* config_file = "config.json";

    ccord_global_init();
    struct discord* client = discord_config_init(config_file);

    FILE* telegram_config_file = fopen(config_file, "rb");
    if (telegram_config_file == NULL)
    {
        fprintf(stderr, "[CRITICAL] Could not bot config. File was inacessible?.\n");
        return 1;
    }

    long telegram_config_size = get_file_length(telegram_config_file);
    char* telegram_config_text = malloc(telegram_config_size + 1);
    fread(telegram_config_text, telegram_config_size, 1, telegram_config_file);
    telegram_config_text[telegram_config_size] = '\0';
    _telegram_client = process_telegram_config_json(telegram_config_text);
    free(telegram_config_text);
    free(telegram_config_file);

    if (_telegram_client != NULL)
    {
        /*
        telebot_user_t me;
        if (telebot_get_me(_telegram_client, &me) != TELEBOT_ERROR_NONE)
        {
            log_error("Couldn't initialise telegram bot. Failed to get bot information. Bot will be disabled");
            telebot_destroy(_telegram_client);
            _telegram_client = NULL;
        }
        else
        {
            telebot_put_me(&me);
            log_info("Telegram bot successfully connected as @%s (%s)", me.username, me.id);
            if (pthread_create(&telegram_bot_thread, NULL, telegram_listen_message, NULL) != 0
                || pthread_join(telegram_bot_thread, NULL) != 0)
            {
                fprintf(stderr, "Could not initialise telegram bot thread.\n");
            }
        }*/
    }

    FILE* rplace_config_file = fopen("rplace_bot.json", "rb");
    if (rplace_config_file == NULL)
    {
        fprintf(stderr, "[CRITICAL] Could not read rplace config. File does not exist?\n");
        return 1;
    }

    long rplace_config_size = get_file_length(rplace_config_file);
    char* rplace_config_text = malloc(rplace_config_size + 1);
    fread(rplace_config_text, rplace_config_size, 1, rplace_config_file);
    rplace_config_text[rplace_config_size] = '\0';

    rplace_bot_config = calloc(1, sizeof(struct rplace_config));
    process_rplace_config_json(rplace_config_text, rplace_bot_config);

    free(rplace_config_text);
    fclose(rplace_config_file);

    if (pthread_mutex_init(&fetch_lock, NULL) != 0)
    {
        fprintf(stderr, "[CRITICAL] Failed to init fetch lock mutex. Bot can not run.\n");
        return 1;
    }

    char* rplace_over_pattern = "r/?place[^.\\n]+(closed|ended|over|finished|shutdown|done|stopped)/";

    // Compile the regular expression
    if (regcomp(&rplace_over_regex, rplace_over_pattern, REG_EXTENDED))
    {
        fprintf(stderr, "[CRITICAL] Could not compile 'rplace over' regex. Bot can not run.\n");
        return 1;
    }
    
    char* db_err_msg;
    int db_err = sqlite3_open("rplace_bot.db", &bot_db);
    if (db_err)
    {
        fprintf(stderr, "[CRITICAL] Could not open bot database: %s\n", sqlite3_errmsg(bot_db));
        sqlite3_close(bot_db);
        return 1;
    }

    db_err = sqlite3_exec(bot_db, "CREATE TABLE IF NOT EXISTS CensorsHistory ( \
            member_id INTEGER PRIMARY KEY UNIQUE, \
            moderator_id INTEGER, \
            censor_start INTEGER, \
            censor_end INTEGER, \
            reason TEXT \
        )", NULL, NULL, &db_err_msg);
    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: Could not create Censors History table: %s\n", db_err_msg);
        sqlite3_free(db_err_msg);
    }

    sqlite3_exec(bot_db, "CREATE TABLE IF NOT EXISTS PurgesHistory ( \
            member_id INTEGER, \
            channel_id INTEGER, \
            moderator_id INTEGER, \
            message_count INTEGER, \
            purge_date INTEGER \
        )", NULL, NULL, &db_err_msg);
    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: Could not create PurgesHistory table: %s\n", db_err_msg);
        sqlite3_free(db_err_msg);
    }

    db_err = sqlite3_exec(bot_db, "CREATE TABLE IF NOT EXISTS PeriodicArchives ( \
            channel_id INTEGER, \
            period_s INTEGER, \
            board_url TEXT \
        )", NULL, NULL, &db_err_msg);
    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: Could not create PeriodicArchives table: %s\n", db_err_msg);
        sqlite3_free(db_err_msg);
    }

    // Load all applicable current active from censors history into memory
    const char* query_get_active_censors = "SELECT member_id, censor_end FROM CensorsHistory WHERE censor_end > ?";
    sqlite3_stmt* get_cmp_statement;
    if (sqlite3_prepare_v2(bot_db, query_get_active_censors, -1, &get_cmp_statement, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "Could not prepare get active censors: %s\n", sqlite3_errmsg(bot_db));
        return 1;
    }
    sqlite3_bind_int64(get_cmp_statement, 1, time(NULL));

    int step = sqlite3_step(get_cmp_statement);
    while (step == SQLITE_ROW)
    {
        add_active_censor(sqlite3_column_int64(get_cmp_statement, 0),
            sqlite3_column_int64(get_cmp_statement, 1));
        step = sqlite3_step(get_cmp_statement);
    }
    if (step != SQLITE_DONE)
    {
        fprintf(stderr, "[CRITICAL] Could not apply active censors: %s\n", sqlite3_errmsg(bot_db));
        return 1;
    }

    _discord_client = client;
    signal(SIGINT, handle_sigint);

    discord_set_on_ready(client, &on_discord_ready);
    discord_set_on_command(client, "view", &on_canvas_mention);
    discord_set_on_command(client, "help", &on_help);
    discord_set_on_command(client, "status", &on_status);
    discord_set_on_command(client, "archive", &on_archive);
    discord_set_on_command(client, "modhelp", &on_mod_help);
    discord_set_on_command(client, "modhistory", &on_mod_history);
    discord_set_on_command(client, "1984", &on_1984);
    discord_set_on_command(client, "purge", &on_purge);
    discord_set_on_command(client, "", &on_help);
    discord_set_on_commands(client, (char* []){ "help", "?", "" }, 3, &on_help);
    discord_set_on_message_create(client, &on_message);
    discord_run(client);
    
    discord_cleanup(client);
    ccord_global_cleanup();
    pthread_mutex_destroy(&fetch_lock);
    regfree(&rplace_over_regex);
    sqlite3_close(bot_db);
}
