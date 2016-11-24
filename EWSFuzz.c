#define EWS_FUZZ_TEST 1
#include "../EmbeddableWebServer.h"

struct Server server;
int main(int argc, const char* argv[]) {
    printf("%d\n", RequestParseStateBadRequest);
    serverInit(&server);
    struct Connection* connection = connectionAlloc(&server);
    struct sockaddr_in* simulated = (struct sockaddr_in*) &connection->remoteAddr;
    simulated->sin_addr.s_addr = INADDR_LOOPBACK;
    simulated->sin_family = AF_INET;
    simulated->sin_port = htons(8080);
    connection->socketfd = STDIN_FILENO;
    connectionHandlerThread(connection);
    return 0;
}
struct Response* createResponseForRequest(const struct Request* request, struct Connection* connection) {
    //char* strdupDecodePOSTParam(const char* paramNameIncludingEquals, const struct Request* request, const char* valueIfNotFound)
    char* message = strdupDecodePOSTParam("message=", request, "");
    printf("%s\n", message);
    free(message);
    return responseAllocServeFileFromRequestPath("/", request->path, request->pathDecoded, "fuzz-test-document-root");
}

