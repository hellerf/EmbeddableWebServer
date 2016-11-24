# EWS - Embeddable Web Server #
Latest Version: 1.0 released November 23, 2016<br>
Supported platforms: Linux, Mac OS X, Windows<br>
License: BSD 2-clause<br>

Embeddable Web Server is a small web server. It contains no dependencies and resides in a single file like the popular [STB](https://github.com/nothings/stb) libraries. It can serve static files and handle responses with C/C++ code. The server will compile in both C and C++. Here's how to use it:
1. `#include "EmbeddableWebServer.h"` (and `#define EWS_HEADER_ONLY` if you included it somewhere else)
2. Call `acceptConnectionsUntilStoppedFromEverywhereIPv4(NULL)`, which will initialize a new server and block
Note: If you want to take connections from a specific inteface/localhost you can use acceptConnectionsUntilStopped
3. Fill out `createResponseForRequest`. Use the `responseAlloc*` functions to return a response or take over the connection
yourself and return NULL. The easiest way to serve static files is responseAllocServeFileFromRequestPath. The easiest 
way to serve HTML is responseAllocHTML. The easiest way to serve JSON is responseAllocJSON. The server will free() your
response once it's been sent. See the EWSDemo.c file for more examples. 

If you want to control server setup/teardown use `serverInit`, `serverStop`, and `serverDeInit`.

## Quick Example ##
	#include "EmbeddableWebServer.h"

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
			struct Response* response = responseAllocHTML("<html><body><h1>100 Random Numbers</ul><ol>");
			for (int i = 1; i <= 100; i++) {
				heapStringAppendFormat(&response->body, "<li>%d</li>\n", rand());
			}
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
 * Handle HTTP GET + POST forms
 * Serve up websites for embedded touch display panels
 * Mix dynamic request handlers with static content
 * Seamless emoji support: Handles UTF-8 and international files, even on Windows (run the demo)
 * Quickly print out the whole request contents for debugging an HTTP API client

## Warning ##
This server is suitable for controlled applications which will not be accessed over the general Internet. If you are determined to put this in front of the Internet I advise you to use a proxy server in front (like haproxy, squid, or nginx). However I did spend half a day fuzzing it with afl-fuzz and fixed a couple crashers...

## Implementation ##
The server is implemented in a thread-per-connection model. This way you can do slow, hacky things in a request and not stall other requests. On the other hand you will use ~40KB + response body + request body of memory per connection. All strings are assumed to be UTF-8. On Windows, UTF-8 file paths are converted to their wide-character (wchar_t) equivalent so you can serve files with Chinese characters and so on.


## Comparison to other embeddable web servers ##
* [yocto HTTP server](https://github.com/tom-seddon/yhs) - yocto has more features (WebSockets, handling deferred requests, custom headers, and can build PNG images on the fly - pretty cool) and lets you spit out the response in pieces. If you want anything custom EWS makes you take over the whole request yourself. yocto also has better MIME type detection. EWS is smaller and handles each connection on a separate thread so one slow response doesn't block the others.
* [Mongoose](https://github.com/cesanta/mongoose) - mongoose is professionally supported and developed, dual-licensed under GPL and a commercial license you pay for. Mongoose has a huge amount of features. It works with or without an operating system.
