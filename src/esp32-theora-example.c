#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "SDL3/SDL.h"
#include "SDL3/SDL_esp-idf.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ogg/ogg.h>
#include <theora/theora.h>
#include <theora/theoradec.h>

void *sdl_thread(void *args) {
    printf("SDL3 Theora Video Playback Example\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        printf("Unable to initialize SDL: %s\n", SDL_GetError());
        return NULL;
    }
    printf("SDL initialized successfully\n");

    SDL_Window *window = SDL_CreateWindow("Theora Video Playback", BSP_LCD_H_RES, BSP_LCD_V_RES, 0);
    if (!window) {
        printf("Failed to create window: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        printf("Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return NULL;
    }

    // Initialize Ogg and Theora structures
    FILE *videoFile = fopen("/spiffs/video.ogv", "rb");
    if (!videoFile) {
        printf("Failed to open video file\n");
        return NULL;
    }

    ogg_sync_state   oy;
    ogg_page         og;
    ogg_stream_state vo;
    ogg_packet       op;

    th_info          ti;
    th_comment       tc;
    th_setup_info   *ts = NULL;
    th_dec_ctx      *td = NULL;

    int             theora_p = 0;
    int             stateflag = 0;

    ogg_sync_init(&oy);
    th_info_init(&ti);
    th_comment_init(&tc);

    // Read headers
    while (!stateflag) {
        char *buffer = ogg_sync_buffer(&oy, 4096);
        int bytes = fread(buffer, 1, 4096, videoFile);
        ogg_sync_wrote(&oy, bytes);

        if (bytes == 0) {
            printf("End of file while searching for codec headers.\n");
            break;
        }

        while (ogg_sync_pageout(&oy, &og) > 0) {
            ogg_stream_state test;

            if (!ogg_page_bos(&og)) {
                // This is not a header page
                ogg_stream_pagein(&oy, &og);
                stateflag = 1;
                break;
            }

            ogg_stream_init(&test, ogg_page_serialno(&og));
            ogg_stream_pagein(&test, &og);
            ogg_stream_packetout(&test, &op);

            // Identify if this is Theora stream
            if (!theora_p && th_decode_headerin(&ti, &tc, &ts, &op) >= 0) {
                // This is Theora stream
                memcpy(&vo, &test, sizeof(test));
                theora_p = 1;
            } else {
                // Not Theora, discard
                ogg_stream_clear(&test);
            }
        }
    }

    // Begin decoding
    if (theora_p) {
        td = th_decode_alloc(&ti, ts);
        printf("Theora stream is %dx%d\n", ti.pic_width, ti.pic_height);
    } else {
        printf("No Theora stream found\n");
        fclose(videoFile);
        return NULL;
    }

    th_setup_free(ts);

    // Video parameters
    int frame_width = ti.pic_width;
    int frame_height = ti.pic_height;

    // Create SDL texture
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);
    if (!texture) {
        printf("Failed to create texture: %s\n", SDL_GetError());
        th_decode_free(td);
        fclose(videoFile);
        return NULL;
    }

    // Main decoding loop
    int ret;
    SDL_Event event;
    int quit = 0;

    while (!quit) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = 1;
            }
        }

        // Read packets
        while ((ret = ogg_stream_packetout(&vo, &op)) > 0) {
            if (th_decode_packetin(td, &op, NULL) == 0) {
                // Get the decoded frame
                th_ycbcr_buffer ycbcr;
                th_decode_ycbcr_out(td, ycbcr);

                // Lock texture for updating
                void *pixels;
                int pitch;
                if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) {
                    printf("Failed to lock texture: %s\n", SDL_GetError());
                    continue;
                }

                // Convert YCbCr to RGB and copy to texture
                uint8_t *dst = (uint8_t *)pixels;
                for (int y = 0; y < frame_height; y++) {
                    for (int x = 0; x < frame_width; x++) {
                        int y_index = ycbcr[0].stride * y + x;
                        int cb_index = ycbcr[1].stride * (y >> ycbcr[1].height_shift) + (x >> ycbcr[1].width_shift);
                        int cr_index = ycbcr[2].stride * (y >> ycbcr[2].height_shift) + (x >> ycbcr[2].width_shift);

                        int Y = ycbcr[0].data[y_index];
                        int Cb = ycbcr[1].data[cb_index] - 128;
                        int Cr = ycbcr[2].data[cr_index] - 128;

                        // Convert YCbCr to RGB
                        int R = Y + (1.402 * Cr);
                        int G = Y - (0.34414 * Cb) - (0.71414 * Cr);
                        int B = Y + (1.772 * Cb);

                        // Clamp values
                        R = R < 0 ? 0 : (R > 255 ? 255 : R);
                        G = G < 0 ? 0 : (G > 255 ? 255 : G);
                        B = B < 0 ? 0 : (B > 255 ? 255 : B);

                        // Set pixel
                        uint32_t *pixel = (uint32_t *)(dst + y * pitch + x * 4);
                        *pixel = SDL_MapRGB(SDL_PIXELFORMAT_RGB888, R, G, B);
                    }
                }

                SDL_UnlockTexture(texture);

                // Render the frame
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        }

        // Read more data from the file if needed
        if (ogg_sync_pageout(&oy, &og) == 0) {
            char *buffer = ogg_sync_buffer(&oy, 4096);
            int bytes = fread(buffer, 1, 4096, videoFile);
            ogg_sync_wrote(&oy, bytes);
            if (bytes == 0) {
                // End of file
                printf("End of video file.\n");
                quit = 1;
                break;
            }
        }

        // Insert the page into the stream
        if (ogg_sync_pageout(&oy, &og) > 0) {
            ogg_stream_pagein(&vo, &og);
        }

        // Control playback speed according to frame rate
        vTaskDelay(pdMS_TO_TICKS(1000 / ti.fps_numerator * ti.fps_denominator));
    }

    // Clean up
    th_decode_free(td);
    ogg_stream_clear(&vo);
    ogg_sync_clear(&oy);
    th_info_clear(&ti);
    th_comment_clear(&tc);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    fclose(videoFile);

    return NULL;
}

void app_main(void) {
    pthread_t sdl_pthread;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 65536);  // Increase stack size for decoding

    int ret = pthread_create(&sdl_pthread, &attr, sdl_thread, NULL);
    if (ret != 0) {
        printf("Failed to create SDL thread: %d\n", ret);
        return;
    }

    pthread_detach(sdl_pthread);
}

