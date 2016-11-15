// Copyright Forrest Heller 2016. Released under the BSD License
/*
 The default distribution includes a cool rotating Earth GIF I found on Wikipedia: https://en.wikipedia.org/wiki/GIF#/media/File:Rotating_earth_(large).gif
 It is licensed under Create Commons Attribution-Sharalike 3.0 (https://creativecommons.org/licenses/by-sa/3.0/legalcode) */
// The latest version is probably at https://www.forrestheller.com/notes/embeddable-c-web-server.html

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdbool.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/time.h>

/* This is a very simple copy & pastable web server that I tested on Mac OS X and
 Linux. You write your own HTTP handlers using the responseAlloc* functions. */

/*
 Instructions:
 
 Set this to 0 and define your own:
 struct Response* createResponseForRequest(const struct Request* request, const struct Connection* connection) */
#ifndef EWS_DEMO_RESPONSE_HANDLERS
#define EWS_DEMO_RESPONSE_HANDLERS 1
#endif

#ifndef EWS_DEFINE_MAIN
#define EWS_DEFINE_MAIN 1
#endif
/*
 To use:
 1.
 
 It shines when:
 -You need to embed a web server into an application to surface statistics/counters/status
 -You need to run a web server in a bare-bones environment without Python/Apache/etc.
 -You want to see the contents of every request printed out
 -You don't need sophistication
 
 To use:
 1. Optional: Delete the main() from this file and call acceptConnectionsForeverFromEverywhereIPv4
 2. modify the createResponseFromRequest function to add your own handler.
 */

/* History:
 Version 1.0 - released */

// To embed just call one of these functions. They will accept connections and never return
static int acceptConnectionsForeverFromEverywhereIPv4(uint16_t portInHostOrder);
static int acceptConnectionsForever(const struct sockaddr* address, socklen_t addressLength);

/* quick options */
static bool OptionPrintWholeRequest = false;
/* /status page - makes reallocating heap strings take a lock ewww */
static bool OptionIncludeStatusPage = true;
/* do you want this server to serve files from OptionDocumentRoot ? */
static bool OptionFileServingEnabled = true;
char* OptionDocumentRoot = "./"; // it's a global so you can write this from another main()
#define REQUEST_MAX_HEADERS 64
#define REQUEST_HEADERS_MAX_MEMORY (16 * 1024)
#define REQUEST_MAX_BODY_LENGTH (128 * 1024 * 1024)

/* Example usage: */
static void testsRun(); // run tests every time we start the web server
#if EWS_DEFINE_MAIN
int main(int argc, const char * argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        int portScanned = port;
        sscanf(argv[1], "%d", &portScanned);
        port = (uint16_t) portScanned;
    }
    testsRun();
    acceptConnectionsForeverFromEverywhereIPv4(port);
    return 0;
}
#endif

#define EMBEDDABLE_WEB_SERVER_VERSION_STRING "1.0.0"
#define EMBEDDABLE_WEB_SERVER_VERSION 0x00010000 // major = [31:16] minor = [15:8] build = [7:0]

typedef enum  {
    RequestParseStateMethod,
    RequestParseStatePath,
    RequestParseStateVersion,
    RequestParseStateHeaderName,
    RequestParseStateHeaderValue,
    RequestParseStateCR,
    RequestParseStateCRLF,
    RequestParseStateCRLFCR,
    RequestParseStateBody,
    RequestParseStateDone
} RequestParseState;

/* just a calloc'd C string on the heap */
struct HeapString {
    char* contents; // null-terminated
    size_t length;
    size_t capacity;
};

/* a string allocated from the request->headerStringPool */
struct PoolString {
    char* contents; // null-terminated
    size_t length;
};

struct Header {
    struct PoolString name;
    struct PoolString value;
};

struct Request {
    /* null-terminated HTTP method (GET, POST, PUT, ...) */
    char method[64];
    size_t methodLength;
    /* null-terminated HTTP version string (HTTP/1.0) */
    char version[16];
    size_t versionLength;
    /* null-terminated HTTP path/URI ( /index.html?name=Forrest ) */
    char path[1024];
    size_t pathLength;
    /* null-terminated string containing the request body. Used for POST forms and JSON blobs */
    struct HeapString body;
    /* HTTP request headers - use headerInRequest to find the header you're looking for. These used to be a linked list and that worked well, but it seemed overkill */
    struct Header headers[REQUEST_MAX_HEADERS];
    size_t headersCount;
    /* the this->headers point at this string pool */
    char headersStringPool[REQUEST_HEADERS_MAX_MEMORY];
    size_t headersStringPoolOffset;
    /* internal state for the request parser */
    RequestParseState state;
};

#define SEND_RECV_BUFFER_SIZE (16 * 1024)
#define RESPONSE_HEADER_SIZE 1024

/* This contains a full HTTP connection. For every connection, a thread is spawned
 and passed this struct */
struct Connection {
    int socketfd;
    /* Who connected? */
    struct sockaddr_storage remoteAddr;
    socklen_t remoteAddrLength;
    char remoteHost[128];
    char remotePort[20];
    struct Request request;
    /* Just allocate the buffers in the connetcion */
    char sendRecvBuffer[SEND_RECV_BUFFER_SIZE];
    char responseHeader[RESPONSE_HEADER_SIZE];
};

/* these counters exist solely for the purpose of the /status demo. They are declared
 as longs so they can be printf'd in a cross platform way. Otherwise I would declare them
 int64_t's */
static struct Counters {
    pthread_mutex_t lock;
    long bytesReceived;
    long bytesSent;
    long totalConnections;
    long activeConnections;
    long heapStringAllocations;
    long heapStringReallocations;
    long heapStringFrees;
    long heapStringTotalBytesReallocated;
} counters;

/* You create one of these for the server to send. filenameToSend, status, and contentType will be freed so strdup them.
 You can fill out the body field using the heapString* functions. You can also specify a filenameToSend which will be sent using regular file streaming. This is so you don't have to load the entire file into memory all at once to send it.
 */
struct Response {
    int code;
    struct HeapString body;
    char* filenameToSend;
    char* status;
    char* contentType;
};

static struct Server {
    pthread_mutex_t globalMutex; // for convenience
} server;

#ifndef __printflike
#define __printflike(...) // clang (and maybe GCC) has this macro that can check printf/scanf format arguments
#endif

/* You fill in this function. Look at request->path for the requested URI */
struct Response* createResponseForRequest(const struct Request* request, const struct Connection* connection);

/* use these in createResponseForRequest */
static struct Response* responseAlloc(int code, const char* status, const char* contentType, size_t contentsLength);
static struct Response* responseAllocHTML(const char* html);
static struct Response* responseAlloc500InternalErrorHTML(const char* extraInformationOrNull);
static struct Response* responseAlloc404NotFoundHTML(const char* resourcePathOrNull);
static struct Response* responseAllocHTMLWithStatus(int code, const char* status, const char* html);
static struct Response* responseAllocWithFormat(int code, const char* status, const char* contentType, const char* format, ...) __printflike(3, 0);
static struct Response* responseAllocWithFile(const char* filename);

/* Wrappers around strdupDecodeGetorPOSTParam */
static char* strdupDecodeGETParam(const char* paramNameIncludingEquals, const struct Request* request, const char* valueIfNotFound);
static char* strdupDecodePOSTParam(const char* paramNameIncludingEquals, const struct Request* request, const char* valueIfNotFound);
/* You can pass this the request->path for GET or request->body.contents for POST. Accepts NULL for paramString for convenience */
static char* strdupDecodeGETorPOSTParam(const char* paramNameIncludingEquals, const char* paramString, const char* valueIfNotFound);
/* If you want to echo back HTML into the value="" attribute or display some user output this will help you */
static char* strdupEscapeForHTML(const char* stringToEscape);
/* If you have a file you reading/writing across connections you can use this provided pthread mutex so you don't have to make your own */
static int globalMutexLock();
static int globalMutexUnlock();
/* Need to inspect a header in a request? */
static const struct Header* headerInRequest(const char* headerName, const struct Request* request);
/* Get a debug string representing this connection that's easy to print out. wrap it in HTML <pre> tags */
static struct HeapString connectionDebugStringCreate(const struct Connection* connection);
/* Some really basic dynamic string handling. AppendChar and AppendFormat allocate enough memory and
 these strings are null-terminated so you can pass them into sprintf */
static void heapStringInit(struct HeapString* string);
static void heapStringFreeContents(struct HeapString* string);
static void heapStringSetToCString(struct HeapString* heapString, const char* cString);
static void heapStringAppendChar(struct HeapString* string, char c);
static void heapStringAppendFormat(struct HeapString* string, const char* format, ...);
static void heapStringAppendString(struct HeapString* string, const char* stringToAppend);
static void heapStringAppendFormatV(struct HeapString* string, const char* format, va_list ap);
static void heapStringAppendHeapString(struct HeapString* target, const struct HeapString* source);
/* functions that help when serving files */
static const char* MIMETypeFromFile(const char* filename, const uint8_t* contents, size_t contentsLength);

/* Modify this or copy and paste */
#if EWS_DEMO_RESPONSE_HANDLERS
struct Response* createResponseForRequest(const struct Request* request, const struct Connection* connection) {
    /* Here's an example of how to return a regular web page */
    if (request->path == strstr(request->path, "/status")) {
        return responseAllocWithFormat(200, "OK", "text/html; charset=UTF-8", "<html><title>Server Stats Page Example</title>"
                                       "Here are some basic measurements and status indicators for this server<br>"
                                       "<table border=\"1\">\n"
                                       "<tr><td>Active connections</td><td>%ld</td></tr>\n"
                                       "<tr><td>Total connections</td><td>%ld (Remember that most browsers try to get a /favicon)</td></tr>\n"
                                       "<tr><td>Total bytes sent</td><td>%ld</td></tr>\n"
                                       "<tr><td>Total bytes received</td><td>%ld</td></tr>\n"
                                       "<tr><td>Heap string allocations</td><td>%ld</td></tr>\n"
                                       "<tr><td>Heap string reallocations</td><td>%ld</td></tr>\n"
                                       "<tr><td>Heap string frees</td><td>%ld</td></tr>\n"
                                       "<tr><td>Heap string total bytes allocated</td><td>%ld</td></tr>\n"
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
    if (0 == strcmp(request->path, "/")) {
        struct HeapString connectionDebugInfo = connectionDebugStringCreate(connection);
        struct Response* response = responseAllocWithFormat(200, "OK", "text/html; charset=UTF-8",
                                                            "<html><head><title>Embedded C Web Server Version %s</title></head>"
                                                            "<body>"
                                                            "<h2>Embedded C Web Server Version %s</h2>"
                                                            "Welcome to the Embedded C Web Server, a minimal web server that you copy and paste into your application. You can create your own page/app by modifying the <code>createResponseForRequest</code> function and calling <code>responseAllocWithFormat</code>\n"
                                                            "<h2>Check it out</h2>"
                                                            "<a href=\"/status\">Server Status</a><br>"
                                                            "<a href=\"/form_post_demo\">HTML Form POST Demo</a><br>"
                                                            "<a href=\"/form_get_demo\">HTML Form GET Demo</a><br>"
                                                            "<a href=\"/json_status_example\">JSON status example</a><br>"
                                                            "<a href=\"/page.html\">File server example</a><br>"
                                                            "<a href=\"/about\">About</a>"
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
        char* message = strdupDecodePOSTParam("message=", request, NULL);
        char* name = strdupDecodePOSTParam("name=", request, NULL);
        char* action = strdupDecodePOSTParam("action=", request, NULL);
        
        if (NULL != action && 0 == strcmp(action, "Post") && NULL != message && NULL != name) {
            /* make sure we're the only thread writing this file */
            globalMutexLock();
            FILE* messagesFP = fopen("messages.txt", "ab");
            if (NULL != messagesFP) {
                fprintf(messagesFP, "%s\t%s\n", name, message);
                fclose(messagesFP);
            } else {
                heapStringAppendFormat(&response->body, "<font color=\"red\">Could not open 'messages.txt' for writing. %s = %d</font><br>", strerror(errno), errno);
            }
            globalMutexUnlock();
        } else if (NULL != action && 0 == strcmp(action, "Clear All Messages")) {
            unlink("messages.txt");
        }
        free(action);
        /* we don't want to access this file from multiple threads. It's probably safer
         just to use something like flock */
        globalMutexLock();
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
        globalMutexUnlock();
        char* nameEncoded = name ? strdupEscapeForHTML(name) : "";
        char* messageEncoded = message ? strdupEscapeForHTML(message) : "";
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
        
        if (NULL != name) {
            free(name);
            free(nameEncoded);
        }
        if (NULL != message) {
            free(message);
            free(messageEncoded);
        }
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
                "\t\"active_connections\" : %ld,\n"
                "\t\"total_connections\" : %ld,\n"
                "\t\"total_bytes_sent\" : %ld,\n"
                "\t\"total_bytes_received\" : %ld,\n"
                "\t\"heap_string_allocations\" : %ld,\n"
                "\t\"heap_string_reallocations\" : %ld,\n"
                "\t\"heap_string_frees\" : %ld,\n"
                "\t\"heap_string_total_bytes_allocated\" : %ld\n"
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
        return responseAllocHTML("<html><head><title>About</title><body>EmbeddedWebServer version 1.0 by Forrest Heller</body></html>");
    }
    
    if (OptionFileServingEnabled) {
        /* make some test files really quick */
        static bool filesWritten = false;
        if (!filesWritten) {
            FILE* fp;
            fp = fopen("page.html", "wb");
            if (NULL != fp) {
                time_t t;
                time(&t);
                fprintf(fp, "<html><head><title>Index page</title><link rel=\"stylesheet\" href=\"style.css\"></head><body>Welcome to this page which was written at %ld. The background should be purple and the text will be white if the external stylesheet was loaded and served correctly.<br>"
                        "sizeof(Connection) - the main connection structure is %ld bytes.<br>"
                        "sizeof(Request) - which is inside of the Connection structure is %ld bytes.<br>"
                        "<img src=\"Rotating_earth_(large).gif\">\n"
                        "</body></html>",
                        (long) t, (long) sizeof(struct Connection), (long) sizeof(struct Request));
                fclose(fp);
            }
            fp = fopen("style.css", "wb");
            if (NULL != fp) {
                fprintf(fp, "body {\n\tbackground-color: purple;\n\tcolor:white;\n}");
                fclose(fp);
            }
            /* this page rules! http://www.matthewflickinger.com/lab/whatsinagif/bits_and_bytes.asp */
            static const uint8_t GIFHeader[] = {'G', 'I', 'F', '8', '9', 'a'};
            const uint16_t canvasWidth = 200;
            const uint16_t canvasHeight = 200;
            const uint8_t logicalScreenDescriptor[] =
            {canvasWidth & 0xff, canvasWidth >> 8, canvasHeight & 0xff, canvasHeight >> 8, 0x91, 0, 0};
            const uint8_t colorTable[] = {
                0xff, 0xff, 0xff, // white
                0x00, 0x00, 0x00, // black
                0x77, 0x77, 0x77, // gray
                0x00, 0xaa, 0x00, // green
            };
            const uint8_t imageDescriptor[] = {0x2c, 0, 0, 0, 0, canvasWidth & 0xff, canvasWidth >> 8, canvasHeight & 0xff, canvasHeight >> 8};
            filesWritten = true;
        }
        
        const char* path = request->path;
        /* skip over . \ / nonsense. */
        while (*path == '/' || *path == '.' || *path == '\\') {
            path++;
        }
        if (*path == '\0') {
            path = "index.html";
        }
        char* pathWithDocumentRoot = (char*) malloc(strlen(path) + strlen(OptionDocumentRoot) + sizeof('/') + sizeof('\0'));
        sprintf(pathWithDocumentRoot, "%s/%s", OptionDocumentRoot, path);
        struct Response* response = responseAllocWithFile(pathWithDocumentRoot);
        free(pathWithDocumentRoot);
        return response;
    }
    /* !!! Wait! Don't delete this one!  !!!*/
    return responseAllocHTMLWithStatus(404, "Not Found", "<html><head><title>Not found</title>"
                                       "<body><h1>404 - Not found</h1>"
                                       "The URL you requested could not be found");
}
#endif

/* Internal stuff */
#ifndef MIN
#define MIN(a, b) ((a < b) ? a : b)
#endif

static void responseFree(struct Response* response);
static void printIPv4Addresses(uint16_t portInHostOrder);
static struct Connection* connectionAlloc();
static void connectionFree(struct Connection* connection);
static void* connectionHandlerThread(void* connectionPointer);
static void requestParse(struct Request* request, const char* requestFragment, size_t requestFragmentLength);
static size_t heapStringNextAllocationSize(size_t required);
static void poolStringStartNewString(struct PoolString* poolString, struct Request* request);
static void poolStringAppendChar(struct Request* request, struct PoolString* string, char c);
static bool strEndsWith(const char* big, const char* endsWith);

static const struct Header* headerInRequest(const char* headerName, const struct Request* request) {
    for (size_t i = 0; i < request->headersCount; i++) {
        if (0 == strcasecmp(request->headers[i].name.contents, headerName)) {
            return &request->headers[i];
        }
    }
    return NULL;
}

static char* strdupIfNotNull(const char* strToDup) {
    if (NULL == strToDup) {
        return NULL;
    }
    return strdup(strToDup);
}

static char* strdupDecodeGETParam(const char* paramNameIncludingEquals, const struct Request* request, const char* valueIfNotFound) {
    return strdupDecodeGETorPOSTParam(paramNameIncludingEquals, request->path, valueIfNotFound);
}

static char* strdupDecodePOSTParam(const char* paramNameIncludingEquals, const struct Request* request, const char* valueIfNotFound) {
    return strdupDecodeGETorPOSTParam(paramNameIncludingEquals, request->body.contents, valueIfNotFound);
}

typedef enum {
    URLDecodeStateNormal,
    URLDecodeStatePercentFirstDigit,
    URLDecodeStatePercentSecondDigit
} URLDecodeState;

static char* strdupDecodeGETorPOSTParam(const char* paramNameIncludingEquals, const char* paramString, const char* valueIfNotFound) {
    assert(strstr(paramNameIncludingEquals, "=") != NULL && "You have to pass an equals sign after the param name, like 'name='");
    /* The string passed is actually NULL -- this is accepted because it's more convenient */
    if (NULL == paramString) {
        return strdupIfNotNull(valueIfNotFound);
    }
    /* Find the paramString ("name=") */
    const char* paramStart = strstr(paramString, paramNameIncludingEquals);
    if (NULL == paramStart) {
        return strdupIfNotNull(valueIfNotFound);
    }
    /* Ok paramStart points at -->"name=" ; let's make it point at "=" */
    paramStart = strstr(paramStart, "=");
    if (NULL == paramStart) {
        printf("It's very suspicious that we couldn't find an equals sign after searching for '%s' in '%s'\n", paramStart, paramString);
        return strdupIfNotNull(valueIfNotFound);
    }
    /* We need to skip past the "=" */
    paramStart++;
    /* Oh man! End of string is right here */
    if ('\0' == *paramStart) {
        char* empty = (char*) malloc(1);
        empty[0] = '\0';
        return empty;
    }
    /* We found a value. Unescape the URL. This is probably filled with bugs */
    size_t maximumPossibleLength = strlen(paramStart);
    char* decoded = (char*) malloc(maximumPossibleLength + 1);
    size_t deci = 0;
    const char* param = paramStart;
    /* these three vars unescape % escaped things */
    URLDecodeState state = URLDecodeStateNormal;
    char firstDigit = '\0';
    char secondDigit;
    while ('&' != *param && '\0' != *param) {
        switch (state) {
            case URLDecodeStateNormal:
                if ('%' == *param) {
                    state = URLDecodeStatePercentFirstDigit;
                } else if ('+' == *param) {
                    decoded[deci] = ' ';
                    deci++;
                } else {
                    decoded[deci] = *param;
                    deci++;
                }
                break;
            case URLDecodeStatePercentFirstDigit:
                // copy the first digit, get the second digit
                firstDigit = *param;
                state = URLDecodeStatePercentSecondDigit;
                break;
            case URLDecodeStatePercentSecondDigit:
            {
                secondDigit = *param;
                int decodedEscape;
                char hexString[] = {firstDigit, secondDigit, '\0'};
                int items = sscanf(hexString, "%02x", &decodedEscape);
                if (1 == items) {
                    decoded[deci] = (char) decodedEscape;
                    deci++;
                } else {
                    printf("Warning: Unable to decode hex string 0x%s from %s", hexString, paramStart);
                }
                state = URLDecodeStateNormal;
            }
                break;
        }
        param++;
    }
    decoded[deci] = '\0';
    return decoded;
}

static void heapStringReallocIfNeeded(struct HeapString* string, size_t minimumCapacity) {
    if (minimumCapacity <= string->capacity) {
        return;
    }
    /* to avoid many reallocations every time we call AppendChar, round up to the next power of two */
    string->capacity = heapStringNextAllocationSize(minimumCapacity);
    assert(string->capacity > 0 && "We are about to allocate a string with 0 capacity. We should have checked this condition above");
    bool previouslyAllocated = string->contents != NULL;
    string->contents = (char*) realloc(string->contents, string->capacity);
    if (OptionIncludeStatusPage) {
        pthread_mutex_lock(&counters.lock);
        if (previouslyAllocated) {
            counters.heapStringReallocations++;
        } else {
            counters.heapStringAllocations++;
        }
        counters.heapStringTotalBytesReallocated += string->capacity;
        pthread_mutex_unlock(&counters.lock);
        globalMutexUnlock();
    }
    memset(&string->contents[string->length], 0, string->capacity - string->length);
}

static size_t heapStringNextAllocationSize(size_t required) {
    /* most of the small heap string allocations are headers */
    size_t powerOf2 = 128; // most of the small heap string allocations are for head
    while (powerOf2 < required) {
        powerOf2 *= 2;
    }
    return powerOf2;
}

static void heapStringAppendChar(struct HeapString* string, char c) {
    heapStringReallocIfNeeded(string, string->length + 2);
    string->contents[string->length] = c;
    string->length++;
    string->contents[string->length] = '\0';
}

static void heapStringAppendFormat(struct HeapString* string, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    heapStringAppendFormatV(string, format, ap);
    va_end(ap);
}

static void heapStringSetToCString(struct HeapString* heapString, const char* cString) {
    size_t cStringLength = strlen(cString);
    heapStringReallocIfNeeded(heapString, cStringLength + 1);
    memcpy(heapString->contents, cString, cStringLength);
    heapString->length = cStringLength;
    heapString->contents[heapString->length] = '\0';
}

static void heapStringAppendString(struct HeapString* string, const char* stringToAppend) {
    size_t stringToAppendLength = strlen(stringToAppend);
    size_t requiredCapacity = stringToAppendLength + string->length + 1;
    heapStringReallocIfNeeded(string, requiredCapacity);
    memcpy(&string->contents[string->length], stringToAppend, stringToAppendLength);
    string->length += stringToAppendLength;
    string->contents[string->length] = '\0';
}

static void heapStringAppendHeapString(struct HeapString* target, const struct HeapString* source) {
    heapStringReallocIfNeeded(target, target->length + source->length + 1);
    memcpy(&target->contents[target->length], source->contents, source->length);
    target->length += source->length;
    target->contents[target->length] = '\0';
}

static bool heapStringIsSaneCString(const struct HeapString* heapString) {
    if (NULL == heapString->contents) {
        if (heapString->capacity != 0) {
            printf("The heap string %p has a capacity of %ld but it's contents are NULL\n", heapString, (long) heapString->capacity);
            return false;
        }
        if (heapString->length != 0) {
            printf("The heap string %p has a length of %ld but capacity is 0 and contents are NULL\n", heapString, (long) heapString->capacity);
            return false;
        }
        return true;
    }
    if (heapString->capacity <= heapString->length) {
        printf("Heap string %p has probably overwritten invalid memory because the capacity (%ld) is <= length (%ld), which is a big nono. The capacity must always be 1 more than the length since the contents is null-terminated for convenience.\n",
               heapString, heapString->capacity, heapString->length);
        return false;
    }
    
    if (strlen(heapString->contents) != heapString->length) {
        printf("The %p strlen(heap string contents) (%ld) is not equal to heapString length (%ld), which is not correct for a C string. This can be correct if we're sending something like a PNG image which can contain '\\0' characters",
               heapString,
               (long) strlen(heapString->contents),
               (long) heapString->length);
        return false;
    }
    return true;
}

static char* strdupEscapeForHTML(const char* stringToEscape) {
    struct HeapString escapedString;
    heapStringInit(&escapedString);
    size_t stringToEscapeLength = strlen(stringToEscape);
    if (0 == strlen(stringToEscape)) {
        char* empty = (char*) malloc(1);
        *empty = '\0';
        return empty;
    }
    for (size_t i = 0; i < stringToEscapeLength; i++) {
        // this is an excerpt of some things translated by the PHP htmlentities function
        char c = stringToEscape[i];
        switch (c) {
            case '"':
                heapStringAppendFormat(&escapedString, "&quot;");
                break;
            case '&':
                heapStringAppendFormat(&escapedString, "&amp;");
                break;
            case '\'':
                heapStringAppendFormat(&escapedString, "&#039;");
                break;
            case '<':
                heapStringAppendFormat(&escapedString, "&lt;");
                break;
            case '>':
                heapStringAppendFormat(&escapedString, "&gt;");
                break;
            case ' ':
                heapStringAppendFormat(&escapedString, "&nbsp;");
                break;
            default:
                heapStringAppendChar(&escapedString, c);
                break;
        }
    }
    return escapedString.contents;
}

static void heapStringAppendFormatV(struct HeapString* string, const char* format, va_list ap) {
    /* Figure out how many characters it would take to print the string */
    char tmp[4];
    va_list apCopy;
    va_copy(apCopy, ap);
    size_t appendLength = vsnprintf(tmp, 4, format, ap);
    size_t requiredCapacity = string->length + appendLength + 1;
    heapStringReallocIfNeeded(string, requiredCapacity);
    assert(string->capacity >= string->length + appendLength + 1);
    /* perform the actual vsnprintf that does the work */
    size_t actualAppendLength = vsnprintf(&string->contents[string->length], string->capacity - string->length, format, apCopy);
    string->length += appendLength;
    assert(actualAppendLength == appendLength && "We called vsnprintf twice with the same format and value arguments and got different string lengths");
    /* explicitly null terminate in case I messed up the vsnprinf logic */
    string->contents[string->length] = '\0';
}

static void heapStringInit(struct HeapString* string) {
    string->capacity = 0;
    string->contents = NULL;
    string->length = 0;
}

static void heapStringFreeContents(struct HeapString* string) {
    if (NULL != string->contents) {
        assert(string->capacity > 0 && "A heap string had a capacity > 0 with non-NULL contents which implies a malloc(0)");
        free(string->contents);
        string->contents = NULL;
        string->capacity = 0;
        string->length = 0;
        if (OptionIncludeStatusPage) {
            pthread_mutex_lock(&counters.lock);
            counters.heapStringFrees++;
            pthread_mutex_unlock(&counters.lock);
        }
    } else {
        assert(string->capacity == 0 && "Why did a string with a NULL contents have a capacity > 0? This is not correct and may indicate corruption");
    }
}
static struct HeapString connectionDebugStringCreate(const struct Connection* connection) {
    struct HeapString debugString = {0};
    heapStringAppendFormat(&debugString, "%s from %s:%s\n", connection->request.method, connection->remoteHost, connection->remotePort);
    bool firstHeader = true;
    heapStringAppendString(&debugString, "\n*** Request Headers ***\n");
    for (size_t i = 0; i < connection->request.headersCount; i++) {
        if (firstHeader) {
            firstHeader = false;
        }
        const struct Header* header = &connection->request.headers[i];
        heapStringAppendFormat(&debugString, "'%s' = '%s'\n", header->name.contents, header->value.contents);
    }
    if (NULL != connection->request.body.contents) {
        heapStringAppendFormat(&debugString, "\n*** Request Body ***\n%s\n", connection->request.body.contents);
    }
    return debugString;
}

static void poolStringStartNewString(struct PoolString* poolString, struct Request* request) {
    /* always re-initialize the length...just in case */
    poolString->length = 0;
    
    /* this is the first string in the pool */
    if (0 == request->headersStringPoolOffset) {
        poolString->contents = request->headersStringPool;
        return;
    }
    /* the pool string is full - don't initialize anything writable and ensure any writing to this pool string crashes */
    if (request->headersStringPoolOffset >= REQUEST_HEADERS_MAX_MEMORY - 1) { // -1 is because the last pool string needs to be null-terminated
        poolString->length = 0;
        poolString->contents = NULL;
        return;
    }
    /* there's already another string in the pool - we need to skip the null character (the string pool is initialized to 0s by calloc) */
    request->headersStringPoolOffset++;
    poolString->contents = &request->headersStringPool[request->headersStringPoolOffset];
    poolString->length = 0;
}

static void poolStringAppendChar(struct Request* request, struct PoolString* string, char c) {
    if (request->headersStringPoolOffset >= REQUEST_HEADERS_MAX_MEMORY - 1) {
        printf("Warning: header string pool is exhausted\n"); // TODO: this is going to print a lot when the pool is exhausted
        return;
    }
    string->contents[string->length] = c;
    string->length++;
    request->headersStringPoolOffset++;
}

// allocates a response with content = malloc(contentLength + 1) so you can write null-terminated strings to it
static struct Response* responseAlloc(int code, const char* status, const char* contentType, size_t bodyCapacity) {
    struct Response* response = (struct Response*) calloc(1, sizeof(*response));
    response->code = code;
    heapStringInit(&response->body);
    response->body.capacity = bodyCapacity;
    response->body.length = 0;
    if (response->body.capacity > 0) {
        response->body.contents = (char*) calloc(1, response->body.capacity);
        if (OptionIncludeStatusPage) {
            pthread_mutex_lock(&counters.lock);
            counters.heapStringAllocations++;
            pthread_mutex_unlock(&counters.lock);
        }
    }
    response->contentType = strdupIfNotNull(contentType);
    response->status = strdupIfNotNull(status);
    return response;
}

static struct Response* responseAllocHTML(const char* html) {
    return responseAllocHTMLWithStatus(200, "OK", html);
}

static struct Response* responseAllocHTMLWithStatus(int code, const char* status, const char* html) {
    struct Response* response = responseAlloc(200, "OK", "text/html; charset=UTF-8", 0);
    heapStringSetToCString(&response->body, html);
    return response;
}

static struct Response* responseAllocWithFormat(int code, const char* status, const char* contentType, const char* format, ...) {
    struct Response* response = responseAlloc(code, status, contentType, 0);
    // it's less code to just free the response and call vasprintf
    va_list ap;
    va_start(ap, format);
    heapStringAppendFormatV(&response->body, format, ap);
    va_end(ap);
    return response;
}

static struct Response* responseAlloc404NotFoundHTML(const char* resourcePathOrNull) {
    if (NULL == resourcePathOrNull) {
        return responseAllocHTMLWithStatus(404, "Not Found", "<html><head><title>404 Not Found</title></head><body>The resource you specified could not be found</body></html>");
    } else {
        return responseAllocWithFormat(404, "Not Found", "text/html; charset=UTF-8", "<html><head><title>404 Not Found</title></head><body>The resource you specified ('%s') could not be found</body></html>", resourcePathOrNull);
    }
}

static struct Response* responseAlloc500InternalErrorHTML(const char* extraInformationOrNull) {
    if (NULL == extraInformationOrNull) {
        return responseAllocHTMLWithStatus(500, "Internal Error", "<html><head><title>500 Internal Error</title></head><body>There was an internal error while completing your request</body></html>");
    } else {
        return responseAllocWithFormat(500, "Internal Error", "text/html; charset=UTF-8", "<html><head><title>500 Internal Error</title></head><body>There was an internal error while completing your request. %s</body></html>", extraInformationOrNull);
    }
    
}

static struct Response* responseAllocWithFile(const char* filename) {
    struct Response* response = responseAlloc(200, "OK", NULL, 0);
    response->filenameToSend = strdup(filename);
    return response;
}

static void responseFree(struct Response* response) {
    free(response->status);
    heapStringFreeContents(&response->body);
    if (NULL != response->filenameToSend) {
        free(response->filenameToSend);
    }
    free(response->contentType);
    free(response);
}

/* parses a typical HTTP request looking for the first line: GET /path HTTP/1.0\r\n */
static void requestParse(struct Request* request, const char* requestFragment, size_t requestFragmentLength) {
    for (size_t i = 0; i < requestFragmentLength; i++) {
        char c = requestFragment[i];
        switch (request->state) {
            case RequestParseStateMethod:
                if (c == ' ') {
                    request->state = RequestParseStatePath;
                } else if (request->methodLength < sizeof(request->method) - 1) {
                    request->method[request->methodLength] = c;
                    request->methodLength++;
                } else {
                    printf("Warning: request method %s is too long...\n", request->method);
                }
                break;
            case RequestParseStatePath:
                if (c == ' ') {
                    request->state = RequestParseStateVersion;
                } else if (request->pathLength < sizeof(request->path) - 1) {
                    request->path[request->pathLength] = c;
                    request->pathLength++;
                } else {
                    printf("Warning: request path %s is too long...\n", request->path);
                }
                break;
            case RequestParseStateVersion:
                if (c == '\r') {
                    request->state = RequestParseStateCR;
                } else if (request->versionLength < sizeof(request->version) - 1) {
                    request->version[request->versionLength] = c;
                    request->versionLength++;
                } else {
                    printf("Warning: request version %s is too long...\n", request->version);
                }
                break;
            case RequestParseStateHeaderName:
                if (c == ':') {
                    request->state = RequestParseStateHeaderValue;
                } else if (c == '\r') {
                    request->state = RequestParseStateCR;
                } else {
                    if (request->headersCount < REQUEST_MAX_HEADERS) {
                        /* if this is the first character in this header name, then initialize the string pool */
                        if (NULL == request->headers[request->headersCount].name.contents) {
                            poolStringStartNewString(&request->headers[request->headersCount].name, request);
                        }
                        /* store the header name in the string pool */
                        poolStringAppendChar(request, &request->headers[request->headersCount].name, c);
                    }
                }
                break;
            case RequestParseStateHeaderValue:
                /* skip the first space */
                if (c == ' ' && request->headers[request->headersCount].value.length == 0) {
                    continue;
                } else if (c == '\r') {
                    if (request->headersCount < REQUEST_MAX_HEADERS) {
                        request->headersCount++;
                    }
                    request->state = RequestParseStateCR;
                } else {
                    if (request->headersCount < REQUEST_MAX_HEADERS) {
                        /* if this is the first character in this header name, then initialize the string pool */
                        if (NULL == request->headers[request->headersCount].value.contents) {
                            poolStringStartNewString(&request->headers[request->headersCount].value, request);
                        }
                        /* store the header name in the string pool */
                        poolStringAppendChar(request, &request->headers[request->headersCount].value, c);
                    }
                }
                break;
                break;
            case RequestParseStateCR:
                if (c == '\n') {
                    request->state = RequestParseStateCRLF;
                } else {
                    request->state = RequestParseStateHeaderName;
                }
                break;
            case RequestParseStateCRLF:
                if (c == '\r') {
                    request->state = RequestParseStateCRLFCR;
                } else {
                    request->state = RequestParseStateHeaderName;
                    /* this is the first character of the header - replay the HeaderName case so this character gets appended */
                    i--;
                }
                break;
            case RequestParseStateCRLFCR:
                if (c == '\n') {
                    /* assume the request state is done unless we have some Content-Length, which would come from something like a JSON blob */
                    request->state = RequestParseStateDone;
                    const struct Header* contentLengthHeader = headerInRequest("Content-Length", request);
                    if (NULL != contentLengthHeader) {
                        printf("Incoming request has a body of length %s\n", contentLengthHeader->value.contents);
                        long contentLength = 0;
                        if (1 == sscanf(contentLengthHeader->value.contents, "%ld", &contentLength)) {
                            request->body.contents = (char*) calloc(1, contentLength + 1);
                            request->body.capacity = contentLength;
                            request->body.length = 0;
                            request->state = RequestParseStateBody;
                        }
                        
                    } else {
                    }
                } else {
                    request->state = RequestParseStateHeaderName;
                }
                break;
            case RequestParseStateBody:
                request->body.contents[request->body.length] = c;
                request->body.length++;
                if (request->body.length == request->body.capacity) {
                    request->state = RequestParseStateDone;
                }
                break;
            case RequestParseStateDone:
                break;
        }
    }
}


static struct Connection* connectionAlloc() {
    struct Connection* connection = (struct Connection*) calloc(1, sizeof(*connection)); // calloc 0's everything which requestParse depends on
    connection->remoteAddrLength = sizeof(connection->remoteAddr);
    return connection;
}

static void connectionFree(struct Connection* connection) {
    /* free the body */
    heapStringFreeContents(&connection->request.body);
    free(connection);
}
static int acceptConnectionsForeverFromEverywhereIPv4(uint16_t portInHostOrder) {
    /* In order to keep the code really short I've just assumed
     we want to bind to 0.0.0.0, which is all available interfaces.
     What you actually want to do is call getaddrinfo on command line arguments
     to let users specify the interface and port */
    struct sockaddr_in anyInterfaceIPv4 = {0};
    anyInterfaceIPv4.sin_addr.s_addr = INADDR_ANY; // also popular inet_addr("127.0.0.1") which is INADDR_LOOPBACK
    anyInterfaceIPv4.sin_family = AF_INET;
    anyInterfaceIPv4.sin_port = htons(portInHostOrder);
    return acceptConnectionsForever((struct sockaddr*) &anyInterfaceIPv4, sizeof(anyInterfaceIPv4));
}

static int acceptConnectionsForever(const struct sockaddr* address, socklen_t addressLength) {
    char addressHost[256];
    char addressPort[20];
    int nameResult = getnameinfo(address, addressLength, addressHost, sizeof(addressHost), addressPort, sizeof(addressPort), NI_NUMERICHOST | NI_NUMERICSERV);
    if (0 != nameResult) {
        strcpy(addressHost, "Unknown");
        strcpy(addressPort, "Unknown");
    }
    
    int listenerfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenerfd <= 0) {
        printf("Could not create listener socket: %s = %d\n", strerror(errno), errno);
        return 1;
    }
    /* SO_REUSEADDR tells the kernel to re-use the bind address in certain circumstances.
     I've always found when making debug/test servers that I want this option, especially on Mac OS X */
    int result;
    int reuse = 1;
    result = setsockopt(listenerfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (0 != result) {
        printf("Failed to setsockopt SE_REUSEADDR = true with %s = %d. Continuing because we might still succeed...\n", strerror(errno), errno);
    }
    
    result = bind(listenerfd, address, addressLength);
    if (0 != result) {
        printf("Could not bind to %s:%s %s = %d\n", addressHost, addressPort, strerror(errno), errno);
        return 1;
    }
    /* listen for the maximum possible amount of connections */
    result = listen(listenerfd, SOMAXCONN);
    if (0 != result) {
        printf("Could not listen for SOMAXCONN (%d) connections. %s = %d. Continuing because we might still succeed...\n", SOMAXCONN, strerror(errno), errno);
    }
    /* print out the addresses we're listening on. Special-case IPv4 0.0.0.0 bind-to-all-interfaces */
    bool printed = false;
    if (address->sa_family == AF_INET) {
        struct sockaddr_in* addressIPv4 = (struct sockaddr_in*) address;
        if (INADDR_ANY == addressIPv4->sin_addr.s_addr) {
            printIPv4Addresses(ntohs(addressIPv4->sin_port));
            printed = true;
        }
    }
    if (!printed) {
        printf("Listening for connections on %s:%s\n", addressHost, addressPort);
    }
    pthread_mutex_init(&counters.lock, NULL);
    pthread_mutex_init(&server.globalMutex, NULL);
    /* allocate a connection (which sets connection->remoteAddrLength) and accept the next inbound connection */
    struct Connection* nextConnection = connectionAlloc();
    while (-1 != (nextConnection->socketfd = accept(listenerfd, (struct sockaddr*) &nextConnection->remoteAddr, &nextConnection->remoteAddrLength))) {
        pthread_t connectionThread;
        /* we just received a new connection, spawn a thread */
        pthread_create(&connectionThread, NULL, &connectionHandlerThread, nextConnection);
        nextConnection = connectionAlloc();
    }
    pthread_mutex_destroy(&counters.lock);
    pthread_mutex_destroy(&server.globalMutex);
    printf("exiting because accept failed (probably interrupted) %s = %d\n", strerror(errno), errno);
    return 0;
}

static int sendResponseBody(struct Connection* connection, const struct Response* response, ssize_t* bytesSent) {
    /* First send the response HTTP headers */
    size_t headerLength = snprintf(connection->responseHeader,
                                   sizeof(connection->responseHeader),
                                   "HTTP/1.0 %d %s\r\n"
                                   "Content-Type: %s\r\n"
                                   "Content-Length: %ld\r\n"
                                   "\r\n",
                                   response->code,
                                   response->status,
                                   response->contentType,
                                   (long)response->body.length);
    ssize_t sendResult;
    sendResult = send(connection->socketfd, connection->responseHeader, headerLength, 0);
    if (sendResult != headerLength) {
        printf("Failed to respond to %s:%s because we could not send the HTTP response *header*. send returned %ld with %s = %d\n",
               connection->remoteHost,
               connection->remotePort,
               (long) sendResult,
               strerror(errno),
               errno);
        return -1;
    }
    /* Second, if a response body exists, send that */
    if (response->body.length > 0) {
        sendResult = send(connection->socketfd, response->body.contents, response->body.length, 0);
        if (sendResult != response->body.length) {
            printf("Failed to response to %s:%s because we could not send the HTTP response *body*. send returned %ld with %s = %d\n",
                   connection->remoteHost,
                   connection->remotePort,
                   (long) sendResult,
                   strerror(errno),
                   errno);
            return -1;
        }
        return 0;
    }
    return 0;
}

static int sendResponseFile(struct Connection* connection, const struct Response* response, ssize_t* bytesSent) {
    /* For high performance we really want to use sendfile. However I want the code to work on Windows some day
     and we really don't need that level of performance right now. Also there's no stat on Windows. Also Windows
     almost certainly has some nicer higher-performance equivalent of sendfile. */
    struct Response* errorResponse = NULL;
    FILE* fp = fopen(response->filenameToSend, "rb");
    int result = 0;
    long fileLength;
    ssize_t sendResult;
    size_t headerLength;
    size_t actualMIMEReadSize;
    const char* contentType = NULL;
    const size_t MIMEReadSize = 100;
    if (NULL == fp) {
        printf("Unable to satisfy request for '%s' because we could not open the file '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
        errorResponse = responseAlloc404NotFoundHTML(connection->request.path);
        goto exit;
    }
    assert(sizeof(connection->sendRecvBuffer) >= MIMEReadSize);
    actualMIMEReadSize = fread(connection->sendRecvBuffer, 1, MIMEReadSize, fp);
    if (-1 == actualMIMEReadSize) {
        printf("Unable to satisfy request for '%s' because we could read the first bunch of bytes to determine MIME type '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
        errorResponse = responseAlloc500InternalErrorHTML("fread for MIME type detection failed");
        goto exit;
    }
    contentType = MIMETypeFromFile(response->filenameToSend, (const uint8_t*) connection->sendRecvBuffer, actualMIMEReadSize);
    /* get the file length, laboriously checking for errors */
    result = fseek(fp, 0, SEEK_END);
    if (0 != result) {
        printf("Unable to satisfy request for '%s' because we could not fseek to the end of the file '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
        errorResponse = responseAlloc500InternalErrorHTML("fseek to end of file failed");
        goto exit;
    }
    fileLength = ftell(fp);
    if (fileLength < 0) {
        printf("Unable to satisfy request for '%s' because we could not ftell on the file '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
        errorResponse = responseAlloc500InternalErrorHTML("ftell to determine file length failed");
        goto exit;
    }
    result = fseek(fp, 0, SEEK_SET);
    if (0 != result) {
        printf("Unable to satisfy request for '%s' because we could not fseek to the beginning of the file '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
        errorResponse = responseAlloc500InternalErrorHTML("fseek to beginning of file to start sending failed");
        goto exit;
    }
    
    /* now we have the file length + MIME TYpe and we can send the header */
    headerLength = snprintf(connection->responseHeader,
                            sizeof(connection->responseHeader),
                            "HTTP/1.0 %d %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %ld\r\n"
                            "\r\n",
                            response->code,
                            response->status,
                            contentType,
                            fileLength);
    sendResult = send(connection->socketfd, connection->responseHeader, headerLength, 0);
    if (sendResult != headerLength) {
        printf("Unable to satisfy request for '%s' because we could not send the HTTP header '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
        fclose(fp);
        return 1;
    }
    *bytesSent = sendResult;
    /* read the whole file, just buffering into the connection buffer, and sending it out to the socket */
    while (!feof(fp)) {
        size_t bytesRead = fread(connection->sendRecvBuffer, 1, sizeof(connection->sendRecvBuffer), fp);
        if (0 == bytesRead) { /* peacefull end of file */
            break;
        }
        if (-1 == bytesRead) {
            printf("Unable to satisfy request for '%s' because there was an error freading. '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
            errorResponse = responseAlloc500InternalErrorHTML("Could not fread to send over socket");
            goto exit;
        }
        /* send the data out the socket to the network */
        sendResult = send(connection->socketfd, connection->sendRecvBuffer, bytesRead, 0);
        if (sendResult != bytesRead) {
            printf("Unable to satisfy request for '%s' because there was an error sending bytes. '%s' %s = %d\n", connection->request.path, response->filenameToSend, strerror(errno), errno);
            result = 1;
            goto exit;
        }
        *bytesSent = *bytesSent + sendResult;
    }
exit:
    if (NULL != fp) {
        fclose(fp);
    }
    if (NULL != errorResponse) {
        ssize_t errorBytesSent = 0;
        result = sendResponseBody(connection, errorResponse, &errorBytesSent);
        *bytesSent = *bytesSent + errorBytesSent;
        return result;
    }
    return result;
}

static int sendResponse(struct Connection* connection, const struct Response* response, ssize_t* bytesSent) {
    if (response->body.length > 0) {
        return sendResponseBody(connection, response, bytesSent);
    }
    if (NULL != response->filenameToSend) {
        return sendResponseFile(connection, response, bytesSent);
    }
    printf("Error: the request for '%s' failed because there was neither a response body nor a filenameToSend\n", response->filenameToSend);
    assert(0 && "See above printf");
    return 1;
}

static void* connectionHandlerThread(void* connectionPointer) {
    struct Connection* connection = (struct Connection*) connectionPointer;
    getnameinfo((struct sockaddr*) &connection->remoteAddr, connection->remoteAddrLength,
                connection->remoteHost, sizeof(connection->remoteHost),
                connection->remotePort, sizeof(connection->remotePort), NI_NUMERICHOST | NI_NUMERICSERV);
    printf("New connection from %s:%s...\n", connection->remoteHost, connection->remotePort);
    pthread_mutex_lock(&counters.lock);
    counters.activeConnections++;
    counters.totalConnections++;
    pthread_mutex_unlock(&counters.lock);
    /* first read the request + request body */
    bool madeRequestPrintf = false;
    bool foundRequest = false;
    ssize_t bytesRead;
    while ((bytesRead = recv(connection->socketfd, connection->sendRecvBuffer, SEND_RECV_BUFFER_SIZE, 0)) > 0) {
        if (OptionPrintWholeRequest) {
            fwrite(connection->sendRecvBuffer, 1, bytesRead, stdout);
        }
        pthread_mutex_lock(&counters.lock);
        counters.bytesReceived += bytesRead;
        pthread_mutex_unlock(&counters.lock);
        requestParse(&connection->request, connection->sendRecvBuffer, bytesRead);
        if (connection->request.state >= RequestParseStateVersion && !madeRequestPrintf) {
            printf("Request from %s:%s: %s to %s version %s\n",
                   connection->remoteHost,
                   connection->remotePort,
                   connection->request.method,
                   connection->request.path,
                   connection->request.version);
            madeRequestPrintf = true;
        }
        if (connection->request.state == RequestParseStateDone) {
            foundRequest = true;
            break;
        }
    }
    ssize_t bytesSent = 0;
    if (foundRequest) {
        struct Response* response = createResponseForRequest(&connection->request, connection);
        if (NULL != response) {
            int result = sendResponse(connection, response, &bytesSent);
            if (0 == result) {
                printf("Sent response length %ld to %s:%s\n", bytesSent, connection->remoteHost, connection->remotePort);
            } else {
                /* sendResponse already printed something out, don't add another printf */
            }
            responseFree(response);
        } else {
            printf("You have returned a NULL response - I'm assuming you took over the request handling yourself.\n");
        }
    } else {
        printf("No request found from %s:%s? Bailing...\n", connection->remoteHost, connection->remotePort);
    }
    /* Alright - we're done */
    pthread_mutex_lock(&counters.lock);
    counters.bytesSent += (long) bytesSent;
    counters.activeConnections--;
    pthread_mutex_unlock(&counters.lock);
    printf("Connection from %s:%s closed\n", connection->remoteHost, connection->remotePort);
    close(connection->socketfd);
    connectionFree(connection);
    return NULL;
}

static void printIPv4Addresses(uint16_t portInHostOrder) {
    struct ifaddrs* addrs = NULL;
    getifaddrs(&addrs);
    struct ifaddrs* p = addrs;
    while (NULL != p) {
        if (p->ifa_addr->sa_family == AF_INET) {
            char hostname[256];
            getnameinfo(p->ifa_addr, p->ifa_addr->sa_len, hostname, sizeof(hostname), NULL, 0, NI_NUMERICHOST);
            printf("Probably listening on http://%s:%u\n", hostname, portInHostOrder);
        }
        p = p->ifa_next;
    }
    if (NULL != addrs) {
        freeifaddrs(addrs);
    }
}

static int globalMutexLock() {
    return pthread_mutex_lock(&server.globalMutex);
}

static int globalMutexUnlock() {
    return pthread_mutex_unlock(&server.globalMutex);
}

/* Apache2 has a module called MIME magic or something which does a really good version of this. */
static const char* MIMETypeFromFile(const char* filename, const uint8_t* contents, size_t contentsLength) {
    static const uint8_t PNGMagic[] = {137, 80, 78, 71, 13, 10, 26, 10}; // http://libpng.org/pub/png/spec/1.2/PNG-Structure.html
    static const uint8_t GIFMagic[] = {'G', 'I', 'F'}; // http://www.onicos.com/staff/iz/formats/gif.html
    static const uint8_t JPEGMagic[] = {0xFF, 0xD8}; // ehh pretty shaky http://www.fastgraph.com/help/jpeg_header_format.html
    
    // PNG?
    if (contentsLength >= 8) {
        if (0 == memcmp(PNGMagic, contents, sizeof(PNGMagic))) {
            return "image/png";
        }
    }
    // GIF?
    if (contentsLength >= 3) {
        if (0 == memcmp(GIFMagic, contents, sizeof(GIFMagic))) {
            return "image/gif";
        }
    }
    // JPEG?
    if (contentsLength >= 2) {
        if (0 == memcmp(JPEGMagic, contents, sizeof(JPEGMagic))) {
            return "image/jpeg";
        }
    }
    /* just start guessing based on file extension */
    if (strEndsWith(filename, "html") || strEndsWith(filename, "htm")) {
        return "text/html";
    }
    if (strEndsWith(filename, "css")) {
        return "text/css";
    }
    if (strEndsWith(filename, "gz")) {
        return "application/x-gzip";
    }
    if (strEndsWith(filename, "js")) {
        return "application/javascript";
    }
    /* is it a plain text file? Just inspect the first 100 bytes or so for ASCII */
    bool plaintext = true;
    for (size_t i = 0; i < MIN(contentsLength, 100); i++) {
        if (contents[i] > 127) {
            plaintext = false;
            break;
        }
    }
    if (plaintext) {
        return "text/plain";
    }
    /* well that's pretty much all the different file types in existence */
    return "application/binary";
}

static bool strEndsWith(const char* big, const char* endsWith) {
    size_t bigLength = strlen(big);
    size_t endsWithLength = strlen(endsWith);
    if (bigLength < endsWithLength) {
        return false;
    }
    
    for (size_t i = 0; i < endsWithLength; i++) {
        size_t bigIndex = i + (bigLength - endsWithLength);
        if (big[bigIndex] != endsWith[i]) {
            return false;
        }
    }
    return true;
}

static void testHeapString() {
    struct HeapString easy;
    heapStringSetToCString(&easy, "Part1");
    assert(heapStringIsSaneCString(&easy));
    heapStringAppendString(&easy, " Part2");
    assert(heapStringIsSaneCString(&easy));
    assert(0 == strcmp(easy.contents, "Part1 Part2"));
    const int testNumber = 3;
    heapStringAppendFormat(&easy, " And this is Part%d", testNumber);
    assert(heapStringIsSaneCString(&easy));
    assert(0 == strcmp(easy.contents, "Part1 Part2 And this is Part3"));
    heapStringAppendChar(&easy, ' ');
    heapStringAppendChar(&easy, 'P');
    heapStringAppendChar(&easy, 'a');
    heapStringAppendChar(&easy, 'r');
    heapStringAppendChar(&easy, 't');
    heapStringAppendChar(&easy, '4');
    assert(heapStringIsSaneCString(&easy));
    assert(0 == strcmp(easy.contents, "Part1 Part2 And this is Part3 Part4"));
    printf("The test heap string is '%s' with an allocated capacity of %ld\n", easy.contents, (long) easy.capacity);
    heapStringFreeContents(&easy);
    struct HeapString testFormat;
    heapStringInit(&testFormat);
    heapStringAppendFormat(&testFormat, "Testing format %d", 1);
    assert(heapStringIsSaneCString(&testFormat));
    assert(0 == strcmp("Testing format 1", testFormat.contents));
    heapStringFreeContents(&testFormat);
    struct HeapString testAppend;
    heapStringInit(&testAppend);
    heapStringAppendChar(&testAppend, 'X');
    assert(heapStringIsSaneCString(&testAppend));
    assert(0 == strcmp("X", testAppend.contents));
    heapStringFreeContents(&testAppend);
    struct HeapString testSet;
    heapStringSetToCString(&testSet, "This is a C string");
    assert(heapStringIsSaneCString(&testSet));
    assert(0 == strcmp("This is a C string", testSet.contents));
}

static int strcmpAndFreeFirstArg(char* firstArg, const char* secondArg) {
    int result = strcmp(firstArg, secondArg);
    free(firstArg);
    return result;
}

static void teststrdupHTMLEscape() {
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML(" "), "&nbsp;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("t "), "t&nbsp;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML(" t"), "&nbsp;t"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("\n"), "\n"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML(""), ""));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("nothing"), "nothing"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("   "), "&nbsp;&nbsp;&nbsp;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("<"), "&lt;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML(">"), "&gt;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("< "), "&lt;&nbsp;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("> "), "&gt;&nbsp;"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML("<a"), "&lt;a"));
    assert(0 == strcmpAndFreeFirstArg(strdupEscapeForHTML(">a"), "&gt;a"));
}

static void teststrdupEscape() {
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=value", NULL), "value"));
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=+value+", NULL), " value "));
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=%20value%20", NULL), " value "));
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=%200value%200", NULL), " 0value 0"));
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=%0a0value%0a0", NULL), "\n0value\n0"));
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=val%20ue", NULL), "val ue"));
    assert(0 == strcmpAndFreeFirstArg( strdupDecodeGETorPOSTParam("param=", "param=value%0a&next", NULL), "value\n"));
}

static void testsRun() {
    testHeapString();
    teststrdupHTMLEscape();
    teststrdupEscape();
    memset(&counters, 0, sizeof(counters)); // reset counters from tests
}
