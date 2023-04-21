#include <stdio.h>
#include <stdlib.h>
#include <concord/discord.h>
#include <concord/log.h>
#include <curl/curl.h>
#include <png.h>
#include <string.h>
#include <sys/param.h>

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

}

void on_canvas_mention(struct discord* client, const struct discord_message* event) {
    if (event->author->bot) return;

    char* count_state = NULL;
    char* arg = strtok_r(event->content, " ", &count_state);
    if (arg == NULL) return;
    char* canvas_name = arg;
    char* canvas_url = NULL;
    int image_bounds[4];
    int canvas_width;
    int canvas_height;

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

    for (int i = 0; i < 4; i++) {
        arg = strtok_r(NULL, " ", &count_state);
        if (arg == NULL) {
            struct discord_create_message params = { .content =
                "Not enough arguments supplied, use this command like:\n"
                "r/view `canvas1/canvas2/turkeycanvas` `x` `y` `w` `h`" };
            discord_create_message(client, event->channel_id, &params, NULL);
            return;
        }

        image_bounds[i] = atoi(arg);
    }

    // TODO: Split out image bounds into separate variables
    image_bounds[0] = MAX(0, image_bounds[0]);
    image_bounds[1] = MAX(0, image_bounds[1]);
    image_bounds[2] = MIN(canvas_width - image_bounds[0], image_bounds[2]);
    image_bounds[3] = MIN(canvas_height - image_bounds[1], image_bounds[3]);

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
    if (result != CURLE_OK || chunk.size < canvas_width * canvas_height) {
        printf("%s\n", curl_easy_strerror(result));
        perror("Error fetching file");
        printf("%d\n", result);

        struct discord_create_message params = { .content =
            "Sorry, an unexpected network error ocurred and I can't fetch that canvas, "
            "please try again later." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        struct discord_create_message params = { .content =
            "Sorry, an unexpected drawing error and I can't create an image of that canvas, "
            "please try again later." };
        discord_create_message(client, event->channel_id, &params, NULL);
        return;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        struct discord_create_message params = { .content =
            "Sorry, an unexpected drawing error and I can't create an image of that canvas, "
            "please try again later." };
        png_destroy_write_struct(&png_ptr, NULL);
        return;
    }

    png_init_io(png_ptr, memory_stream);
    png_set_IHDR(png_ptr, info_ptr, image_bounds[2], image_bounds[3], 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    png_bytep row_pointers[image_bounds[3]];
    
    for (int i = 0; i < image_bounds[3]; i++) {
        row_pointers[i] = (png_bytep) calloc(3 * image_bounds[2], sizeof(png_byte));
    }
    
    // image_bounds[0] = startX, image_bounds[1] = endX,
    // image_bounds[2] = width, image_bounds[3] = height
    int i = canvas_width * image_bounds[0] + image_bounds[1];
    while (i < canvas_width * canvas_height) {
        memcpy(&row_pointers
                    [i / canvas_width - image_bounds[1]] // y
                    [3 * (i % canvas_width - image_bounds[0])], // x
              default_palette[chunk.memory[i]], 3); // colour
        i++;

        // If we exceed width, go to next row, otherwise continue
        if (i % canvas_width < image_bounds[0] + image_bounds[2]) {
            continue; 
        }

        // If we exceed end bottom, we are done drawing this
        if (i / canvas_width == image_bounds[1] + image_bounds[3] - 1) {
            break; 
        }
        
        i += canvas_width - image_bounds[2];
    }

    png_write_image(png_ptr, row_pointers);

    for (int i = 0; i < image_bounds[3]; i++) {
        free(row_pointers[i]);
    }

    png_write_end(png_ptr, NULL);
    fflush(memory_stream);

    // At minimum, may be "Image at 65535 65535 on ``, source: " (length 36 + 1 (\0))
    int max_response_length = strlen(canvas_url) + strlen(canvas_name) + 37;
    char* response = malloc(max_response_length); 
    snprintf(response, max_response_length, "Image at %i %i on `%s`, source: %s",
        image_bounds[0], image_bounds[1], canvas_name, canvas_url);
        

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
    
    free(response);
    free(chunk.memory);
    fclose(memory_stream);
    free(stream_buffer);
    curl_easy_cleanup(curl);
    png_destroy_write_struct(&png_ptr, &info_ptr);
}

int main(int argc, char* argv[]) {
    const char* config_file = "config.json";

    ccord_global_init();
    struct discord* client = discord_config_init(config_file);

    discord_set_on_ready(client, &on_ready);
    discord_set_on_command(client, "view", &on_canvas_mention);
    discord_set_on_command(client, "help", &on_help);
    discord_set_on_command(client, "", &on_help);
    discord_set_on_commands(client, (char*[]){ "help", "?", "" }, 3,  &on_help);
    discord_run(client);

    discord_cleanup(client);
    ccord_global_cleanup();
}
