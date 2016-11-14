// Released into the Public Domain by Forrest Heller 2016
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

/* This is a very simple copy & pastable web server that I tested on Mac OS X and Linux. It doesn't serve files or do CGI and you have to write your own HTTP handlers. It's almost certainly insecure. It shines when:
-You need to embed a web server into an application to surface statistics/counters/status
-You need to run a web server in a bare-bones environment without Python/Apache/etc.
-You want to see the contents of every request printed out
-You don't need sophistication
 
To use:
 1.
 1. Delete the main() from this file and call acceptConnectionsForeverFromEverywhereIPv4
*/
 
/* History:
 Version 1.0 - released */

// To embed just call this function
static int acceptConnectionsForeverFromEverywhereIPv4(uint16_t portInHostOrder);

/* quick options */
static bool OptionPrintWholeRequest = false;

/* Example usage: */
int main(int argc, const char * argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        int portScanned = port;
        sscanf(argv[1], "%d", &portScanned);
        port = (uint16_t) portScanned;
    }
    acceptConnectionsForeverFromEverywhereIPv4(port);
    return 0;
}

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

struct HeaderString {
    char* contents; // null-terminated
    size_t length;
    size_t capacity;
};

struct Header {
    struct HeaderString name;
    struct HeaderString value;
    struct Header* next;
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
    struct HeaderString body;
    /* linked list containing HTTP request headers */
    struct Header* firstHeader;
    struct Header* currentHeader;
    struct Header* lastHeader;
    /* internal state for the request parser */
    RequestParseState state;
};

#define RECV_BUFFER_SIZE 4096
#define HEADER_BUFFER_SIZE 4096

/* This contains a full HTTP connection. For every connection, a thread is spawned
 and passed this struct */
struct Connection {
    int socketfd;
    /* Who connected? */
    struct sockaddr_storage remoteAddr;
    socklen_t remoteAddrLength;
    char remoteHost[256];
    char remotePort[20];
    /* Just allocate the buffers in the connetcion */
    char recvBuffer[RECV_BUFFER_SIZE];
    char headerBuffer[HEADER_BUFFER_SIZE];
    struct Request request;
};

static struct Counters {
    pthread_mutex_t lock;
    long bytesReceived;
    long bytesSent;
    long totalConnections;
    long activeConnections;
} counters;

struct Response {
    int code;
    char* contents;
    size_t contentsLength;
    char* status;
    char* contentType;
};

#ifndef __printflike
#define __printflike(...)
#endif

/* use these in createResponseToRequest */
static struct Response* responseAlloc(int code, const char* status, const char* contentType, size_t contentsLength);
static struct Response* responseAllocHTML(const char* html);
static struct Response* responseAllocHTMLWithStatus(const char* html, int code, const char* status);
static struct Response* responseAllocWithFormat(int code, const char* status, const char* contentType, const char* format, ...) __printflike(3, 0);
/* You can pass this the request->path for GET or request->body.contents for POST. Accepts NULL for paramString for convenience */
static char* strdupDecodeGETorPOSTParam(const char* paramNameIncludingEquals, const char* paramString, const char* valueIfNotFound);
static struct Header* headerInRequest(const char* headerName, const struct Request* request);

struct String {
    char* contents;
    size_t length;
    size_t capacity;
};
// This calls asprintf (malloc) every call
static void stringAppendFormat(struct String* string, const char* format, ...) {
    char* append = NULL;
    va_list ap;
    va_start(ap, format);
    size_t appendLength = vasprintf(&append, format, ap);
    va_end(ap);
    while (string->length + appendLength + 1 > string->capacity) {
        string->capacity *= 2;
    }
    string->contents = (char*) realloc(string->contents, string->capacity);
    strncat(string->contents, append, appendLength);
    string->length += appendLength;
    string->contents[string->length] = '\0';
    free(append);
}

static void stringFree(struct String* string) {
    if (NULL != string->contents) {
        free(string->contents);
    }
}

static struct String connectionDebugStringCreate(const struct Connection* connection) {
    struct String debugString = {0};
    stringAppendFormat(&debugString, "%s from %s:%s\n", connection->request.method, connection->remoteHost, connection->remotePort);
    struct Header* header = connection->request.firstHeader;
    bool firstHeader = true;
    while (NULL != header) {
        if (firstHeader) {
            stringAppendFormat(&debugString, "\n*** Request Headers ***\n");
            firstHeader = false;
        }
        stringAppendFormat(&debugString, "'%s' = '%s'\n", header->name.contents, header->value.contents);
        header = header->next;
    }
    if (NULL != connection->request.body.contents) {
        stringAppendFormat(&debugString, "\n*** Request Body ***\n%s\n", connection->request.body.contents);
    }
    return debugString;
}
/* Modify this */
static struct Response* createResponseToRequest(const struct Request* request, const struct Connection* connection) {
    /* Here's an example of how to return a regular web page */
    if (request->path == strstr(request->path, "/status")) {
        return responseAllocWithFormat(200, "OK", "text/html; charset=UTF-8", "<html><title>Server Stats Page Example</title>"
                                       "Here are some basic measurements and status indicators for this server<br>"
                                       "<table border=\"1\">\n"
                                       "<tr><td>Active connections</td><td>%ld</td></tr>\n"
                                       "<tr><td>Total connections</td><td>%ld (Remember that most browsers try to get a /favicon)</td></tr>\n"
                                       "<tr><td>Total bytes sent</td><td>%ld</td></tr>\n"
                                       "<tr><td>Total bytes received</td><td>%ld</td></tr>\n"
                                       "</table></html>",
                                       counters.activeConnections,
                                       counters.totalConnections,
                                       counters.bytesSent,
                                       counters.bytesReceived);
    }
    if (request->path == strstr(request->path, "/")) {
        /* Here's a really simple example of using an HTML GET param (without URL decoding) */
        char* name = strdupDecodeGETorPOSTParam("name=", request->path, "(empty - not filled in)");
        char* birthday = strdupDecodeGETorPOSTParam("birthday=", request->path, "(empty - not filled in)");
        /* Here's how to get POST data */
const char* favoriteDrink = strdupDecodeGETorPOSTParam("favorite_drink=", request->body.contents, "(empty - not filled in)");
        
        /* Get the request body directly */
        const char* requestBody = request->body.contents;
        if (NULL == requestBody) {
            requestBody = "(no body found - make sure you're client is sending the Content-Length header)";
        }
        
        /* Assume headers are less than 16kb and put them all into a string */
        char* headerStrings = (char*) malloc(16*1024);
        memset(headerStrings, 0, 16*1024);
        size_t headerBufferOffset = 0;
        struct Header* header = request->firstHeader;
        while (NULL != header) {
            headerBufferOffset += sprintf(&headerStrings[headerBufferOffset], "<code>'%s' = '%s'</code><br>", header->name.contents, header->value.contents);
            header = header->next;
        }
        
        struct Response* response = responseAllocWithFormat(200, "OK", "text/html", "<html><title>Home - Embeddable C Web Server</title><body><h4>Request Info</h4>Request from %s:%s:<br> %s to %s version %s"
                                       "<h4>Your name and birthday GET demo</h4>"
                                       "Hello:<strong>%s</strong> born <strong>%s</strong><br>"
                                       "<form action=\"/index\" method=\"GET\">What is your name? <input type=\"text\" name=\"name\"><br>"
                                       "What is your birthday? <input type=\"text\" name=\"birthday\"><br>"
                                       "<input type=\"submit\" value=\"Show me the GET\">\n"
                                       "</form>"
                                       "<h4>Your favorite drink POST demo</h4>"
                                       "<form action=\"/index\" method=\"POST\">\n"
                                       "Your favorite drink is <strong>%s</strong>\n<br>"
                                       "What is your favorite drink?"
                                       "<input type=\"text\" name=\"favorite_drink\">"
                                       "<input type=\"submit\" value=\"Show me the POST\">\n"
                                       "</form>\n"
                                       "<h4>Misc</h4>\n"
                                       "<a href=\"/status\">Server Stats Page</a><br>\n"
                                       "<a href=\"/about\">About</a>\n"
                                       "<h4>Request Headers</h4>"
                                       "%s\n"
                                       "<h4>Request Body</h4>"
                                       "%s\n"
                                       "</body></html>",
                                       connection->remoteHost,
                                       connection->remotePort,
                                       connection->request.method,
                                       connection->request.path,
                                       connection->request.version,
                                       name,
                                       birthday,
                                       favoriteDrink,
                                       headerStrings,
                                       requestBody);
        return response;
    }
    if (request->path == strstr(request->path, "/about")) {
        return responseAllocHTML("<html><head><title>About</title><body>EmbeddedWebServer version 1.0 by Forrest Heller</body></html>");
    }
    /* !!! Wait! Don't delete this one!  !!!*/
    return responseAllocHTMLWithStatus("<html><head><title>Not found</title>"
                                       "<body><h1>404 - Not found</h1>"
                                       "The URL you requested could not be found", 404, "Not Found");
}


/* Internal stuff */
static void responseFree(struct Response* response);
static void printIPv4Addresses(uint16_t portInHostOrder);
static struct Connection* connectionAlloc();
static void connectionFree(struct Connection* connection);
static void* connectionHandlerThread(void* connectionPointer);
static void requestParse(struct Request* request, const char* requestFragment, size_t requestFragmentLength);
struct Header* headerAlloc();
static void headerStringAppend(struct HeaderString* string, char c);

static struct Header* headerInRequest(const char* headerName, const struct Request* request) {
    struct Header* header = request->firstHeader;
    while (NULL != header) {
        if (0 == strcasecmp(header->name.contents, headerName)) {
            return header;
        }
        header = header->next;
    }
    return NULL;
}

static char* strdupIfNotNull(const char* strToDup) {
    if (NULL == strToDup) {
        return NULL;
    }
    return strdup(strToDup);
}

static char* strdupDecodeGETorPOSTParam(const char* paramNameIncludingEquals, const char* paramString, const char* valueIfNotFound) {
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
    size_t maximumPossibleLength = strlen(paramString);
    char* decoded = (char*) malloc(maximumPossibleLength + sizeof('\0'));
    size_t deci = 0;
    const char* param = paramStart;
    /* these three vars unescape % escaped things */
    char state = 0; // 0 = not escaping, 1 = first hex digit, 2 = second hex digit
    char firstDigit = 0;
    char secondDigit = 0;
    while (NULL != param && '&' != *param && '\0' != *param) {
        switch (state) {
            case 0:
                if ('%' == *param) {
                    state = 1; // first digit
                } else if ('+' == *param) {
                    decoded[deci] = ' ';
                    deci++;
                } else {
                    decoded[deci] = *param;
                    deci++;
                }
                break;
            case 1:
                // copy the first digit, get the second digit
                firstDigit = *param;
                state = 2;
                break;
            case 2:
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
                state = 0;
            }
                break;
        }
        param++;
    }
    decoded[deci] = '\0';
    return decoded;
}

static void headerStringAppend(struct HeaderString* string, char c) {
    if (NULL == string->contents) {
        string->capacity = 32;
        string->length = 0;
        string->contents = (char*) malloc(string->capacity);
    }
    
    if (string->length >= string->capacity - 2) {
        string->capacity *= 2;
        string->contents = (char*) realloc(string->contents, string->capacity);
    }
    
    string->contents[string->length] = c;
    string->length++;
    string->contents[string->length] = '\0';
}

struct Header* headerAlloc() {
    struct Header* header = (struct Header*) calloc(1, sizeof(*header));
    return header;
}

static void headerFree(struct Header* header) {
    if (NULL != header->name.contents) {
        free(header->name.contents);
    }
    if (NULL != header->value.contents) {
        free(header->value.contents);
    }
}

// allocates a response with content = malloc(contentLength + 1) so you can write null-terminated strings to it
static struct Response* responseAlloc(int code, const char* status, const char* contentType, size_t contentsLength) {
    struct Response* response = (struct Response*) malloc(sizeof(*response));
    response->code = code;
    response->contentsLength = contentsLength;
    response->contents = (char*) malloc(response->contentsLength + 1);
    response->contentType = strdup(contentType);
    response->status = strdup(status);
    return response;
}

static struct Response* responseAllocHTML(const char* html) {
    struct Response* response = responseAlloc(200, "OK", "text/html; charset=UTF-8", strlen(html));
    // remember that responseAlloc allocates an extra byte
    strcpy(response->contents, html);
    return response;
}

static struct Response* responseAllocHTMLWithStatus(const char* html, int code, const char* status) {
    struct Response* response = responseAlloc(code, status, "text/html; charset=UTF-8", strlen(html));
    // remember that responseAlloc allocates an extra byte
    strcpy(response->contents, html);
    return response;
}

static struct Response* responseAllocWithFormat(int code, const char* status, const char* contentType, const char* format, ...) {
    struct Response* response = responseAlloc(code, status, contentType, 16);
    // it's less code to just free the response and call vasprintf
    free(response->contents);
    va_list ap;
    va_start(ap, format);
    vasprintf(&response->contents, format, ap);
    va_end(ap);
    response->contentsLength = strlen(response->contents);
    return response;
}

static void responseFree(struct Response* response) {
    free(response->status);
    free(response->contents);
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
                    if (NULL == request->firstHeader) {
                        request->firstHeader = headerAlloc();
                        request->lastHeader = request->firstHeader;
                        request->currentHeader = request->firstHeader;
                    }
                    if (NULL == request->currentHeader) {
                        request->currentHeader = headerAlloc();
                    }
                    headerStringAppend(&request->currentHeader->name, c);
                }
            break;
            case RequestParseStateHeaderValue:
                /* skip the first space */
                if (c == ' ' && request->currentHeader->value.length == 0) {
                    continue;
                } else if (c == '\r') {
                    request->lastHeader->next = request->currentHeader;
                    request->lastHeader =request->currentHeader;
                    request->currentHeader = NULL;
                    request->state = RequestParseStateCR;
                } else {
                    headerStringAppend(&request->currentHeader->value, c);
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
                    struct Header* contentLengthHeader = headerInRequest("Content-Length", request);
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

static void* connectionHandlerThread(void* connectionPointer) {
    struct Connection* connection = (struct Connection*) connectionPointer;
    getnameinfo((struct sockaddr*) &connection->remoteAddr, connection->remoteAddrLength,
                connection->remoteHost, sizeof(connection->remoteHost),
                connection->remotePort, sizeof(connection->remotePort), NI_NUMERICHOST | NI_NUMERICSERV);
    printf("New connection from %s:%s...\n", connection->remoteHost, connection->remotePort);
    ssize_t bytesRead;
    pthread_mutex_lock(&counters.lock);
    counters.activeConnections++;
    counters.totalConnections++;
    pthread_mutex_unlock(&counters.lock);
    while ((bytesRead = recv(connection->socketfd, connection->recvBuffer, RECV_BUFFER_SIZE, 0)) > 0) {
        if (OptionPrintWholeRequest) {
            fwrite(connection->recvBuffer, 1, bytesRead, stdout);
        }
        pthread_mutex_lock(&counters.lock);
        counters.bytesReceived += bytesRead;
        pthread_mutex_unlock(&counters.lock);
        requestParse(&connection->request, connection->recvBuffer, bytesRead);
        if (connection->request.state == RequestParseStateDone) {
            /* Do you response handling here. Right now we're handling /kittens */
            printf("Request from %s:%s: %s to %s version %s\n",
                   connection->remoteHost,
                   connection->remotePort,
                   connection->request.method,
                   connection->request.path,
                   connection->request.version);
            struct Response* response = createResponseToRequest(&connection->request, connection);
            assert(NULL != response && "You must return a response in createResponseToRequest - I recommend a 404");
            /* First send the response header */
            size_t headerLength = sprintf(connection->headerBuffer,
                    "HTTP/1.0 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %ld\r\n"
                    "\r\n",
                    response->code,
                    response->status,
                    response->contentType,
                    (long)response->contentsLength);
            ssize_t sendResult;
            sendResult = send(connection->socketfd, connection->headerBuffer, headerLength, 0);
            if (sendResult != headerLength) {
                printf("Failed to respond to %s:%s because send returned %ld with %s = %d\n",
                       connection->remoteHost,
                       connection->remotePort,
                       (long) sendResult,
                       strerror(errno),
                       errno);
                /* Keep going - just cleanup the response later */
            }
            /* Ok now send the actual response body (if there's anything there?) */
            if (response->contentsLength > 0) {
                sendResult =send(connection->socketfd, response->contents, response->contentsLength, 0);
                if (sendResult != response->contentsLength) {
                printf("Failed to respond to %s:%s because send returned %ld with %s = %d\n",
                       connection->remoteHost,
                       connection->remotePort,
                       (long) sendResult,
                       strerror(errno),
                       errno);
                }
            }
            printf("Sent response length %ld to %s:%s\n", response->contentsLength + headerLength, connection->remoteHost, connection->remotePort);
            pthread_mutex_lock(&counters.lock);
            counters.bytesSent += (long) response->contentsLength + headerLength;
            pthread_mutex_unlock(&counters.lock);
            /* forget long polling or anything */
            close(connection->socketfd);
            responseFree(response);
            break;
        }
    }
    pthread_mutex_lock(&counters.lock);
    counters.activeConnections--;
    pthread_mutex_unlock(&counters.lock);
    printf("Connection from %s:%s closed\n", connection->remoteHost, connection->remotePort);
    connectionFree(connection);
    return NULL;
}

static struct Connection* connectionAlloc() {
    struct Connection* connection = (struct Connection*) calloc(1, sizeof(*connection)); // calloc 0's everything which requestParse depends on
    connection->remoteAddrLength = sizeof(connection->remoteAddr);
    return connection;
}

static void connectionFree(struct Connection* connection) {
    struct Header* header = connection->request.firstHeader;
    while (NULL != header) {
        struct Header* headerToFree = header;
        header = header->next;
        headerFree(headerToFree);
    }
    if (NULL != connection->request.body.contents) {
        free(connection->request.body.contents);
    }
    free(connection);
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

static int acceptConnectionsForeverFromEverywhereIPv4(uint16_t portInHostOrder) {
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
    
    /* In order to keep the code really short I've just assumed
     we want to bind to 0.0.0.0, which is all available interfaces.
     What you actually want to do is call getaddrinfo on command line arguments
     to let users specify the interface and port */
    struct sockaddr_in anyInterfaceIPv4 = {0};
    anyInterfaceIPv4.sin_addr.s_addr = INADDR_ANY; // also popular inet_addr("127.0.0.1") which is INADDR_LOOPBACK
    anyInterfaceIPv4.sin_family = AF_INET;
    anyInterfaceIPv4.sin_port = htons(portInHostOrder);
    result = bind(listenerfd, (struct sockaddr*) &anyInterfaceIPv4, sizeof(anyInterfaceIPv4));
    if (0 != result) {
        printf("Could not bind to 0.0.0.0:%u: %s = %d\n", portInHostOrder, strerror(errno), errno);
        return 1;
    }
    /* listen for the maximum possible amount of connections */
    result = listen(listenerfd, SOMAXCONN);
    if (0 != result) {
        printf("Could not listen for SOMAXCONN (%d) connections. %s = %d. Continuing because we might still succeed...\n", SOMAXCONN, strerror(errno), errno);
    }
    printIPv4Addresses(portInHostOrder);
    printf("Listening for connections from anywhere on port %u...\n", portInHostOrder);
    /* allocate a connection (which sets connection->remoteAddrLength) and accept the next inbound connection */
    struct Connection* nextConnection = connectionAlloc();
    while (-1 != (nextConnection->socketfd = accept(listenerfd, (struct sockaddr*) &nextConnection->remoteAddr, &nextConnection->remoteAddrLength))) {
        pthread_t connectionThread;
        /* we just received a new connection, spawn a thread */
        pthread_create(&connectionThread, NULL, &connectionHandlerThread, nextConnection);
        nextConnection = connectionAlloc();
    }
    printf("exiting because accept failed (probably interrupted) %s = %d\n", strerror(errno), errno);
    return 0;
}
