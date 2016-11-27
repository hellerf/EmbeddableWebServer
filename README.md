# EWS - Single .h File C Embeddable Web Server #
Latest Version: 1.0 released November 23, 2016<br>
Supported platforms: Linux, Mac OS X, Windows<br>
License: BSD 2-clause<br>

Embeddable Web Server is a web server in a single header file and has no external dependencies in the tradition of the [STB](https://github.com/nothings/stb) libraries. It can serve static files and handle responses with C/C++ code (the server compiles in both). Here's how to use it:<br>
1. `#include "EmbeddableWebServer.h"` (and `#define EWS_HEADER_ONLY` if you included it somewhere else)<br>
2. Call `acceptConnectionsUntilStoppedFromEverywhereIPv4(NULL)`, which will initialize a new server and block
Note: If you want to take connections from a specific inteface/localhost you can use `acceptConnectionsUntilStopped`<br>
3. Fill out `createResponseForRequest`. Use the `responseAlloc*` functions to return a response or take over the connection
yourself and return NULL. The easiest way to serve static files is responseAllocServeFileFromRequestPath. The easiest 
way to serve HTML is responseAllocHTML. The easiest way to serve JSON is responseAllocJSON. The server will free() your
response once it's been sent.
<br>See the <b>EWSDemo.cpp</b> file for more examples like chunked transfer, HTML forms, and JSON responses. 

If you want to control server setup/teardown use `serverInit`, `serverStop`, and `serverDeInit` and pass that same `Server` in `acceptConnectionsUntilStopped`.

## Quick Example ##
	#include "EmbeddableWebServer.h"
	#pragma comment(lib, "ws2_32")

	int main(int argc, char* argv[]) {
		return acceptConnectionsUntilStoppedFromEverywhereIPv4(NULL, 8080);
	}

	struct Response* createResponseForRequest(const struct Request* request, struct Connection* connection) {
		if (0 == strcmp(request->pathDecoded, "/welcome")) {
			return responseAllocHTML("<html><body><marquee><h1>Welcome to my home page</h1></marquee></body></html>");
		}
		if (0 == strcmp(request->pathDecoded, "/status/json")) {
			static const char* statuses[] = { ":-)", ":-(", ":-|" };
			int status = rand() % (sizeof(statuses) / sizeof(*statuses));
			return responseAllocWithFormat(200, "OK", "application/json", "{ \"status\" : \"%s\" }", statuses[status]);
		}
		if (0 == strcmp(request->pathDecoded, "/100_random_numbers")) {
			struct Response* response = responseAllocHTML("<html><body><h1>100 Random Numbers</h1><ol>");
			for (int i = 1; i <= 100; i++) {
				heapStringAppendFormat(&response->body, "<li>%d</li>\n", rand());
			}
			heapStringAppendString(&response->body, "</ol></body></html>");
			return response;
		}
		/* Serve files from the current directory */
		if (request->pathDecoded == strstr(request->pathDecoded, "/files")) {
			return responseAllocServeFileFromRequestPath("/files", request->path, request->pathDecoded, ".");
		}
		return responseAlloc404NotFoundHTML("What?!");
	}

## Features and use cases ##
 * Serve a debug page/dashboard for your application 
 * Expose variables for debugging your 3D graphics application
 * Handle HTML GET + POST form data
 * Serve up websites for embedded touch display panels
 * Mix dynamic request handlers with static content
 * Seamless emoji support: Handles UTF-8 and international files, even on Windows (run the demo)

## Warning ##
This server is suitable for controlled applications which will not be accessed over the general Internet. If you are determined to use this on Internet I advise you to use a proxy server in front (like haproxy, squid, or nginx). However I found and fixed only 2 crashes with alf-fuzz...

## Implementation ##
The server is implemented in a thread-per-connection model. This way you can do slow, hacky things in a request and not stall other requests. On the other hand you will use ~40KB + response body + request body of memory per connection. All strings are assumed to be UTF-8. On Windows, UTF-8 file paths are converted to their wide-character (wchar_t) equivalent so you can serve files with Chinese characters and so on.

The server assumes all strings are UTF-8. When accessing the file system on Windows, EWS will convert to/from the wchar_t representation and use the appropriate APIs.

## pthreads wrapper for Windows ##
Since EWS uses threads we need to have a way to launch threads on all platforms. pthreads are supported on most of the operating systems this targets. Hence, EWS targets pthreads directly. EWS includes a very light wrapper for pthreads that supports thread creation, mutexes, and condition variables.

## Example of launching a server thread from your app ##
    #include "EmbeddableWebServer.h"
    #ifdef WIN32
    #pragma comment(lib, "ws2_32") // link against Winsock on Windows
    #endif

    static int counter = 0;
    static struct Server server;
    static THREAD_RETURN_TYPE STDCALL_ON_WIN32 acceptConnectionsThread(void* unusedParam) {
        serverInit(&server);
        const uint16_t portInHostOrder = 8080;
        acceptConnectionsUntilStoppedFromEverywhereIPv4(&server, portInHostOrder);
        return (THREAD_RETURN_TYPE) 0;
    }

    int main() {
        pthread_t threadHandle;
        pthread_create(&threadHandle, NULL, &acceptConnectionsThread, NULL);
        while (1) {
            counter++;
        }
        // rest of the program
        return 0;
    }
    
    struct Response* createResponseForRequest(const struct Request* request, struct Connection* connection) {
        return responseAllocHTMLWithFormat("The counter is currently %d\n", counter);
    }

## Comparison to other really light embeddable web servers ##
* [yocto HTTP server](https://github.com/tom-seddon/yhs) - yocto has more features (WebSockets, handling deferred requests, custom headers, and can build PNG images on the fly - pretty cool) and lets you spit out the response in pieces. If you want anything custom EWS makes you take over the whole request yourself. yocto also has better MIME type detection. EWS is smaller and handles each connection on a separate thread so one slow response doesn't block the others.
* [Mongoose](https://github.com/cesanta/mongoose) - mongoose is professionally supported and developed, dual-licensed under GPL and a commercial license you pay for. Mongoose has a huge amount of features. It works with or without an operating system.
* [Baraccuda](https://realtimelogic.com/products/barracuda-application-server/) - Baraccuda from Real-Time logic is a proprietary web server targetting embedded systems. I think they run with and without an OS and include lots of features like Mongoose does.
