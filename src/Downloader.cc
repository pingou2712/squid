/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "client_side.h"
#include "client_side_request.h"
#include "client_side_reply.h"
#include "ClientRequestContext.h"
#include "Downloader.h"
#include "http/one/RequestParser.h"
#include "http/Stream.h"

CBDATA_CLASS_INIT(Downloader);

/// Used to hold and pass the required info and buffers to the
/// clientStream callbacks
class DownloaderContext: public RefCountable
{
    MEMPROXY_CLASS(DownloaderContext);

public:
    typedef RefCount<DownloaderContext> Pointer;

    DownloaderContext(Downloader *dl, ClientHttpRequest *h);
    ~DownloaderContext();
    void finished();

    CbcPointer<Downloader> downloader;
    ClientHttpRequest *http;
    char requestBuffer[HTTP_REQBUF_SZ];
};

DownloaderContext::DownloaderContext(Downloader *dl, ClientHttpRequest *h):
    downloader(dl),
    http(h)
{
    debugs(33, 6, "DownloaderContext constructed, this=" << (void*)this);
}

DownloaderContext::~DownloaderContext()
{
    debugs(33, 6, "DownloaderContext destructed, this=" << (void*)this);
    if (http)
        finished();
}

void
DownloaderContext::finished()
{
    delete http;
    http = nullptr;
}

void
Downloader::CbDialer::print(std::ostream &os) const
{
    os << " Http Status:" << status << Raw("body data", object.rawContent(), 64).hex();
}

Downloader::Downloader(SBuf &url, AsyncCall::Pointer &aCallback, unsigned int level):
    AsyncJob("Downloader"),
    url_(url),
    callback_(aCallback),
    level_(level)
{
}

Downloader::~Downloader()
{
}

bool
Downloader::doneAll() const
{
    return (!callback_ || callback_->canceled()) && AsyncJob::doneAll();
}

static void
downloaderRecipient(clientStreamNode * node, ClientHttpRequest * http,
                    HttpReply * rep, StoreIOBuffer receivedData)
{
    debugs(33, 6, MYNAME);
     /* Test preconditions */
    assert(node);

    /* TODO: handle this rather than asserting
     * - it should only ever happen if we cause an abort and
     * the callback chain loops back to here, so we can simply return.
     * However, that itself shouldn't happen, so it stays as an assert for now.
     */
    assert(cbdataReferenceValid(node));
    assert(!node->node.next);
    DownloaderContext::Pointer context = dynamic_cast<DownloaderContext *>(node->data.getRaw());
    assert(context);

    if (context->downloader.valid())
        context->downloader->handleReply(node, http, rep, receivedData);
}

static void
downloaderDetach(clientStreamNode * node, ClientHttpRequest * http)
{
    debugs(33, 5, MYNAME);
    clientStreamDetach(node, http);
}

/// Initializes and starts the HTTP GET request to the remote server
bool
Downloader::buildRequest()
{ 
    const HttpRequestMethod method = Http::METHOD_GET;

    char *uri = xstrdup(url_.c_str());
    HttpRequest *const request = HttpRequest::CreateFromUrl(uri, method);
    if (!request) {
        debugs(33, 5, "Invalid URI: " << url_);
        xfree(uri);
        return false; //earlyError(...)
    }
    request->http_ver = Http::ProtocolVersion();
    request->header.putStr(Http::HdrType::HOST, request->url.host());
    request->header.putTime(Http::HdrType::DATE, squid_curtime);
    request->flags.internalClient = true;
    request->client_addr.setNoAddr();
#if FOLLOW_X_FORWARDED_FOR
    request->indirect_client_addr.setNoAddr();
#endif /* FOLLOW_X_FORWARDED_FOR */
    request->my_addr.setNoAddr();   /* undefined for internal requests */
    request->my_addr.port(0);
    request->downloader = this;

    debugs(11, 2, "HTTP Client Downloader " << this << "/" << id);
    debugs(11, 2, "HTTP Client REQUEST:\n---------\n" <<
           request->method << " " << url_ << " " << request->http_ver << "\n" <<
           "\n----------");

    ClientHttpRequest *const http = new ClientHttpRequest(nullptr);
    http->request = request;
    HTTPMSGLOCK(http->request);
    http->req_sz = 0;
    http->uri = uri;
    setLogUri (http, urlCanonicalClean(request));

    context_ = new DownloaderContext(this, http);
    StoreIOBuffer tempBuffer;
    tempBuffer.data = context_->requestBuffer;
    tempBuffer.length = HTTP_REQBUF_SZ;

    ClientStreamData newServer = new clientReplyContext(http);
    ClientStreamData newClient = context_.getRaw();
    clientStreamInit(&http->client_stream, clientGetMoreData, clientReplyDetach,
                     clientReplyStatus, newServer, downloaderRecipient,
                     downloaderDetach, newClient, tempBuffer);

    // Build a ClientRequestContext to start doCallouts
    http->calloutContext = new ClientRequestContext(http);
    http->doCallouts();
    return true;
}

void
Downloader::start()
{
    if (!buildRequest())
        callBack(Http::scInternalServerError);
}

void
Downloader::handleReply(clientStreamNode * node, ClientHttpRequest *http, HttpReply *reply, StoreIOBuffer receivedData)
{
    DownloaderContext::Pointer callerContext = dynamic_cast<DownloaderContext *>(node->data.getRaw());
    // TODO: remove the following check:
    assert(callerContext == context_);

    debugs(33, 4, "Received " << receivedData.length <<
           " object data, offset: " << receivedData.offset <<
           " error flag:" << receivedData.flags.error);

    const bool failed = receivedData.flags.error;
    if (failed) {
        callBack(Http::scInternalServerError);
        return;
    }

    const int64_t existingContent = reply ? reply->content_length : 0;
    const size_t maxSize = MaxObjectSize > SBuf::maxSize ? SBuf::maxSize : MaxObjectSize;
    const bool tooLarge = (existingContent > -1 && existingContent > static_cast<int64_t>(maxSize)) ||
                          (maxSize < object_.length()) ||
                          ((maxSize - object_.length()) < receivedData.length);

    if (tooLarge) {
        callBack(Http::scInternalServerError);
        return;
    }

    object_.append(receivedData.data, receivedData.length);
    http->out.size += receivedData.length;
    http->out.offset += receivedData.length;

    switch (clientStreamStatus(node, http)) {
    case STREAM_NONE: {
        debugs(33, 3, "Get more data");
        StoreIOBuffer tempBuffer;
        tempBuffer.offset = http->out.offset;
        tempBuffer.data = context_->requestBuffer;
        tempBuffer.length = HTTP_REQBUF_SZ;
        clientStreamRead(node, http, tempBuffer);
    }
        break;
    case STREAM_COMPLETE:
        debugs(33, 3, "Object data transfer successfully complete");
        callBack(Http::scOkay);
        break;
    case STREAM_UNPLANNED_COMPLETE:
        debugs(33, 3, "Object data transfer failed: STREAM_UNPLANNED_COMPLETE");
        callBack(Http::scInternalServerError);
        break;
    case STREAM_FAILED:
        debugs(33, 3, "Object data transfer failed: STREAM_FAILED");
        callBack(Http::scInternalServerError);
        break;
    default:
        fatal("unreachable code");
    }
}

void
Downloader::downloadFinished()
{
    debugs(33, 7, this);
    // We cannot delay http destruction until refcounting deletes 
    // DownloaderContext. The http object destruction will cause 
    // clientStream cleanup and will release the refcount to context_
    // object hold by clientStream structures.
    context_->finished();
    context_ = nullptr;
    Must(done());
}

/// Schedules for execution the "callback" with parameters the status
/// and object.
void
Downloader::callBack(Http::StatusCode const statusCode)
{
     CbDialer *dialer = dynamic_cast<CbDialer*>(callback_->getDialer());
     Must(dialer);
     dialer->status = statusCode;
     if (statusCode == Http::scOkay)
         dialer->object = object_;
     ScheduleCallHere(callback_);
     callback_ = nullptr;

     // Calling deleteThis method here to finish Downloader
     // may result to squid crash.
     // This method called by handleReply method which maybe called
     // by ClientHttpRequest::doCallouts. The doCallouts after this object
     // deleted, may operate on non valid objects.
     // Schedule an async call here just to force squid to delete this object.
     CallJobHere(33, 7, CbcPointer<Downloader>(this), Downloader, downloadFinished);
}
