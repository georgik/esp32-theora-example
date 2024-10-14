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
#include "filesystem.h"

void *sdl_thread(void *args) {
    printf("SDL3 Theora Video Playback Example\n");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
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

    SDL_InitFS();

    // Clear screen
    SDL_SetRenderDrawColor(renderer, 88, 66, 255, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    // Initialize Ogg and Theora structures
    FILE *videoFile = fopen("/assets/video.ogv", "rb");
    if (!videoFile) {
        printf("Failed to open video file\n");
        return NULL;
    }

    ogg_sync_state   oy;
    ogg_page         og;
    ogg_stream_state vo;  // Initialize without copying
    ogg_packet       op;

    th_info          ti;
    th_comment       tc;
    th_setup_info   *ts = NULL;
    th_dec_ctx      *td = NULL;

    int             theora_p = 0;
    int             theora_serialno = -1;

    ogg_sync_init(&oy);
    th_info_init(&ti);
    th_comment_init(&tc);

    // Read headers
    int headers_read = 0;
    int done = 0;

    while (!done) {
        // Read data from file into ogg_sync_state
        char *buffer = ogg_sync_buffer(&oy, 4096);
        int bytes = fread(buffer, 1, 4096, videoFile);
        ogg_sync_wrote(&oy, bytes);

        printf("Read %d bytes into Ogg sync buffer.\n", bytes);

        if (bytes == 0 && ogg_sync_pageout(&oy, &og) <= 0) {
            printf("End of file reached before finding all Theora headers.\n");
            return NULL;
        }

        // Extract pages from ogg_sync_state
        while (ogg_sync_pageout(&oy, &og) > 0) {
            printf("Extracted page from Ogg sync buffer. Serial no: %d\n", ogg_page_serialno(&og));

            if (ogg_page_bos(&og)) {
                printf("Found BOS (Beginning of Stream) page.\n");

                ogg_stream_state test;
                ogg_stream_init(&test, ogg_page_serialno(&og));
                ogg_stream_pagein(&test, &og);

                // Extract packets from test stream
                while (ogg_stream_packetout(&test, &op) > 0) {
                    int ret = th_decode_headerin(&ti, &tc, &ts, &op);
                    if (ret >= 0) {
                        // Identified Theora stream
                        printf("Identified Theora stream with serial no: %d\n", ogg_page_serialno(&og));
                        theora_p = 1;
                        theora_serialno = ogg_page_serialno(&og);

                        // Initialize main Theora stream state
                        ogg_stream_init(&vo, theora_serialno);
                        ogg_stream_pagein(&vo, &og);

                        headers_read++;

                        // Read the rest of the Theora headers
                        while (headers_read < 3) {
                            // Attempt to extract packet
                            if (ogg_stream_packetout(&vo, &op) > 0) {
                                ret = th_decode_headerin(&ti, &tc, &ts, &op);
                                if (ret < 0) {
                                    printf("Error parsing Theora header packet %d; ret=%d\n", headers_read + 1, ret);
                                    return NULL;
                                }
                                printf("Parsed Theora header packet %d.\n", headers_read + 1);
                                headers_read++;
                            } else {
                                // Need more data
                                break;
                            }
                        }

                        if (headers_read == 3) {
                            printf("All Theora headers parsed successfully.\n");
                            done = 1;
                        }
                        ogg_stream_clear(&test);
                        break;
                    } else {
                        // Not a Theora stream
                        printf("Stream is not Theora; ret = %d. Clearing test stream.\n", ret);
                        ogg_stream_clear(&test);
                        break;
                    }
                }
            } else {
                // Non-BOS page
                if (theora_p && ogg_page_serialno(&og) == theora_serialno) {
                    ogg_stream_pagein(&vo, &og);

                    // Read headers
                    while (headers_read < 3 && ogg_stream_packetout(&vo, &op) > 0) {
                        int ret = th_decode_headerin(&ti, &tc, &ts, &op);
                        if (ret < 0) {
                            printf("Error parsing Theora header packet %d; ret=%d\n", headers_read + 1, ret);
                            return NULL;
                        }
                        printf("Parsed Theora header packet %d.\n", headers_read + 1);
                        headers_read++;
                        if (headers_read == 3) {
                            printf("All Theora headers parsed successfully.\n");
                            done = 1;
                        }
                    }
                } else {
                    // Ignore pages from other streams
                    printf("Ignoring page from another stream with serial no: %d\n", ogg_page_serialno(&og));
                }
            }
        }
    }

    if (theora_p) {
        td = th_decode_alloc(&ti, ts);
        printf("Theora stream is %lux%lu\n", (unsigned long)ti.pic_width, (unsigned long)ti.pic_height);
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
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);
    if (!texture) {
        printf("Failed to create texture: %s\n", SDL_GetError());
        th_decode_free(td);
        fclose(videoFile);
        return NULL;
    }

    // Determine chroma subsampling shifts
    int x_shift = (ti.pixel_fmt != TH_PF_444) ? 1 : 0;
    int y_shift = (ti.pixel_fmt == TH_PF_420) ? 1 : 0;

    // Main decoding loop
    int ret;
    SDL_Event event;
    int quit = 0;
    int eos = 0;

    while (!quit && !eos) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = 1;
            }
        }

        // Read data from file into ogg_sync_state
        char *buffer = ogg_sync_buffer(&oy, 4096);
        int bytes = fread(buffer, 1, 4096, videoFile);
        ogg_sync_wrote(&oy, bytes);

        if (bytes == 0) {
            // End of file
            eos = 1;
        }

        // Extract all available pages from ogg_sync_state
        while (ogg_sync_pageout(&oy, &og) > 0) {
            // Check if this page is for the Theora stream
            if (ogg_page_serialno(&og) == theora_serialno) {
                ogg_stream_pagein(&vo, &og);

                // Extract all available packets from the stream
                while (ogg_stream_packetout(&vo, &op) > 0) {
                    // Feed packet into decoder
                    if (th_decode_packetin(td, &op, NULL) == 0) {
                        // Got a decoded frame
                        th_ycbcr_buffer ycbcr;
                        th_decode_ycbcr_out(td, ycbcr);

                        // Lock texture for updating
                        void *pixels;
                        int pitch;
                        if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) {
                            printf("Failed to lock texture: %s\n", SDL_GetError());
                            continue;
                        }

                        uint8_t *dst = (uint8_t *)pixels;
                        for (int y = 0; y < frame_height; y++) {
                            uint8_t *row = dst + y * pitch;
                            for (int x = 0; x < frame_width; x++) {
                                int y_index = ycbcr[0].stride * y + x;
                                int y_sub = y >> y_shift;
                                int x_sub = x >> x_shift;
                                int cb_index = ycbcr[1].stride * y_sub + x_sub;
                                int cr_index = ycbcr[2].stride * y_sub + x_sub;

                                int Y = ycbcr[0].data[y_index];
                                int Cb = ycbcr[1].data[cb_index] - 128;
                                int Cr = ycbcr[2].data[cr_index] - 128;

                                // Convert YCbCr to RGB
                                int R = Y + (1.402 * Cr);
                                int G = Y - (0.344136 * Cb) - (0.714136 * Cr);
                                int B = Y + (1.772 * Cb);

                                // Clamp values
                                R = R < 0 ? 0 : (R > 255 ? 255 : R);
                                G = G < 0 ? 0 : (G > 255 ? 255 : G);
                                B = B < 0 ? 0 : (B > 255 ? 255 : B);

                                // Set pixel (RGB24 format)
                                uint8_t *pixel = row + x * 3;
                                pixel[0] = R;
                                pixel[1] = G;
                                pixel[2] = B;
                            }
                        }

                        SDL_UnlockTexture(texture);

                        // Render the frame
                        SDL_FRect destRect = {0, 0, (float)frame_width, (float)frame_height};
                        SDL_RenderTexture(renderer, texture, NULL, &destRect);
                        SDL_RenderPresent(renderer);
                    }
                }
            } else {
                printf("Skipping page...\n");
            }

            if (ogg_page_eos(&og)) {
                eos = 1;
            }
        }

        // Control playback speed according to frame rate
        int delay = (int)(1000.0 * ti.fps_denominator / ti.fps_numerator);
        vTaskDelay(pdMS_TO_TICKS(delay));
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
