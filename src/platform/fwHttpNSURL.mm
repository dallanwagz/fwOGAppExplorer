// The iOS transport behind fwHttp.cpp's Apple-mobile branch. iOS ships no
// public libcurl, so the dlopen design the other POSIX platforms share has
// nothing to open; NSURLSession is the platform stack (and brings the same
// property the header promises: the OS keeps the trust store current).
//
// Blocking on purpose: httpGet()'s contract is a blocking call made from
// worker threads, and both existing callers already honor that. A semaphore
// over the async dataTask is the standard way to express it.
#include "platform/fwHttp.h"

#import <Foundation/Foundation.h>

// The same redirect rules the libcurl branch sets with CURLOPT_MAXREDIRS and
// CURLOPT_REDIR_PROTOCOLS: at most 10 hops, and never off https onto plain
// http (mayRedirectToPlainHttp() is the shared, tested statement of that
// policy). completionHandler(nil) refuses the hop; the 3xx response is then
// delivered as the result and rejected by the non-2xx check below, so a
// refused redirect is an error rather than a silent success with an HTML
// stub body.
@interface FwogRedirectPolicy : NSObject <NSURLSessionTaskDelegate>
@property(atomic) int  hops;
@property(atomic) BOOL allowPlainHttp;
@end

@implementation FwogRedirectPolicy
- (void)URLSession:(NSURLSession*)session
                     task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
               newRequest:(NSURLRequest*)request
        completionHandler:(void (^)(NSURLRequest* _Nullable))completionHandler
{
    (void)session; (void)task; (void)response;
    self.hops += 1;
    const BOOL toPlainHttp =
        [[request.URL.scheme lowercaseString] isEqualToString:@"http"];
    if (self.hops > 10 || (toPlainHttp && !self.allowPlainHttp)) {
        completionHandler(nil);
        return;
    }
    completionHandler(request);
}
@end

namespace fwog {
namespace detail {

std::expected<std::string, std::string> nsurlHttpGet(const std::string& url)
{
    @autoreleasepool {
        NSURL* nsUrl =
            [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!nsUrl) return std::unexpected("the URL could not be parsed");

        NSURLSessionConfiguration* cfg =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        // Mirrors the libcurl branch's time limits in spirit: 30 s of silence
        // fails, a slow-but-moving large body does not. timeoutIntervalForRequest
        // is an IDLE timeout (resets on data), which is exactly the
        // LOW_SPEED_LIMIT shape rather than the old total-cap mistake that
        // branch documents removing.
        cfg.timeoutIntervalForRequest  = 30.0;
        // timeoutIntervalForResource is a TOTAL wall-clock cap, and this
        // function also pulls firmware images (the largest in this repo is
        // 16,424,448 bytes), so it is sized the way the libcurl branch sizes
        // its backstop: never the thing that fails a real download.
        // 16,424,448 / 3600 = 4,562 B/s, i.e. the largest image still
        // completes on any link averaging 4.5 KiB/s; the idle timeout above
        // remains the real limiter.
        cfg.timeoutIntervalForResource = 3600.0;

        FwogRedirectPolicy* policy = [FwogRedirectPolicy new];
        policy.allowPlainHttp = mayRedirectToPlainHttp(url) ? YES : NO;

        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg
                                                              delegate:policy
                                                         delegateQueue:nil];

        __block std::expected<std::string, std::string> result =
            std::unexpected(std::string("the request produced no response"));
        dispatch_semaphore_t done = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task = [session
              dataTaskWithURL:nsUrl
            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                if (error) {
                    result = std::unexpected(
                        std::string([[error localizedDescription] UTF8String]));
                } else {
                    const NSInteger status =
                        [(NSHTTPURLResponse*)response statusCode];
                    if (status < 200 || status >= 300) {
                        result = std::unexpected("the server returned HTTP " +
                                                 std::to_string(status));
                    } else {
                        result = std::string(
                            static_cast<const char*>(data.bytes), data.length);
                    }
                }
                dispatch_semaphore_signal(done);
            }];
        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];
        return result;
    }
}

} // namespace detail
} // namespace fwog
