#include "../source/fichier.c"
#include <assert.h>

int main(void) {
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    const char *share = "https://megaup.net/sample/hello.txt";
    const char *gate = "https://download.megaup.net/?url=signed&name=hello.txt";
    const char *storage = "https://e7.megaupdownup.org/sample/hello.txt?x=1&download_token=signed";
    char next[URL_CAPACITY] = {0};

    assert(megaup_url(NULL, share, next) == MEGAUP_SHARE);
    assert(megaup_url(NULL, storage, next) == MEGAUP_FILE);
    assert(megaup_url(NULL, "http://megaup.net/sample/file", next) == MEGAUP_INVALID);
    assert(megaup_url(NULL, "https://user@megaup.net/sample/file", next) == MEGAUP_INVALID);
    assert(megaup_url(NULL, "https://megaup.net:8443/sample/file", next) == MEGAUP_INVALID);
    assert(megaup_url(NULL, "https://evilmegaupdownup.org/file?download_token=a", next) == MEGAUP_INVALID);
    assert(megaup_url(NULL, "https://e7.megaupdownup.org/file?other=download_token=a", next) == MEGAUP_INVALID);
    assert(megaup_url(NULL, "https://e7.megaupdownup.org/file?download_token=&x=a", next) == MEGAUP_INVALID);

    char share_html[] =
        "<script>$('.download-timer').html(\"<a href='https://download.megaup.net/"
        "?url=signed&amp;name=hello.txt'>Download</a>\");</script>";
    Transfer page = { .html = share_html, .length = strlen(share_html) };
    assert(megaup_action(&page, share, MEGAUP_SHARE, next) == MEGAUP_GATE);
    assert(!strcmp(next, gate));

    char gate_html[] =
        "<a href='https://megaup.net.evil.test/file?pt=bad'>Ad</a>"
        "<a href='https://megaup.net/sample?pt=signed%3D'>Continue</a>";
    page.html = gate_html;
    page.length = strlen(gate_html);
    assert(megaup_action(&page, gate, MEGAUP_GATE, next) == MEGAUP_CONTINUE);
    assert(!strcmp(next, "https://megaup.net/sample?pt=signed%3D"));
    assert(megaup_url(next, storage, next) == MEGAUP_FILE);

    char dead[] = "<title>File Not Found</title>";
    page.html = dead;
    page.status = 200;
    assert(strstr(megaup_error(&page, MEGAUP_SHARE), "shared file"));
    page.status = 403;
    assert(strstr(megaup_error(&page, MEGAUP_GATE), "verification"));

    curl_global_cleanup();
    puts("MegaUp URL and HTML fixtures passed.");
    return 0;
}
