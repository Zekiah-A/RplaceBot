// RplaceBot (c) Zekiah-A - BUILD INSTRUCTIONS:
// You will have to install concord separately unfortunately as the library itself
// does not implement a cmakelists.txt to be compiled alongside this project as a gitmodule
// You will also have to self compile CURL with websocket support if you receive 'curl_easy_perform() failed: Unsupported protocol'
// error messages. To check if your cURL has support, run curl --version and check for ws/wss protocols present.
// This project can be compiled easily with gcc main.c lib/parson.c lib/parson.h -o RplaceBot -pthread -ldiscord -lcurl -lpng -lsqlite3
// This project can be debugged with flags -Wall -Wextra -Wno-unused-parameter / -g -fsanitize=address
#include <stdio.h>
#include <stdlib.h>
#include <concord/discord.h>
#include <concord/log.h>
#include <curl/curl.h>
#include <png.h>
#include <string.h>
#include <sys/param.h>
#include <pthread.h>
#include <stdint.h>
#include <alloca.h>
#include <regex.h>
#include <sqlite3.h>
#include <signal.h>
#include <time.h>
#include "lib/parson.h"

struct memory_fetch {
    size_t size;
    uint8_t* memory;
};

struct colour {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct config {
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
    int width;
    int height;
};

struct censor {
    u64snowflake member_id;
    time_t end_date;
};

struct censor* active_censors;
int active_censors_size = 0;
int active_censors_capacity = 0;

struct config* rplace_config;

struct discord_client_message_info {
    struct discord* client;
    u64snowflake message_id;
    u64snowflake channel_id;
    u64snowflake author_id;
};

uint8_t default_palette[32][3] = {
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
int db_err;
char* db_err_msg;

struct discord* _discord_client;
int requested_sigint = 0;

void handle_sigint(int signum)
{
    if (requested_sigint)
    {
        printf("\rForce quitting!");
        exit(1);
    }

    if (signum == SIGINT)
    {
        printf("\nPerforming cleanup. Wait a sec!\n");
        sqlite3_close(bot_db);
        discord_shutdown(_discord_client);
        requested_sigint = 1;
    }
}

void on_ready(struct discord* client, const struct discord_ready* event)
{
    log_info("Rplace canvas bot succesfully connected to Discord as %s#%s!",
             event->user->username, event->user->discriminator);
}

int check_member_has_mod(struct discord* client, u64snowflake guild_id, u64snowflake member_id)
{
    struct discord_guild_member guild_member = { .roles = NULL };
    struct discord_ret_guild_member guild_member_ret = {.sync = &guild_member};
    discord_get_guild_member(client, guild_id, member_id, &guild_member_ret);

    if (guild_member.roles == NULL || rplace_config == NULL || rplace_config->mod_roles == NULL)
    {
        return 0;
    }

    for (int i = 0; i < guild_member.roles->size; i++)
    {
        for (int j = 0; j < rplace_config->mod_roles_count; j++)
        {
            if (guild_member.roles->array[i] == rplace_config->mod_roles[j])
            {
                return 1;
            }
        }
    }

    return 0;
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
    struct discord_embed embed = {
        .title = "Moderation action - 1984 user",
        .color = 0xFF4500,
        .footer = &(struct discord_embed_footer) {
            .text = "https://rplace.live, bot by Zekiah-A",
            .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"}};


    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        embed.description = "Sorry. You need moderator or higher permissions to be able to use this command!";
        embed.image = &(struct discord_embed_image) {
            .url = "https://media.tenor.com/qmSIzc-H7vIAAAAC/1984.gif" };

        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
        discord_create_message(client, event->channel_id, &params, NULL);
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
        embed.description = "Sorry. Can't find the user that you are trying to 1984. ¯\\_(ツ)_/¯";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
        
    if (check_member_has_mod(client, event->guild_id, member->id))
    {
        free(member);
        embed.description = "Sorry. You can not 1984 another moderator!";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
    if (period_s > 31540000) // 365 days
    {
        free(member);
        embed.description = "Sorry. That 1984 is too massive! (Maximum 365 days).";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    else if (period_s <= 0)
    {
        free(member);
        embed.description = "You can't 1984 for that period of time? (Minimum 1 second).";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
        
        embed.description = "Failed to 1984 user. Internal bot error occured :skull:";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    sqlite3_bind_int64(existing_cmp_statement, 1, member->id);

    // Existing, so we need to delete before making new (too lazy to update)
    if (sqlite3_step(existing_cmp_statement) == SQLITE_ROW)
    {
        char* query_delete_1984 = sqlite3_mprintf("DELETE FROM CensorsHistory WHERE member_id='%llu'", member->id);
        db_err = sqlite3_exec(bot_db, query_delete_1984, NULL, NULL, &db_err_msg);
        if (db_err != SQLITE_OK)
        {
            free(member);
            sqlite3_finalize(existing_cmp_statement);
            fprintf(stderr, "Could not remove existing censor: %s\n", db_err_msg);
            sqlite3_free(db_err_msg);

            embed.description = "Failed to 1984 user. Internal bot error occured :skull:";
            struct discord_create_message params = {
                .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
        sqlite3_bind_int64(insert_cmp_statement, 4, current_time + period_s);
        sqlite3_bind_text(insert_cmp_statement, 5, reason, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insert_cmp_statement) != SQLITE_DONE)
        {
            free(member);
            fprintf(stderr, "Could not insert censor: %s\n", sqlite3_errmsg(bot_db));
            
            embed.description = "Failed to 1984 user. Internal bot error occured :skull:";
            struct discord_create_message params = {
                .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        sqlite3_finalize(insert_cmp_statement);
    }
    else
    {
        free(member);
        fprintf(stderr, "Could not prepare insert censor: %s\n", sqlite3_errmsg(bot_db));
        
        embed.description = "Failed to 1984 user. Internal bot error occured :skull:";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }
    add_active_censor(member->id, current_time + period_s);
    
    const char* raw_str_1984 = "Successfully 1984ed user **%s** for **%d %s** (reason: **%s**).";
    size_t str_1984_len = snprintf(NULL, 0, raw_str_1984, member->username, period_original, period_unit, reason) + 1;
    char* str_1984 = malloc(str_1984_len);
    snprintf(str_1984, str_1984_len, raw_str_1984, member->username, period_original, period_unit, reason);
    embed.description = str_1984;

    struct discord_create_message params = {
        .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
    discord_create_message(client, event->channel_id, &params, NULL);

    free(reason);
    free(str_1984);
}

void on_purge(struct discord* client, const struct discord_message* event)
{
    struct discord_embed embed = {
        .title = "Moderation action - Purge user",
        .color = 0xFF4500,
        .footer = &(struct discord_embed_footer) {
            .text = "https://rplace.live, bot by Zekiah-A",
            .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"}};


    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        embed.description = "Sorry. You need moderator or higher permissions to be able to use this command!";
        embed.image = &(struct discord_embed_image) {
            .url = "https://media.tenor.com/qmSIzc-H7vIAAAAC/1984.gif" };

        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
        discord_create_message(client, event->channel_id, &params, NULL);
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
        embed.description = "Sorry. You can't purge less than one message!";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
            embed.description = "Sorry. Can't find the user that you are trying to purge. (-_- )";
            struct discord_create_message params = {
                .embeds = &(struct discord_embeds){ .size = 1, .array = &embed }};
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

        embed.description = "Failed to purge messages. An internal bot error occured :skull:";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
        if ((moderator_hourly_purge += message_count) > rplace_config->max_hourly_mod_purge)
        {
            if (member != NULL)
            {
                free(member);
            }

            const char* raw_str_purge_cooldown = "Calm down! You can't purge more than %i messages within an hour.";
            size_t str_purge_cooldown_len = snprintf(NULL, 0, raw_str_purge_cooldown, rplace_config->max_hourly_mod_purge) + 1;
            char* str_purge_cooldown = malloc(str_purge_cooldown_len);
            snprintf(str_purge_cooldown, str_purge_cooldown_len, raw_str_purge_cooldown, rplace_config->max_hourly_mod_purge);
            
            embed.description = str_purge_cooldown;
            struct discord_create_message params = {
                .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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

        embed.description = "Failed to purge messages. An internal bot error occured :skull:";
        struct discord_create_message params = {
            .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
            
            embed.description = "Failed to purge user messages. Internal bot error occured :skull:";
            struct discord_create_message params = {
                .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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

            embed.description = "Failed to purge messages. Internal bot error occured :skull:";
            struct discord_create_message params = {
                .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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
    struct discord_embed embed = {
    .title = "Moderation action - View history",
    .color = 0xFF4500,
    .footer = &(struct discord_embed_footer) {
        .text = "https://rplace.live, bot by Zekiah-A",
        .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"}};

    if (!check_member_has_mod(client, event->guild_id, event->author->id))
    {
        embed.description = "Sorry. You need moderator or higher permissions to be able to use this command!";
        embed.image = &(struct discord_embed_image) {
            .url = "https://media.tenor.com/qmSIzc-H7vIAAAAC/1984.gif" };

        struct discord_create_message params = {
            .embeds = &(struct discord_embeds) { .size = 1, .array = &embed }};
        discord_create_message(client, event->channel_id, &params, NULL);
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
    db_err = sqlite3_prepare_v2(bot_db, query_get_censors, -1, &censors_cmp_statement, NULL);

    if (db_err != SQLITE_OK)
    {
        fprintf(stderr, "Could not prepare get censors: %s\n", sqlite3_errmsg(bot_db));
        return;
    }

    while (sqlite3_step(censors_cmp_statement) == SQLITE_ROW)
    {
        const uint64_t start_date_i = sqlite3_column_int64(censors_cmp_statement, 2);
        const uint64_t int_member_id = sqlite3_column_int64(censors_cmp_statement, 0);
        const char* str_member_id = sqlite3_column_text(censors_cmp_statement, 0);
        const uint64_t int_moderator_id = sqlite3_column_int64(censors_cmp_statement, 1);
        const char* str_moderator_id = sqlite3_column_text(censors_cmp_statement, 1);
        const uint64_t end_date_i = sqlite3_column_int64(censors_cmp_statement, 3);
        const char* reason = sqlite3_column_text(censors_cmp_statement, 4);

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

    embed.description = tables;
    struct discord_create_message params = {
        .embeds = &(struct discord_embeds){.size = 1, .array = &embed}};
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

    fetch->memory = realloc(fetch->memory, fetch->size + realsize);
    if (fetch->memory == NULL)
    {
        fetch->size = 0;
        fprintf(stderr, "Not enough memory to continue fetch (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(fetch->memory[fetch->size]), contents, realsize);
    fetch->size += realsize;

    return realsize;
}

void on_canvas_mention(struct discord* client, const struct discord_message* event)
{
    if (event->author->bot)
        return;

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    if (arg == NULL)
    {
        on_help(client, event);
        return;
    }
    char* canvas_name = arg;
    char* canvas_url = NULL;
    int canvas_width = 0;
    int canvas_height = 0;

    // Read parameters from message
    for (int i = 0; i < rplace_config->mod_roles_count; i++) {
        struct view_canvas view_canvas = rplace_config->view_canvases[i];

        if (strcmp(arg, view_canvas.name) == 0) {
            canvas_url = view_canvas.http;
            canvas_width = view_canvas.width;
            canvas_height = view_canvas.height;
            break;
        }
    }

    if (canvas_url == NULL)
    {
        struct discord_create_message params = {.content =
            "At the moment, custom canvases URLs are not supported.\n"
            "Format: r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`\n"
            "Try: r/view canvas1 10 10 100 100 2x"};
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    int start_x = 0;
    int start_y = 0;
    int width = canvas_width - 1;
    int height = canvas_height - 1;
    int scale = 1;
    int scaled_width = width;
    int scaled_height = height;

    // No arguments is allowed as it will just do a 1:1 full canvas preview
    arg = strtok_r(NULL, " ", &count_state);
    if (arg != NULL)
    {
        start_x = MAX(0, MIN(canvas_width - 1, atoi(arg)));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL) {
            struct discord_create_message params = { .content =
                "Start Y argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        start_y = MAX(0, MIN(canvas_width - 1, atoi(arg)));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL)
        {
            struct discord_create_message params = { .content =
                "Width argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        width = MIN(canvas_width - 1 - start_x, atoi(arg));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL)
        {
            struct discord_create_message params = { .content =
                "Height argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`"};
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        height = MIN(canvas_height - 1 - start_y, atoi(arg));

        if (width <= 0 || height <= 0)
        {
            struct discord_create_message params = { .content =
                "Height or width can not be zero, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h` `upscale`"};
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        scaled_width = width;
        scaled_height = height;

        arg = strtok_r(NULL, " ", &count_state);
        if (arg != NULL)
        {
            int len = strlen(arg);
            if (arg[len - 1] == 'x')
            {
                arg[len - 1] = '\0';
            }

            scale = MAX(1, MIN(10, atoi(arg)));
            scaled_width = width * scale;
            scaled_height = height * scale;
        }
    }
    // Reassure client that we have stared before we do any heavy lifting
    discord_create_reaction(client, event->channel_id, event->id,
        0, "✅", NULL);
    // Lock before we fetch to ensure no overlapping curl actions
    pthread_mutex_lock(&fetch_lock);

    // Fetch and render canvas
    char* stream_buffer = NULL;
    size_t stream_length = 0;
    FILE* memory_stream = open_memstream(&stream_buffer, &stream_length);
    CURL* curl = curl_easy_init();
    CURLcode result;
    struct memory_fetch chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curl, CURLOPT_URL, canvas_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK)
    {
        fprintf(stderr, "Error fetching file: %s\n", curl_easy_strerror(result));

        struct discord_create_message params = { .content =
            "Sorry, an unexpected network error ocurred and I can't fetch that canvas, "
            "please try again later." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    // Then this is a new format (RLE encoded) board that must be decoded
    if (chunk.size < canvas_width * canvas_height)
    {
        int decoded_size = canvas_width * canvas_height;
        uint8_t* decoded_board = malloc(decoded_size);
        int boardI = 0;
        int colour = 0;

        for (int i = 0; i < chunk.size; i++)
        {
            // Then it is a palette value
            if (i % 2 == 0)
            {
                colour = chunk.memory[i];
                continue;
            }
            // After the colour, we koop until we unpack all repeats, since we never have zero
            // repeats, we use 0 as 1 so we treat everything as i + 1 repeats.
            for (int j = 0; j < chunk.memory[i] + 1; j++)
            {
                decoded_board[boardI] = colour;
                boardI++;
            }
        }

        free(chunk.memory);
        chunk.memory = decoded_board;
        chunk.size = decoded_size;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL)
    {
        struct discord_create_message params = { .content =
            "Sorry, an unexpected drawing error ocurred and I can't create an image of that canvas, "
            "please try again later." };
        discord_create_message(client, event->channel_id, &params, NULL);

        // Cleanup resources
        png_destroy_write_struct(&png_ptr, NULL);
        free(chunk.memory);
        fclose(memory_stream);
        free(stream_buffer);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&fetch_lock);
        return;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL)
    {
        struct discord_create_message params = { .content =
            "Sorry, an unexpected drawing error ocurred and I can't create an image of that canvas, "
            "please try again later." };

        // Cleanup resources
        png_destroy_write_struct(&png_ptr, NULL);
        free(chunk.memory);
        fclose(memory_stream);
        free(stream_buffer);
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&fetch_lock);
        return;
    }

    png_init_io(png_ptr, memory_stream);
    png_set_IHDR(png_ptr, info_ptr, scaled_width, scaled_height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    png_bytep row_pointers[scaled_height]; // 2D ptr array

    for (int i = 0; i < scaled_height; i++)
    {
        row_pointers[i] = (png_bytep) malloc(3*  scaled_width);
    }

    int i = canvas_width * start_y + start_x;
    while (i < canvas_width * canvas_height)
    {
        // Copy over colour to image
        int x = i / canvas_width - start_y;       // image x (assuming image scale 1:1 with canvas)
        int y = 3 * (i % canvas_width - start_x); // image y (assuming image scale 1:1 with canvas + accounting for 3 byte colour)

        if (scale == 1)
        {
            uint8_t* position = &row_pointers[x][y];

            for (int p = 0; p < 3; p++)
            {
                position[p] = default_palette[chunk.memory[i]][p]; // colour
            }
        }
        else
        {
            for (int sx = 0; sx < scale; sx++)
            {
                for (int sy = 0; sy < scale; sy++)
                {
                    uint8_t* position = &row_pointers
                        [x * scale + sx]      // We project X to upscaled X position
                        [y * scale + sy * 3]; // We project Y to upscaled Y position

                    for (int p = 0; p < 3; p++)
                    {
                        position[p] = default_palette[chunk.memory[i]][p]; // colour
                    }
                }
            }
        }
        i++;

        // If we exceed width, go to next row, otherwise keep drawing on this row
        if (i % canvas_width < start_x + width)
        {
            continue;
        }

        // If we exceed end bottom, we are done drawing this
        if (i / canvas_width >= start_y + height - 1)
        {
            break;
        }

        i += canvas_width - width;
    }

    png_write_image(png_ptr, row_pointers);

    for (int i = 0; i < scaled_height; i++)
    {
        free(row_pointers[i]);
    }

    png_write_end(png_ptr, NULL);
    fflush(memory_stream);

    // At minimum, may be "Image at 65535 65535 on ``, source: " (length 36 + 1 (\0))
    int max_response_length = strlen(canvas_url) + strlen(canvas_name) + 37;
    char* response = alloca(max_response_length); 
    snprintf(response, max_response_length, "Image at %i %i on `%s`, source: %s",
        start_x, start_y, canvas_name, canvas_url);

    struct discord_create_message params = {
        .content = response,
        .attachments = &(struct discord_attachments){
            .size = 1,
            .array = &(struct discord_attachment){
                .content = stream_buffer,
                .size = stream_length,
                .filename = "place.png"},
        }};
    discord_create_message(client, event->channel_id, &params, NULL);

    // Cleanup
    free(chunk.memory);
    fclose(memory_stream);
    free(stream_buffer);
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&fetch_lock);
    png_destroy_write_struct(&png_ptr, &info_ptr);
}

void on_status(struct discord* client, const struct discord_message* event) {
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

    for (int i = 0; i < rplace_config->mod_roles_count; i++) {
        struct view_canvas view_canvas = rplace_config->view_canvases[i];

        if (strcmp(canvas_name, view_canvas.name) == 0) {
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

void parse_view_canvases(const char* key, JSON_Value* value, struct view_canvas* canvas) {
    JSON_Object* obj = json_value_get_object(value);
    canvas->name = strdup(key);
    canvas->socket = strdup(json_object_get_string(obj, "socket"));
    canvas->http = strdup(json_object_get_string(obj, "http"));
    canvas->width = json_object_get_number(obj, "width");
    canvas->height = json_object_get_number(obj, "height");
}

void parse_mod_roles(const char* key, JSON_Value* value, u64snowflake** roles, int* count) {
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

void process_config_json(const char* json_string, struct config* config) {
    JSON_Value* root = json_parse_string(json_string);
    JSON_Object* root_obj = json_value_get_object(root);
    JSON_Value* mod_roles_value = json_object_get_value(root_obj, "mod_roles");
    JSON_Value* max_mod_purge_per_hr_value = json_object_get_value(root_obj, "max_mod_purge_per_hr");
    JSON_Object* view_canvases_obj = json_object_get_object(root_obj, "view_canvases");

    parse_mod_roles("mod_roles", mod_roles_value, &config->mod_roles, &config->mod_roles_count);

    config->max_hourly_mod_purge = (int) json_value_get_number(max_mod_purge_per_hr_value);
    config->view_canvases_count = json_object_get_count(view_canvases_obj);
    config->view_canvases = (struct view_canvas*) malloc(config->view_canvases_count * sizeof(struct view_canvas));

    for (int canvas_index = 0; canvas_index < config->view_canvases_count; canvas_index++) {
        const char* canvas_name = json_object_get_name(view_canvases_obj, canvas_index);
        JSON_Value* canvas_value = json_object_get_value_at(view_canvases_obj, canvas_index);
        
        parse_view_canvases(canvas_name, canvas_value, &config->view_canvases[canvas_index]);
    }

    json_value_free(root);
}

int main(int argc, char* argv[])
{
    const char* config_file = "config.json";

    ccord_global_init();
    struct discord* client = discord_config_init(config_file);

    FILE* rplace_config_file = fopen("rplace_bot.json", "rb");
    if (rplace_config_file == NULL)
    {
        fprintf(stderr, "[CRITICAL] Could not read rplace config. FIle does not exist?.\n");
        return 1;
    }

    fseek(rplace_config_file, 0L, SEEK_END);
    long rplace_config_size = ftell(rplace_config_file);
    fseek(rplace_config_file, 0L, SEEK_SET);

    char* rplace_config_text = malloc(rplace_config_size + 1);
    if (!rplace_config_text)
    {
        fclose(rplace_config_file);
        fprintf(stderr, "[CRITICAL] Could not read rplace bot config. File was empty?.\n");
        return 1;
    }

    fread(rplace_config_text, rplace_config_size, 1, rplace_config_file);
    rplace_config_text[rplace_config_size] = '\0';


    rplace_config = calloc(1, sizeof(struct config));
    process_config_json(rplace_config_text, rplace_config);

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
    
    db_err = sqlite3_open("rplace_bot.db", &bot_db);
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
        fprintf(stderr, "Could not apply active censors: %s\n", sqlite3_errmsg(bot_db));
        return 1;
    }

    _discord_client = client;
    signal(SIGINT, handle_sigint);

    discord_set_on_ready(client, &on_ready);
    discord_set_on_command(client, "view", &on_canvas_mention);
    discord_set_on_command(client, "help", &on_help);
    discord_set_on_command(client, "status", &on_status);
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
