// See fwVolumeGrant.h. iOS only.
#include "platform/fwVolumeGrant.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <mutex>

static NSString* const kBookmarkKey = @"fwog.volumeGrant.bookmark";

namespace fwog { namespace grant { void clearGrantCacheForNewBookmark(); } }

// The picker delegate must outlive the (asynchronous) presentation, and
// UIKit holds delegates weakly -- a strong static keeps it alive between
// presentGrantPicker() and the user's tap.
@interface FwogGrantPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end

@implementation FwogGrantPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    (void)controller;
    NSURL* url = urls.firstObject;
    if (!url) return;
    const BOOL scoped = [url startAccessingSecurityScopedResource];
    NSData* bm = [url bookmarkDataWithOptions:0
               includingResourceValuesForKeys:nil
                                relativeToURL:nil
                                        error:nil];
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (bm) {
        [[NSUserDefaults standardUserDefaults] setObject:bm forKey:kBookmarkKey];
        // The next grantedVolumePath() must re-resolve rather than keep
        // serving a previously cached (possibly different) folder.
        fwog::grant::clearGrantCacheForNewBookmark();
    }
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
    (void)controller;
}
@end

namespace fwog {
namespace grant {

namespace {

std::mutex   g_mutex;
NSURL*       g_activeUrl = nil;       // scope opened, held for process life
FwogGrantPickerDelegate* g_delegate = nil;

/// Resolve the stored bookmark and open its scope, caching the URL. Returns
/// nil when there is no bookmark or it does not resolve (drive unplugged).
/// Caller holds g_mutex.
NSURL* resolvedUrlLocked()
{
    if (g_activeUrl) return g_activeUrl;
    NSData* bm = [[NSUserDefaults standardUserDefaults] dataForKey:kBookmarkKey];
    if (!bm) return nil;
    BOOL   stale = NO;
    NSURL* url = [NSURL URLByResolvingBookmarkData:bm
                                           options:0
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:nil];
    if (!url) return nil;
    if (![url startAccessingSecurityScopedResource]) return nil;
    if (stale) {
        // Refresh the stored bookmark while the URL is live, so staleness
        // does not accumulate into a bookmark that one day stops resolving.
        if (NSData* fresh = [url bookmarkDataWithOptions:0
                          includingResourceValuesForKeys:nil
                                           relativeToURL:nil
                                                   error:nil])
            [[NSUserDefaults standardUserDefaults] setObject:fresh forKey:kBookmarkKey];
    }
    g_activeUrl = url;
    return g_activeUrl;
}

} // namespace

void clearGrantCacheForNewBookmark()
{
    std::lock_guard lock(g_mutex);
    if (g_activeUrl) {
        [g_activeUrl stopAccessingSecurityScopedResource];
        g_activeUrl = nil;
    }
}

bool hasGrant()
{
    return [[NSUserDefaults standardUserDefaults] dataForKey:kBookmarkKey] != nil;
}

std::optional<std::string> grantedVolumePath()
{
    std::lock_guard lock(g_mutex);
    NSURL* url = resolvedUrlLocked();
    if (!url) return std::nullopt;
    // Reachability is asked of the filesystem, not assumed from the resolve:
    // a bookmark can resolve to a URL whose volume left five seconds ago.
    // checkResourceIsReachableAndReturnError is the documented way to ask.
    if (![url checkResourceIsReachableAndReturnError:nil]) {
        // Drop the cached URL so a replugged drive re-resolves fresh rather
        // than being judged through a stale handle.
        [g_activeUrl stopAccessingSecurityScopedResource];
        g_activeUrl = nil;
        return std::nullopt;
    }
    return std::string(url.fileSystemRepresentation);
}

void presentGrantPicker()
{
    dispatch_async(dispatch_get_main_queue(), ^{
        SDL_Window* window = nullptr;
        if (int n = 0; true) {
            SDL_Window** ws = SDL_GetWindows(&n);
            if (ws && n > 0) window = ws[0];
            SDL_free(ws);
        }
        if (!window) return;
        UIWindow* uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,
            nullptr);
        UIViewController* root = uiWindow.rootViewController;
        if (!root) return;

        if (!g_delegate) g_delegate = [FwogGrantPickerDelegate new];
        UIDocumentPickerViewController* picker =
            [[UIDocumentPickerViewController alloc]
                initForOpeningContentTypes:@[ UTTypeFolder ]];
        picker.delegate = g_delegate;
        [root presentViewController:picker animated:YES completion:nil];
    });
}

void clearGrant()
{
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kBookmarkKey];
    clearGrantCacheForNewBookmark();
}

namespace {

/// The uncached status computation: a defaults read, possibly a bookmark
/// resolve, and always a reachability stat -- disk I/O, every time.
/// Caller holds g_mutex.
std::string statusDescriptionLocked()
{
    NSData* bm = [[NSUserDefaults standardUserDefaults] dataForKey:kBookmarkKey];
    if (!bm) return "no drive granted yet";

    if (!g_activeUrl) {
        BOOL   stale = NO;
        NSURL* url = [NSURL URLByResolvingBookmarkData:bm
                                               options:0
                                         relativeToURL:nil
                                   bookmarkDataIsStale:&stale
                                                 error:nil];
        if (!url)
            return "grant no longer resolves (drive unplugged, or the grant "
                   "died -- if the drive IS mounted, re-grant)";
        if (![url startAccessingSecurityScopedResource])
            return "grant resolved but access was refused -- re-grant";
        g_activeUrl = url;
    }
    if (![g_activeUrl checkResourceIsReachableAndReturnError:nil]) {
        [g_activeUrl stopAccessingSecurityScopedResource];
        g_activeUrl = nil;
        return "granted; drive not currently mounted";
    }
    return std::string("granted; mounted at ") + g_activeUrl.fileSystemRepresentation;
}

} // namespace

std::string statusDescription()
{
    // Called once per frame from fwDeviceBar's draw -- its only caller, so
    // this runs on the UI thread alone and the cache below needs no lock of
    // its own. The real computation is disk I/O, which does not belong on a
    // frame, so two throttles gate it: recompute at most every 500 ms, and
    // only when g_mutex is free. The try_to_lock is not optional -- the
    // flash worker can hold g_mutex for the duration of a write, and a
    // blocking lock here would freeze frames for exactly that long.
    static std::string cached = "checking drive grant...";
    static std::chrono::steady_clock::time_point lastRefresh{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastRefresh < std::chrono::milliseconds(500)) return cached;
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return cached;
    lastRefresh = now;
    cached = statusDescriptionLocked();
    return cached;
}

} // namespace grant
} // namespace fwog
