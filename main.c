#include <stdio.h>
#include <stdlib.h>
#include <concord/discord.h>
#include <concord/log.h>
#include <curl/curl.h>
#include <png.h>
#include <string.h>
#include <sys/param.h>
#include <pthread.h>

struct memory_fetch {
    size_t size;
    char* memory;
};

struct colour {
    char red;
    char green;
    char blue;
};

char default_palette[32][3] = {
    { 109, 0, 26 },
    { 190, 0, 57 },
    { 255, 69, 0 },
    { 255, 168, 0 },
    { 255, 214, 53 },
    { 255, 248, 184 },
    { 0, 163, 104 },
    { 0, 204, 120 },
    { 126, 237, 86 },
    { 0, 117, 111 },
    { 0, 158, 170 },
    { 0, 204, 192 },
    { 36, 80, 164 },
    { 54, 144, 234 },
    { 81, 233, 244 },
    { 73, 58, 193 },
    { 106, 92, 255 },
    { 148, 179, 255 },
    { 129, 30, 159 },
    { 180, 74, 192 },
    { 228, 171, 255 },
    { 222, 16, 127 },
    { 255, 56, 129 },
    { 255, 153, 170 },
    { 109, 72, 47 },
    { 156, 105, 38 },
    { 255, 180, 112 },
    { 0, 0, 0 },
    { 81, 82, 82 },
    { 137, 141, 144 },
    { 212, 215, 217 },
    { 255, 255, 255 }
};

pthread_mutex_t fetch_lock;

// You will have to install concord separately unfortunately as the library itself
// does not implement a cmakelists.txt to be compiled alongside this project as a gitmodule
// This project can be compiled easily with gcc main.c -o RplaceBot -pthread -ldiscord -lcurl -lpng
void on_ready(struct discord* client, const struct discord_ready* event) {
    log_info("Rplace canvas bot succesfully connected to Discord as %s#%s!",
             event->user->username, event->user->discriminator);
}


static size_t write_fetch(void* contents, size_t size, size_t nmemb, void* userp) {
    // Size * number of elements
    size_t data_size = size * nmemb;
    struct memory_fetch* fetch = (struct memory_fetch*) userp;

    char* new_memory = realloc(fetch->memory, fetch->size + data_size + 1);
    if (new_memory == NULL) {
        printf("Out of memory, can not carry out fetch reallocation\n");
        return 0;
    }

    fetch->memory = new_memory;
    memcpy(&(fetch->memory[fetch->size]), contents, data_size);
    fetch->size += data_size;
    fetch->memory[fetch->size] = 0;
 
    return data_size;
}

void on_help(struct discord* client, const struct discord_message* event) {
    struct discord_embed embed = {
        .title = "Commands",
        .description =
            "**r/view** `canvas1/canvas2/turkeycanvas/...` `x` `y` `w` `h` \n*Create an image from a region of the canvas*\n\n"
            "**r/help**, **r/?**, **r/** \n*Displays information about this bot*\n\n",
        .color = 0xFF4500,
        .footer = &(struct discord_embed_footer) {
            .text = "https://rplace.tk, bot by Zekiah-A",
            .icon_url = "https://github.com/rslashplace2/rslashplace2.github.io/raw/main/favicon.png"
        }
    };
    
    struct discord_create_message params = {
        .embeds = &(struct discord_embeds) { .size = 1, .array = &embed }
    };
    
    discord_create_message(client, event->channel_id, &params, NULL);
}

void on_canvas_mention(struct discord* client, const struct discord_message* event) {
    if (event->author->bot) return;

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    if (arg == NULL) return;
    char* canvas_name = arg;
    char* canvas_url = NULL;
    int canvas_width = 0;
    int canvas_height = 0;

    // Read parameters from message
    if (strcmp(arg, "canvas1") == 0) {
        canvas_url = "https://raw.githubusercontent.com/rslashplace2/rslashplace2.github.io/main/place";
        canvas_width = 1000;
        canvas_height = 1000;
    }
    else if (strcmp(arg, "canvas2") == 0) {
        canvas_url = "https://server.poemanthology.org/place";
        canvas_width = 500;
        canvas_height = 500;
    }
    else if (strcmp(arg, "turkeycanvas") == 0) {
        canvas_url = "https://server.poemanthology.org/turkeyplace";
        canvas_width = 250;
        canvas_height = 250;
    }
    else {
        struct discord_create_message params = { .content =
            "At the moment, custom canvases URLs are not supported.\n"
            "Try r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h`" };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    int start_x = 0;
    int start_y = 0;
    int width = canvas_width - 1;
    int height = canvas_height - 1;

    arg = strtok_r(NULL, " ", &count_state);
    if (arg != NULL) {
        start_x = MAX(0, MIN(canvas_width - 1, atoi(arg)));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL) {
            struct discord_create_message params = { .content =
                "Start Y argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        start_y = MAX(0, MIN(canvas_width - 1, atoi(arg)));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL) {
            struct discord_create_message params = { .content =
                "Width argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        width = MIN(canvas_width - 1 - start_x, atoi(arg));

        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL) {
            struct discord_create_message params = { .content =
                "Height argument not supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }
        height = MIN(canvas_height - 1 - start_y, atoi(arg));

        if (width <= 0 || height <= 0) {
            struct discord_create_message params = { .content =
                "Height or width can not be zero, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h`" };
            discord_create_message(client, event->channel_id, &params, NULL);
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_fetch);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        printf("%s\n", curl_easy_strerror(result));
        perror("Error fetching file");
        printf("%d\n", result);

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
        char* decoded_board = malloc(decoded_size);
        int boardI = 0;
        int colour = 0;

        for (int i = 0; i < chunk.size; i++) {
            // Then it is a palette value
            if (i % 2 == 0) {
                colour = chunk.memory[i];
                continue;
            }
            // After the colour, we koop until we unpack all repeats, since we never have zero
            // repeats, we use 0 as 1 so we treat everything as i + 1 repeats.
            for (int j = 0; j < chunk.memory[i] + 1; j++) {
                decoded_board[boardI] = colour;
                boardI++;
            }
       }

        free(chunk.memory);
        chunk.memory = decoded_board;
        chunk.size = decoded_size;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) {
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
    if (info_ptr == NULL) {
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
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    png_bytep row_pointers[height];
    
    for (int i = 0; i < height; i++) {
        row_pointers[i] = (png_bytep) calloc(3 * width, sizeof(png_byte));
    }
    
    int i = canvas_width * start_y + start_x;
    while (i < canvas_width * canvas_height) {        
        // Copy over colour to image
        char* position = &row_pointers
            [i / canvas_width - start_y] //x
            [3 * (i % canvas_width - start_x)]; //y
        for (int p = 0; p < 3; p++) {
            position[p] = default_palette[chunk.memory[i]][p]; // colour
        }
        i++;

        // If we exceed width, go to next row, otherwise keep drawing on this row
        if (i % canvas_width < start_x + width) {
            continue; 
        }

        // If we exceed end bottom, we are done drawing this
        if (i / canvas_width >= start_y + height - 1) {
            break; 
        }
        
        i += canvas_width - width;
    }

    png_write_image(png_ptr, row_pointers);

    for (int i = 0; i < height; i++) {
        free(row_pointers[i]);
    }

    png_write_end(png_ptr, NULL);
    fflush(memory_stream);

    // At minimum, may be "Image at 65535 65535 on ``, source: " (length 36 + 1 (\0))
    int max_response_length = strlen(canvas_url) + strlen(canvas_name) + 37;
    char* response = malloc(max_response_length); 
    snprintf(response, max_response_length, "Image at %i %i on `%s`, source: %s",
        start_x, start_y, canvas_name, canvas_url);
        

    struct discord_create_message params = {
        .content = response,
        .attachments = &(struct discord_attachments) {
            .size = 1,
            .array = &(struct discord_attachment) {
                .content = stream_buffer,
                .size = stream_length,
                .filename = "place.png"
            },
        }
    };
    discord_create_message(client, event->channel_id, &params, NULL);
    
    // Cleanup
    free(response);
    free(chunk.memory);
    fclose(memory_stream);
    free(stream_buffer);
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&fetch_lock);
    png_destroy_write_struct(&png_ptr, &info_ptr);
}

int main(int argc, char* argv[]) {
    const char* config_file = "config.json";

    ccord_global_init();
    struct discord* client = discord_config_init(config_file);

    if (pthread_mutex_init(&fetch_lock, NULL) != 0) {
        printf("[CRITICAL] Failed to init fetch lock mutex. Bot can not run.\n");
        return 1;
    }

    discord_set_on_ready(client, &on_ready);
    discord_set_on_command(client, "view", &on_canvas_mention);
    discord_set_on_command(client, "help", &on_help);
    discord_set_on_command(client, "", &on_help);
    discord_set_on_commands(client, (char*[]){ "help", "?", "" }, 3,  &on_help);
    discord_run(client);

    discord_cleanup(client);
    ccord_global_cleanup();
    pthread_mutex_destroy(&fetch_lock);
}
