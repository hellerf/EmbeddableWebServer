/* Copyright 2016 Forrest Heller. Released under the 2-clause BSD license in EmbeddableWebServer.h */

/* This is a demo for the Embeddable Web Server. See EmbedabbleWebServer.h and the README for smaller
examples. EWS code in general is written for Linux and Mac OS X and puts the burden of emulation on
Windows, rather than emulating Linux/OS X interfaces. */

#include "EmbeddableWebServer.h"

#ifdef WIN32
#pragma comment(lib, "ws2_32")
static void usleep(DWORD microseconds);
void gettimeofday(struct timeval* tv, const void* unusedTimezone);
#else
#include <sys/time.h>
#endif

static struct Server server = {0};

static THREAD_RETURN_TYPE STDCALL_ON_WIN32 stopAcceptingConnections(void* u) {
    serverStop(&server);
    return (THREAD_RETURN_TYPE) NULL;
}

static void writeDemoFiles();

int main(int argc, const char * argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        int portScanned = port;
        sscanf(argv[1], "%d", &portScanned);
        port = (uint16_t) portScanned;
    }
    printf("Running unit tests...\n");
    EWSUnitTestsRun();
    printf("Unit tests passed. Accepting connections from everywhere...\n");
    serverInit(&server);
    writeDemoFiles();
    acceptConnectionsUntilStoppedFromEverywhereIPv4(&server, port);
    serverDeInit(&server);
    return 0;
}

struct Response* createResponseForRequest(const struct Request* request, struct Connection* connection) {    
    if (request->path == strstr(request->path, "/stop")) {
        pthread_t stopThread;
        pthread_create(&stopThread, NULL, &stopAcceptingConnections, connection->server);
        pthread_detach(stopThread);
    }
    /* Here's an example of how to return a regular dynamic web page */
    if (request->path == strstr(request->path, "/status")) {
        return responseAllocWithFormat(200, "OK", "text/html; charset=UTF-8", "<html><title>Server Stats Page Example</title>"
                                       "Here are some basic measurements and status indicators for this server<br>"
                                       "<table border=\"1\">\n"
                                       "<tr><td>Active connections</td><td>%" PRId64 "</td></tr>\n"
                                       "<tr><td>Total connections</td><td>%" PRId64 " (Remember that most browsers try to get a /favicon)</td></tr>\n"
                                       "<tr><td>Total bytes sent</td><td>%" PRId64 "</td></tr>\n"
                                       "<tr><td>Total bytes received</td><td>%" PRId64 "</td></tr>\n"
                                       "<tr><td>Heap string allocations</td><td>%" PRId64 "</td></tr>\n"
                                       "<tr><td>Heap string reallocations</td><td>%" PRId64 "</td></tr>\n"
                                       "<tr><td>Heap string frees</td><td>%" PRId64 "</td></tr>\n"
                                       "<tr><td>Heap string total bytes allocated</td><td>%" PRId64 "</td></tr>\n"
                                       "</table></html>",
                                       counters.activeConnections,
                                       counters.totalConnections,
                                       counters.bytesSent,
                                       counters.bytesReceived,
                                       counters.heapStringAllocations,
                                       counters.heapStringReallocations,
                                       counters.heapStringFrees,
                                       counters.heapStringTotalBytesReallocated);
    }
    /* This is the home page of the demo, which links to various things */
    if (0 == strcmp(request->path, "/")) {
        struct HeapString connectionDebugInfo = connectionDebugStringCreate(connection);
        struct Response* response = responseAllocWithFormat(200, "OK", "text/html; charset=UTF-8",
                                                            "<html><head><title>Embedded C Web Server Version %s</title></head>"
                                                            "<body>"
                                                            "<h2><img src=\"logo.png\">Embedded C Web Server Version %s</h2>"
                                                            "Welcome to the Embedded C Web Server, a minimal web server that you copy and paste into your application. You can create your own page/app by modifying the <code>createResponseForRequest</code> function and calling <code>responseAllocWithFormat</code>\n"
                                                            "<h2>Check it out</h2>"
                                                            "<a href=\"/status\">Server Status</a><br>"
                                                            "<a href=\"/index.html\">Serve files like a regular web server</a><br>"
                                                            "<a href=\"/random_streaming\">Chunked Streaming / Custom Connection Handling</a><br>"
                                                            "<a href=\"/form_post_demo\">HTML Form POST Demo</a><br>"
                                                            "<a href=\"/form_get_demo\">HTML Form GET Demo</a><br>"
                                                            "<a href=\"/json_status_example\">JSON status example</a><br>"
                                                            "<a href=\"/json_hit_counter\">JSON hit counter</a><br>"
                                                            "<a href=\"/html_hit_counter\">HTML hit counter</a><br>"
                                                            "<a href=\"/about\">About</a><br>"
                                                            "<h2>Connection Debug Info</h2><pre>%s</pre>"
                                                            "</body></html>",
                                                            EMBEDDABLE_WEB_SERVER_VERSION_STRING,
                                                            EMBEDDABLE_WEB_SERVER_VERSION_STRING,
                                                            connectionDebugInfo.contents);
        heapStringFreeContents(&connectionDebugInfo);
        return response;
    }
    
    if (request->path == strstr(request->path, "/form_post_demo")) {
        
        struct HeapString connectionDebugInfo = connectionDebugStringCreate(connection);
        struct Response* response = responseAlloc(200, "OK", "text/html; charset=UTF-8", 0);
        
        heapStringAppendString(&response->body, "<html><head><title>HTML Form POST demo | Embedded C Web Server</title></head>\n"
                               "<body>"
                               "<a href=\"/\">Home</a><br>\n"
                               "<h2>HTML Form POST demo</h2>\n"
                               "Please type a message into the tagbox. Tagboxes were popular on personal websites from the early-2000s. It's like a mini-Twitter for every site.<br>\n");
        char* message = strdupDecodePOSTParam("message=", request, "");
        char* name = strdupDecodePOSTParam("name=", request, "");
        char* action = strdupDecodePOSTParam("action=", request, "");
        
        if (NULL != action && 0 == strcmp(action, "Post") && strlen(message) > 0 && strlen(name) > 0) {
            /* make sure we're the only thread writing this file */
            serverMutexLock(connection->server);
            FILE* messagesFP = fopen("messages.txt", "ab");
            if (NULL != messagesFP) {
                fprintf(messagesFP, "%s\t%s\n", name, message);
                fclose(messagesFP);
            } else {
                heapStringAppendFormat(&response->body, "<font color=\"red\">Could not open 'messages.txt' for writing. %s = %d</font><br>", strerror(errno), errno);
            }
            serverMutexUnlock(connection->server);
        } else if (NULL != action && 0 == strcmp(action, "Clear All Messages")) {
            unlink("messages.txt");
        }
        free(action);
        /* we don't want to access this file from multiple threads. It's probably safer
         just to use something like flock */
        serverMutexLock(connection->server);
        /* open the messages file and read out the messages, creating an HTML table along the way */
        FILE* messagesFP = fopen("messages.txt", "rb");
        if (NULL != messagesFP) {
            heapStringAppendString(&response->body, "<strong>Messages</string><br>"
                                   "<table border=\"1\" cellspacing=\"1\" cellpadding=\"1\">");
            int c;
            bool startingNextMessage = true;
            bool grayBackground = false;
            while (EOF != (c = fgetc(messagesFP))) {
                if ('\t' == c) { // end of name, start of message
                    heapStringAppendString(&response->body, "</td><td>");
                } else if ('\n' == c) { // end of message
                    heapStringAppendString(&response->body, "</td></tr>\n");
                    startingNextMessage = true;
                } else {
                    if (startingNextMessage) {
                        heapStringAppendFormat(&response->body, "<tr style=\"background-color:%s;\"><td>", grayBackground ? "#DDDDDD" : "#FFFFFF");
                        grayBackground = !grayBackground;
                        startingNextMessage = false;
                    }
                    heapStringAppendChar(&response->body, (char) c);
                }
            }
            heapStringAppendString(&response->body, "</table>");
            fclose(messagesFP);
        }
        serverMutexUnlock(connection->server);
        char* nameEncoded = strdupEscapeForHTML(name);
        char* messageEncoded = strdupEscapeForHTML(message);
        heapStringAppendFormat(&response->body,
                               "<form action=\"/form_post_demo\" method=\"POST\">\n"
                               "<table>\n"
                               "<tr><td>Name</td><td><input type=\"text\" name=\"name\" value=\"%s\"></td></tr>\n"
                               "<tr><td>Message</td><td><input type=\"text\" name=\"message\" value=\"%s\"></td></tr>\n"
                               "<tr><td><input type=\"submit\" name=\"action\" value=\"Post\"></td></tr>\n"
                               "<tr><td><input type=\"submit\" name=\"action\" value=\"Clear All Messages\"></td></tr>\n"
                               "</table>\n<pre>", nameEncoded, messageEncoded);
        heapStringAppendHeapString(&response->body, &connectionDebugInfo);
        heapStringAppendString(&response->body, "</pre></body></html>\n");
        
        free(name);
        free(nameEncoded);
        free(message);
        free(messageEncoded);
        heapStringFreeContents(&connectionDebugInfo);
        return response;
    }
    
    if (request->path == strstr(request->path, "/form_get_demo")) {
        struct Response* response = responseAllocHTML("<html><head><title>GET Demo | Embedded C Web Server</title></head>\n");
        heapStringAppendString(&response->body, "<body><a href=\"/\">Home</a><br><form action=\"form_get_demo\" method=\"GET\">\n"
                               "How long should this page delay before returning to you? <input type=\"text\" name=\"delay_in_milliseconds\" value=\"1000\"> milliseconds<br>\n"
                               "<input type=\"submit\" value=\"Does it work?\"></form>\n");
        char* delayTimeString = strdupDecodeGETParam("delay_in_milliseconds=", request, "0");
        int delayTime = 0;
        sscanf(delayTimeString, "%d", &delayTime);
        free(delayTimeString);
        struct timeval startSleep, endSleep;
        gettimeofday(&startSleep, NULL);
        usleep(delayTime * 1000);
        gettimeofday(&endSleep, NULL);
        
        int64_t startSleepMicroseconds = ((startSleep.tv_sec * 1000 * 1000) + startSleep.tv_usec);
        int64_t endSleepMicroseconds = ((endSleep.tv_sec * 1000 * 1000) + endSleep.tv_usec);
        int64_t differenceMicroseconds = (endSleepMicroseconds - startSleepMicroseconds);
        int64_t differenceMilliseconds64 = differenceMicroseconds / 1000;
        long differenceMillisecondsL = (long) differenceMilliseconds64;
        
        heapStringAppendFormat(&response->body, "We delayed for ~%ld milliseconds\n", differenceMillisecondsL);
        heapStringAppendString(&response->body, "</body></html>");
        return response;
    }
    
    if (0 == strcmp(request->path, "/json_status_example"))
    {
        /* advanced JSON support - we could have used responseAllocWithFormat but
         I wanted to show it's easy to use regular C strings */
        char jsonStatus[512];
        sprintf(jsonStatus, "{\n"
                "\t\"active_connections\" : %" PRId64 ",\n"
                "\t\"total_connections\" : %" PRId64 ",\n"
                "\t\"total_bytes_sent\" : %" PRId64 ",\n"
                "\t\"total_bytes_received\" : %" PRId64 ",\n"
                "\t\"heap_string_allocations\" : %" PRId64 ",\n"
                "\t\"heap_string_reallocations\" : %" PRId64 ",\n"
                "\t\"heap_string_frees\" : %" PRId64 ",\n"
                "\t\"heap_string_total_bytes_allocated\" : %" PRId64 "\n"
                "}",
                counters.activeConnections,
                counters.totalConnections,
                counters.bytesSent,
                counters.bytesReceived,
                counters.heapStringAllocations,
                counters.heapStringReallocations,
                counters.heapStringFrees,
                counters.heapStringTotalBytesReallocated);
        struct Response* response = responseAllocWithFormat(200, "OK", "application/json", "%s" , jsonStatus);
        return response;
    }
    
    if (request->path == strstr(request->path, "/about")) {
        return responseAllocHTMLWithFormat("<html><head><title>About</title><body>Embeddable Web Server version %s by Forrest Heller</body></html>", EMBEDDABLE_WEB_SERVER_VERSION_STRING);
    }

    if (request->path == strstr(request->path, "/json_hit_counter")) {
        serverMutexLock(connection->server);
        long count = 0;
        FILE* fp = fopen("EWSDemoFiles/hitcounter.txt", "rb");
        if (NULL != fp) {
            fscanf(fp, "%ld", &count);
            fclose(fp);
        }
        count++;
        fp = fopen("EWSDemoFiles/hitcounter.txt", "wb");
        fprintf(fp, "%ld", count);
        fclose(fp);
        serverMutexUnlock(connection->server);
        return responseAllocJSONWithFormat("{ \"hits\" : %ld }", count);
    }

    if (request->path == strstr(request->path, "/html_hit_counter")) {
        serverMutexLock(connection->server);
        long count = 0;
        FILE* fp = fopen("EWSDemoFiles/hitcounter.txt", "rb");
        if (NULL != fp) {
            fscanf(fp, "%ld", &count);
            fclose(fp);
        }
        count++;
        fp = fopen("EWSDemoFiles/hitcounter.txt", "wb");
        fprintf(fp, "%ld", count);
        fclose(fp);
        serverMutexUnlock(connection->server);
        return responseAllocHTMLWithFormat("<html><head><title>Hit Counter</title></head><body>"
            "<a href=\"/\">Home</a><br>"
            "Hit counters were popular on web pages in the late 1990s + early 2000s. Every time someone loaded your web page the hit counter would increase. People had lots of different styles of hit counter with rolling images and animations. It was fun.<br>"
            "<font family=\"Comic Sans MS\" color=\"purple\" size=\"+10\"><b>%ld</b></font>"
            "</body></html>",
            count);
    }

    /* This is an example of how you can take over the HTTP and do whatever you want */
    if (request->path == strstr(request->path, "/random_streaming")) {
        FILE* randomfp = fopen("/dev/urandom", "rb");
        if (NULL == randomfp) {
            return responseAlloc500InternalErrorHTML("The server operating system did not let us open /dev/urandom. This happens on Windows.");
        }
        // take over the connection and used chunked transfer
        const char headers[] = "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: application/binary\r\n"; // <-- notice only 1 \r\n! The next one will be the start of chunkTerminationAndHeader
        
        send(connection->socketfd, headers, strlen(headers), 0);
        char* sizeInBytesDecoded = strdupDecodeGETParam("size_in_bytes=", request, "1000000");
        long sizeInBytes = 0;
        sscanf(sizeInBytesDecoded, "%ld", &sizeInBytes);
        free(sizeInBytesDecoded);
        if (sizeInBytes <= 0) {
            return responseAlloc400BadRequestHTML("You specified a bad size_in_bytes. It needs to be positive");
        }
        size_t randomBytesSent = 0;
        while (randomBytesSent < sizeInBytes) {
            size_t bytesToSend = MIN(sizeof(connection->sendRecvBuffer), sizeInBytes - randomBytesSent);
            fread(connection->sendRecvBuffer, 1, bytesToSend, randomfp);
            char chunkTerminationAndHeader[20];
            sprintf(chunkTerminationAndHeader, "\r\n%lx\r\n", (long) bytesToSend);
            send(connection->socketfd, chunkTerminationAndHeader, strlen(chunkTerminationAndHeader), 0);
            send(connection->socketfd, connection->sendRecvBuffer, bytesToSend, 0);
            randomBytesSent += bytesToSend;
        }
        const char emptyTransferChunk[] = "0\r\n\r\n";
        send(connection->socketfd, emptyTransferChunk, strlen(emptyTransferChunk), 0);
        return NULL;
    }

    return responseAllocServeFileFromRequestPath("/", request->path, request->pathDecoded, "EWSDemoFiles");
}

#define MASK(high, low) ((1 << (high - low + 1)) - 1)
#define BITS(value, high, low) ((value >> low) & MASK(high, low))

static void fput_utf8_c(FILE* fp, uint32_t c) {
    if (c >= 0x10000) {
        fputc(0xf0 | BITS(c, 20, 18), fp);
        fputc(0x80 | BITS(c, 17, 12), fp);
        fputc(0x80 | BITS(c, 11, 6), fp);
        fputc(0x80 | BITS(c, 5, 0), fp);
    } else if (c >= 0x800) {
        fputc(0xe0 | BITS(c, 15, 12), fp);
        fputc(0x80 | BITS(c, 11, 6), fp);
        fputc(0x80 | BITS(c, 5, 0), fp);
    } else if (c >= 0x80) {
        fputc(0xc0 | BITS(c, 10, 6), fp);
        fputc(0x80 | BITS(c, 5, 0), fp);
    } else {
        fputc(c, fp);
    }
}

static void writeDemoFiles() {
    serverMutexLock(&server);
    // cross-platform mkdir yay
    system("mkdir EWSDemoFiles"); 
    FILE* fp;
    fp = fopen_utf8_path("EWSDemoFiles/美丽的妻子.html", "wb");
    if (NULL == fp) {
        ews_printf("Could not write demo files\n");
        return;
    }
    fprintf(fp, "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html;charset=UTF-8\"><title>早上好 - Good Morning</title></head>"
        "<body><h1>你好老武</h1>This page is encoded in UTF-8. It has a Content-Type specifying UTF-8 so it should show up correctly.</body></html>");
    fclose(fp);
    fp = fopen("EWSDemoFiles/emoji.html", "wb");
    if (NULL == fp) {
        ews_printf("Could not write demo files\n");
        return;
    }
    fprintf(fp, "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html;charset=UTF-8\"><title>Emoji Page</title></head><body>");
    // from http://www.unicode.org/emoji/charts/full-emoji-list.html
    fprintf(fp, "Here's some emoji in UTF-8 encoding:<br>");
    fprintf(fp, "<br>Smiley:"); fput_utf8_c(fp, 0x1f600);
    fprintf(fp, "<br>Grin:"); fput_utf8_c(fp, 0x1f601);
    fprintf(fp, "<br>Cry Laughing:"); fput_utf8_c(fp, 0x1f602);
    fprintf(fp, "<br>Less Smiley:"); fput_utf8_c(fp, 0x1f603);
    fprintf(fp, "<br>Sweating Smiley:"); fput_utf8_c(fp, 0x1f605);
    fprintf(fp, "<br>Wink:"); fput_utf8_c(fp, 0x1f609);
    fprintf(fp, "<br>Frown:"); fput_utf8_c(fp, 0x2639);
    fprintf(fp, "<br>Division Sign:"); fput_utf8_c(fp, 0xf7);
    fprintf(fp, "<br>Micro:"); fput_utf8_c(fp, 0xb5);
    fprintf(fp, "</body></html>");
    fclose(fp);
    static const uint8_t EWSPNGLogo[] = { 137,
        80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 64, 0, 0, 0, 64, 8, 2, 0, 0, 0, 37, 11, 230, 137, 0, 0, 0, 1, 115, 82, 71, 66, 0, 174, 206, 28, 233, 0, 0, 0, 4, 103,
        65, 77, 65, 0, 0, 177, 143, 11, 252, 97, 5, 0, 0, 0, 9, 112, 72, 89, 115, 0, 0, 14, 195, 0, 0, 14, 195, 1, 199, 111, 168, 100, 0, 0, 0, 24, 116, 69, 88, 116, 83, 111, 102, 116, 119, 97, 114, 101, 0, 112,
        97, 105, 110, 116, 46, 110, 101, 116, 32, 52, 46, 48, 46, 51, 140, 230, 151, 80, 0, 0, 3, 93, 73, 68, 65, 84, 104, 67, 237, 150, 63, 139, 19, 81, 20, 197, 83, 166, 240, 3, 164, 72, 41, 178, 69, 10, 107, 209, 62, 160,
        69, 132, 253, 0, 130, 8, 178, 96, 23, 108, 211, 137, 93, 192, 197, 218, 194, 210, 210, 210, 214, 198, 202, 50, 141, 144, 194, 50, 72, 144, 8, 81, 20, 174, 191, 117, 30, 33, 220, 121, 239, 205, 187, 51, 230, 198, 63, 123, 56, 44,
        147, 153, 123, 222, 158, 147, 220, 119, 223, 244, 228, 47, 199, 101, 128, 99, 227, 255, 9, 176, 94, 203, 249, 185, 76, 38, 50, 28, 74, 175, 119, 65, 46, 248, 200, 77, 30, 29, 15, 5, 1, 182, 91, 153, 205, 164, 223, 15, 190, 235,
        228, 17, 5, 148, 29, 3, 77, 1, 150, 75, 25, 141, 180, 227, 40, 41, 163, 216, 29, 217, 0, 24, 26, 12, 180, 209, 12, 41, 118, 207, 144, 14, 64, 75, 20, 126, 247, 251, 68, 226, 219, 75, 233, 0, 180, 181, 50, 87, 72, 132,
        142, 72, 4, 96, 176, 100, 118, 109, 158, 8, 29, 231, 82, 34, 0, 195, 81, 217, 50, 17, 185, 23, 18, 1, 24, 240, 202, 147, 137, 200, 189, 144, 8, 176, 59, 173, 218, 17, 185, 23, 18, 1, 148, 161, 22, 244, 194, 191, 26, 160,
        99, 11, 113, 162, 121, 33, 17, 96, 60, 214, 158, 76, 68, 238, 133, 68, 128, 249, 92, 123, 50, 17, 185, 23, 18, 1, 86, 171, 78, 7, 25, 114, 47, 164, 119, 219, 116, 170, 157, 21, 18, 161, 35, 210, 1, 54, 27, 57, 57, 209,
        230, 26, 137, 4, 161, 35, 210, 1, 192, 98, 97, 126, 157, 70, 98, 196, 250, 179, 156, 191, 148, 201, 153, 12, 111, 73, 239, 218, 5, 185, 224, 35, 55, 121, 212, 136, 108, 0, 128, 161, 194, 223, 129, 50, 163, 251, 237, 87, 153, 61,
        147, 62, 239, 236, 191, 124, 215, 201, 35, 10, 40, 203, 160, 41, 0, 160, 37, 104, 235, 204, 158, 230, 17, 5, 198, 206, 89, 126, 148, 209, 109, 237, 56, 74, 202, 40, 78, 161, 32, 64, 5, 6, 11, 195, 145, 1, 191, 107, 42, 46,
        248, 200, 77, 251, 204, 193, 208, 224, 134, 54, 154, 33, 197, 169, 12, 197, 1, 126, 31, 104, 137, 194, 239, 126, 159, 72, 162, 189, 116, 132, 0, 180, 181, 50, 87, 72, 132, 117, 120, 7, 96, 176, 100, 118, 109, 158, 8, 235, 115, 201,
        59, 0, 195, 81, 217, 50, 17, 185, 130, 119, 0, 6, 188, 242, 100, 34, 114, 5, 239, 0, 187, 211, 170, 29, 145, 43, 120, 7, 80, 134, 90, 80, 225, 50, 128, 17, 29, 91, 136, 19, 77, 193, 59, 192, 248, 190, 246, 100, 34, 114,
        5, 239, 0, 243, 23, 218, 147, 137, 200, 21, 188, 3, 172, 62, 117, 58, 200, 144, 43, 148, 6, 120, 47, 111, 158, 203, 195, 71, 114, 253, 84, 174, 220, 145, 30, 127, 185, 230, 14, 247, 67, 69, 49, 166, 79, 181, 179, 66, 34, 172,
        163, 57, 192, 59, 121, 141, 87, 76, 167, 200, 83, 106, 66, 117, 1, 54, 95, 228, 100, 172, 205, 53, 18, 9, 194, 58, 114, 1, 126, 200, 119, 190, 99, 101, 55, 69, 42, 169, 15, 202, 38, 44, 62, 152, 95, 167, 145, 68, 145, 12,
        128, 155, 39, 114, 170, 92, 230, 73, 189, 41, 67, 225, 239, 64, 89, 202, 61, 72, 6, 40, 255, 238, 247, 137, 42, 232, 11, 64, 75, 208, 214, 153, 61, 205, 35, 10, 162, 157, 179, 67, 60, 0, 61, 173, 156, 149, 211, 180, 31, 0,
        131, 133, 225, 200, 128, 223, 53, 21, 23, 124, 228, 102, 125, 230, 212, 17, 15, 144, 223, 181, 121, 162, 13, 171, 184, 32, 18, 128, 201, 168, 60, 89, 217, 98, 182, 182, 70, 36, 64, 187, 238, 223, 167, 105, 39, 116, 68, 36, 64, 151,
        254, 169, 232, 217, 69, 145, 0, 213, 89, 219, 133, 172, 16, 214, 58, 60, 34, 1, 148, 155, 118, 12, 107, 29, 30, 145, 255, 116, 87, 250, 202, 141, 149, 172, 16, 214, 58, 60, 34, 1, 206, 100, 164, 12, 89, 201, 10, 97, 173, 195,
        35, 18, 96, 46, 247, 148, 33, 43, 89, 33, 172, 117, 120, 68, 2, 116, 57, 134, 43, 90, 15, 227, 46, 136, 4, 224, 133, 236, 129, 92, 85, 158, 202, 137, 182, 252, 149, 174, 59, 34, 1, 192, 91, 121, 165, 108, 149, 19, 109, 88,
        197, 5, 241, 0, 160, 221, 78, 240, 236, 254, 10, 201, 0, 223, 100, 251, 88, 110, 42, 127, 121, 82, 143, 42, 232, 189, 144, 12, 0, 112, 83, 254, 59, 80, 233, 239, 30, 228, 2, 84, 160, 167, 243, 123, 154, 167, 206, 125, 191, 143,
        230, 0, 128, 169, 194, 100, 228, 59, 230, 132, 170, 206, 105, 254, 114, 205, 29, 238, 123, 206, 156, 58, 138, 2, 252, 201, 184, 12, 112, 92, 136, 252, 4, 165, 200, 94, 7, 253, 255, 199, 5, 0, 0, 0, 0, 73, 69, 78, 68, 174,
        66, 96, 130, };
    fp = fopen("EWSDemoFiles/logo.png", "wb");
    if (NULL == fp) {
        ews_printf("Could not write demo files\n");
        return;
    }
    fwrite(EWSPNGLogo, 1, sizeof(EWSPNGLogo), fp);
    fclose(fp);
    fp = fopen("EWSDemoFiles/index.html", "wb");
    if (NULL == fp) {
        ews_printf("Could not write demo files\n");
        return;
    }
    time_t t;
    time(&t);
    struct tm* tExploded;
    tExploded = localtime(&t);
    char timeString[256];
    strftime(timeString, sizeof(timeString), "%x %X", tExploded);
    fprintf(fp, "<html><head><title>Index page</title><link rel=\"stylesheet\" href=\"style.css\"></head><body>\n"
        "<marquee><h3>Welcome To My Home Page</h3></marquee>\n"
        "Welcome to this page which was written at %s. The background should be green and the text will be white if the external stylesheet was loaded and served correctly.<br>"
        "<strong>Pages</strong><br>"
        "<a href=\"美丽的妻子.html\">UTF-8 test page - 美丽的妻子.html</a><br>"
        "<a href=\"emoji.html\">Emoji table page</a><br>"
        "<br>"
        "<strong>Server Trivia</strong><br>"
        "sizeof(Connection) - the main connection structure is %ld bytes.<br>"
        "sizeof(Request) - which is inside of the Connection structure is %ld bytes.<br>"
        "</body></html>",
        timeString, (long) sizeof(struct Connection), (long) sizeof(struct Request));
    fclose(fp);
    fp = fopen("EWSDemoFiles/style.css", "wb");
    if (NULL == fp) {
        ews_printf("Could not write demo files\n");
        return;
    }
    fprintf(fp, "body {\n\tbackground-color: green;\n\tcolor:white;\n}");
    fclose(fp);
    serverMutexUnlock(&server);
}

#ifdef WIN32
static void usleep(DWORD microseconds) {
    DWORD milliseconds = microseconds / 1000;
    Sleep(milliseconds);
}

void gettimeofday(struct timeval* tv, const void* unusedTimezone) {
    // whatever we just need tick differences
    ULONGLONG ticks = GetTickCount64();
    tv->tv_sec = ticks / 1000;
    tv->tv_usec = ticks % 1000;
}

#endif

