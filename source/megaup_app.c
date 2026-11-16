#include "fichier.h"

#include <curl/curl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define LINK_FILE "sdmc:/switch/MegaUpDownloader/link.txt"
#define LINK_SIZE 2048
#define HEADING "MegaUp Downloader 0.1.0\n\n"
#define CONTROLS "A: enter MegaUp link   X: read link.txt   +: exit\n"
#define OUTPUT_LABEL "microSD: /switch/MegaUpDownloader/\n"

static bool input_link(char link[LINK_SIZE]) {
    SwkbdConfig keyboard;
    Result result = swkbdCreate(&keyboard, 0);
    if (R_FAILED(result)) {
        printf("Keyboard error: 0x%08x\n", (unsigned)result);
        return false;
    }

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, "MegaUp");
    swkbdConfigSetGuideText(&keyboard, "Enter a https://megaup.net/ link");
    swkbdConfigSetOkButtonText(&keyboard, "Download");
    swkbdConfigSetStringLenMax(&keyboard, 500);
    result = swkbdShow(&keyboard, link, LINK_SIZE);
    swkbdClose(&keyboard);
    return R_SUCCEEDED(result) && link[0];
}

static bool read_link(char link[LINK_SIZE]) {
    FILE *file = fopen(LINK_FILE, "r");
    if (!file) {
        printf("Put a MegaUp link on the first line of\n/switch/MegaUpDownloader/link.txt\n");
        return false;
    }
    bool read = fgets(link, LINK_SIZE, file) != NULL;
    fclose(file);
    if (!read || strlen(link) >= LINK_SIZE - 1) {
        printf("link.txt is empty or its first line is too long.\n");
        return false;
    }
    link[strcspn(link, "\r\n")] = 0;
    return link[0];
}

static void worker(void *arg) {
    fichier_download(arg);
}

static void show_progress(const FichierJob *job) {
    uint_fast64_t downloaded = atomic_load(&job->received);
    uint_fast64_t total = atomic_load(&job->total);
    unsigned waiting = atomic_load(&job->waiting);
    consoleClear();
    printf(HEADING);
    if (atomic_load(&job->cancelled)) printf("Cancelling...\n");
    else if (waiting) printf("Waiting for MegaUp: %u s\n", waiting);
    else if (!downloaded) printf("Contacting MegaUp...\n");
    else if (total) printf("Received: %" PRIuFAST64 " / %" PRIuFAST64 " bytes\n", downloaded, total);
    else printf("Received: %" PRIuFAST64 " bytes\n", downloaded);
    printf("\nB: cancel\n");
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    Result sockets = socketInitializeDefault();
    CURLcode http = R_SUCCEEDED(sockets) ?
        curl_global_init(CURL_GLOBAL_DEFAULT) : CURLE_FAILED_INIT;

    printf(HEADING);
    if (http) printf("Network initialization failed.\n\n+: exit\n");
    else printf(OUTPUT_LABEL "\n" CONTROLS);

    char link[LINK_SIZE] = {0};
    FichierJob job = { .url = link, .megaup = true, .megaup_native = true };
    atomic_init(&job.cancelled, false);
    atomic_init(&job.received, 0);
    atomic_init(&job.total, 0);
    atomic_init(&job.waiting, 0);
    Thread thread;
    bool active = false;
    unsigned frame = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 buttons = padGetButtonsDown(&pad);
        if (!active && (buttons & HidNpadButton_Plus)) break;
        if (active && (buttons & HidNpadButton_B)) atomic_store(&job.cancelled, true);

        if (!active && !http && (buttons & (HidNpadButton_A | HidNpadButton_X))) {
            consoleClear();
            printf(HEADING);
            link[0] = 0;
            bool accepted = (buttons & HidNpadButton_A) ? input_link(link) : read_link(link);
            if (accepted) {
                atomic_store(&job.cancelled, false);
                atomic_store(&job.received, 0);
                atomic_store(&job.total, 0);
                atomic_store(&job.waiting, 0);
                job.http = 0;
                job.stage = NULL;
                job.path[0] = 0;
                frame = 0;

                Result result = threadCreate(&thread, worker, &job, NULL, 128 * 1024, 0x2c, -2);
                if (R_SUCCEEDED(result)) {
                    result = threadStart(&thread);
                    if (R_SUCCEEDED(result)) active = true;
                    else threadClose(&thread);
                }
                if (active) show_progress(&job);
                else printf("Could not start download: 0x%08x\n", (unsigned)result);
            }
            if (!active) printf("\n" CONTROLS);
        }

        if (active && R_SUCCEEDED(waitSingle(waiterForThread(&thread), 0))) {
            threadWaitForExit(&thread);
            threadClose(&thread);
            active = false;
            consoleClear();
            printf(HEADING "%s\n", job.result);
            if (job.stage) printf("Stage: %s\n", job.stage);
            if (job.http) printf("HTTP:  %ld\n", job.http);
            if (!strcmp(job.result, "Download saved")) printf("%s\n", job.path);
            printf("\n" CONTROLS);
        } else if (active && ++frame % 15 == 0) {
            show_progress(&job);
        }

        consoleUpdate(NULL);
    }

    if (active) {
        atomic_store(&job.cancelled, true);
        threadWaitForExit(&thread);
        threadClose(&thread);
    }
    if (!http) curl_global_cleanup();
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return 0;
}
