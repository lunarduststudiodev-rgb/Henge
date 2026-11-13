// Runs inside macOS on a Henge virtual Mac, as a LaunchAgent. It keeps the clipboard in step with the device,
// keeps the shared folder mounted, and makes touch feel native. It talks to host/share.c at 10.0.2.4, which only
// exists inside Henge.
//
// Touch: the app moves the pointer and clicks through the Mac's absolute pointer device on its own. What only
// something inside macOS can do is here, over a link that stays open (see share.c for its lines):
// - say what is under a finger, so a drag on a title bar, a slider or a menu takes hold at once, while a drag
//   on a page scrolls it;
// - scroll as a trackpad does: by the pixel, with the phases apps use for rubber-banding at the ends, and a
//   glide after a flick that apps see as momentum.
// Both need the Accessibility permission (System Settings > Privacy & Security > Accessibility).
#define __STDC_WANT_LIB_EXT1__ 1 // memset_s
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <AVFoundation/AVFoundation.h> // the benchmark: its classes are looked up by name, so nothing here links it
#import <WebKit/WebKit.h>             // the same
#import <Carbon/Carbon.h> // IsSecureEventInputEnabled
#import <IOKit/IOKitLib.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <IOKit/hidsystem/IOHIDEventSystemClient.h>
#import <IOKit/hidsystem/IOHIDServiceClient.h>
#import <AudioToolbox/AudioServices.h>
#import <CoreAudio/CoreAudio.h>

#ifndef kAudioObjectPropertyElementMaster
#define kAudioObjectPropertyElementMaster 0
#endif
#ifndef kAudioHardwareServiceDeviceProperty_VirtualMasterVolume
#define kAudioHardwareServiceDeviceProperty_VirtualMasterVolume 'vmvc'
#endif
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <grp.h>
#include <mach/machine.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "../host/share.h"
#include "agent-ports.h"
#include "focus-said.h"

#define HOST HV_SHARE_ADDR

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, 0);
        if (w <= 0) return -1;
        c += w, n -= (size_t)w;
    }
    return 0;
}

static int connect_host(int timeout) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval t = {.tv_sec = timeout};
    if (timeout) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(80)};
    inet_pton(AF_INET, HOST, &addr.sin_addr);
    const char *test = getenv("HENGE_AGENT_TEST_PORT"); // tests only: a server on this machine
    if (test) {
        addr.sin_port = htons((uint16_t)atoi(test));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// One request on its own connection. A plain socket: the server is ours, and URLSession refuses plain HTTP.
static NSData *request(NSString *method, NSString *path, NSData *body, int *status, unsigned long *seq) {
    int fd = connect_host(40);
    if (fd < 0) return nil;
    NSString *head = [NSString stringWithFormat:@"%@ %@ HTTP/1.1\r\nHost: " HOST "\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n", method, path,
                                                (unsigned long)body.length];
    const char *text = head.UTF8String;
    if (send_all(fd, text, strlen(text)) != 0 || (body.length && send_all(fd, body.bytes, body.length) != 0)) {
        close(fd);
        return nil;
    }
    NSMutableData *all = [NSMutableData data];
    char piece[16384];
    for (;;) {
        ssize_t got = recv(fd, piece, sizeof piece, 0);
        if (got <= 0) break;
        [all appendBytes:piece length:(NSUInteger)got];
    }
    close(fd);
    NSRange end = [all rangeOfData:[NSData dataWithBytes:"\r\n\r\n" length:4] options:0 range:NSMakeRange(0, all.length)];
    if (end.location == NSNotFound) return nil;
    NSString *headers = [[NSString alloc] initWithData:[all subdataWithRange:NSMakeRange(0, end.location)] encoding:NSUTF8StringEncoding];
    if (sscanf(headers.UTF8String, "HTTP/1.1 %d", status) != 1) return nil;
    NSRange found = [headers rangeOfString:@"X-Henge-Seq: "];
    if (found.location != NSNotFound && seq) *seq = strtoul(headers.UTF8String + NSMaxRange(found), NULL, 10);
    NSUInteger start = NSMaxRange(end);
    return [all subdataWithRange:NSMakeRange(start, all.length - start)];
}

// MARK: - Clipboard and shared folder

static NSInteger lastChange;

// The device's clipboard, as it changes. The server holds each request until there is something new.
static void followDevice(void) {
    unsigned long seq = 0;
    for (BOOL first = YES;; first = NO) {
        int status = 0;
        unsigned long now = seq;
        NSData *body = request(@"GET", [NSString stringWithFormat:@"/.henge/clipboard?since=%lu", first ? ULONG_MAX >> 1 : seq], nil, &status, &now);
        if (!body) {
            sleep(5);
            continue;
        }
        BOOL news = !first && status == 200 && now != seq;
        seq = now; // what the device had before this helper started isn't news
        if (!news) continue;
        NSString *text = [[NSString alloc] initWithData:body encoding:NSUTF8StringEncoding];
        if (!text) continue;
        dispatch_async(dispatch_get_main_queue(), ^{
            NSPasteboard *board = NSPasteboard.generalPasteboard;
            [board clearContents];
            [board setString:text forType:NSPasteboardTypeString];
            lastChange = board.changeCount;
        });
    }
}

// The Mac's clipboard goes to the device, unless the app that copied it marked it as secret.
static void checkClipboard(void) {
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    if (board.changeCount == lastChange) return;
    lastChange = board.changeCount;
    if ([board.types containsObject:@"org.nspasteboard.ConcealedType"]) return;
    NSString *text = [board stringForType:NSPasteboardTypeString];
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    if (!data.length) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int status = 0;
        request(@"PUT", @"/.henge/clipboard", data, &status, NULL);
    });
}

// Tests (--keep-mounted): a stand-in home folder and shared-folder volume.
static NSString *home(void) { return getenv("HENGE_AGENT_TEST_HOME") ? @(getenv("HENGE_AGENT_TEST_HOME")) : NSHomeDirectory(); }
static NSString *mountPoint(void) { return [home() stringByAppendingPathComponent:@"Henge"]; }

static void note(NSString *text);
static void answerTouchWatch(unsigned id); // the touchscreen, below

// macOS's own shared-folder volume (devices/virtio_fs.c), which it mounts by itself as "My Shared Files". Its top
// level is one folder per share: the device's (helper version 5 also had a read-only Henge Tools). The one the
// person can write to, or failing that the first: macOS may show the owner differently from the device, and the
// folder still works. Why there is none, for Henge's log, when there isn't.
static NSString *volumeTrouble;

static NSString *sharedFolderOnVolume(void) {
    struct statfs *mounts;
    int n = getmntinfo(&mounts, MNT_NOWAIT);
    const char *test = getenv("HENGE_AGENT_TEST_VOLUME");
    if (test) n = 1;
    NSMutableArray<NSString *> *trouble = [NSMutableArray new];
    for (int i = 0; i < n; i++) {
        // macOS calls the type "AppleVirtIOFS" (seen in a virtual Mac, 2026-11-07); any spelling of virtiofs counts.
        if (!test && !strcasestr(mounts[i].f_fstypename, "virtiofs") && strcmp(mounts[i].f_mntfromname, "com.apple.virtio-fs.automount") != 0) continue;
        NSString *volume = test ? @(test) : @(mounts[i].f_mntonname);
        NSError *error;
        NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:volume error:&error];
        if (!names) {
            NSError *under = error.userInfo[NSUnderlyingErrorKey];
            [trouble addObject:[NSString stringWithFormat:@"%@ can't be read (%@)", volume, under.localizedDescription ?: error.localizedDescription]];
            continue;
        }
        NSString *first;
        for (NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
            NSString *path = [volume stringByAppendingPathComponent:name];
            BOOL folder = NO;
            if ([name isEqualToString:@"Henge Tools"] || [name hasPrefix:@"."]) continue;
            if (![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&folder] || !folder) continue;
            if (access(path.fileSystemRepresentation, W_OK) == 0) return path;
            first = first ?: path;
        }
        if (first) return first;
        [trouble addObject:[NSString stringWithFormat:@"%@ has no folder in it (%lu items)", volume, (unsigned long)names.count]];
    }
    if (!trouble.count) {
        NSMutableArray<NSString *> *seen = [NSMutableArray new];
        for (int i = 0; i < n && !test; i++)
            if (strncmp(mounts[i].f_mntonname, "/System/Volumes/", 16) && strcmp(mounts[i].f_fstypename, "devfs") && strcmp(mounts[i].f_fstypename, "autofs"))
                [seen addObject:[NSString stringWithFormat:@"%s at %s", mounts[i].f_fstypename, mounts[i].f_mntonname]];
        [trouble addObject:[@"no virtio-fs mount; mounted: " stringByAppendingString:[seen componentsJoinedByString:@", "]]];
    }
    volumeTrouble = [trouble componentsJoinedByString:@"; "];
    return nil;
}

static BOOL isEmptyFolder(NSString *path) {
    NSArray *items = [NSFileManager.defaultManager contentsOfDirectoryAtPath:path error:NULL];
    for (NSString *name in items)
        if (![name isEqualToString:@".DS_Store"] && ![name hasPrefix:@"._"]) return NO;
    return items != nil;
}

// Henge's WebDAV mounts from before the shared-folder volume: Finder lists each by the address, 10.0.2.4, and
// they show the same files as the volume, slower. Usually at ~/Henge, though one mounted from Finder's Connect to
// Server could be anywhere. In use, each is tried again every 30 s; after 5 minutes it goes anyway, as an app still
// holding a file in one would hold it forever. Tests: mounts in the test home stand in for them.
static void disconnectOldNetworkFolders(NSString *volume) {
    static NSMutableDictionary<NSString *, NSNumber *> *busy; // tries each was in use
    if (!busy) busy = [NSMutableDictionary new];
    const char *test = getenv("HENGE_AGENT_TEST_HOME");
    struct statfs *mounts;
    int n = getmntinfo(&mounts, MNT_NOWAIT);
    NSMutableArray<NSString *> *old = [NSMutableArray new];
    for (int i = 0; i < n; i++) {
        NSString *on = @(mounts[i].f_mntonname);
        BOOL ours = !strcmp(mounts[i].f_fstypename, "webdav") && strstr(mounts[i].f_mntfromname, HOST);
        if (test) ours = [on hasPrefix:@(test)] && ![on isEqualToString:volume];
        if (ours) [old addObject:on];
    }
    for (NSString *on in old) {
        NSString *shown = [on hasPrefix:home()] ? [@"~" stringByAppendingString:[on substringFromIndex:home().length]] : on;
        int tries = busy[on].intValue;
        BOOL force = tries >= 10;
        if (unmount(on.fileSystemRepresentation, force ? MNT_FORCE : 0) != 0) {
            if (tries == 0) note([NSString stringWithFormat:@"the old network folder at %@ is in use; it goes when it isn't", shown]);
            busy[on] = @(tries + 1);
            continue;
        }
        note([NSString stringWithFormat:force ? @"the old network folder at %@ was in use for 5 minutes; disconnected it anyway"
                                              : @"disconnected the old network folder at %@", shown]);
        [busy removeObjectForKey:on];
    }
}

// The shared folder, at ~/Henge. With the shared-folder volume (Henge 2026-11 and later), ~/Henge points into
// it: it's faster than WebDAV and Finder shows it by itself. Otherwise WebDAV, as before: mount_webdav needs an
// empty folder it owns, and is a no-op failure if it is mounted. Nothing the person put in ~/Henge is touched.
static void keepMounted(void) {
    NSString *point = mountPoint();
    const char *p = point.fileSystemRepresentation;
    NSString *volume = sharedFolderOnVolume();
    // How things stand, for Henge's log: at the start, and whenever that changes (macOS can mount it late).
    static NSString *reported;
    NSString *now = volume ? [@"the shared folder is " stringByAppendingString:volume]
                           : [NSString stringWithFormat:@"no shared-folder volume (%@): the shared folder is WebDAV at ~/Henge", volumeTrouble];
    if (![now isEqualToString:reported]) note(now);
    reported = now;
    if (volume) disconnectOldNetworkFolders(volume);
    struct stat here, homeInfo;
    BOOL exists = lstat(p, &here) == 0;
    if (stat(home().fileSystemRepresentation, &homeInfo) != 0) return;
    if (exists && S_ISLNK(here.st_mode)) {
        NSString *to = [NSFileManager.defaultManager destinationOfSymbolicLinkAtPath:point error:NULL];
        if (volume && [to isEqualToString:volume]) return;
        unlink(p); // into a volume that is gone, or another: made again below
        exists = NO;
    }
    if (volume) {
        if (exists && S_ISDIR(here.st_mode) && here.st_dev != homeInfo.st_dev) return; // still mounted: in use, or not ours
        static BOOL told;
        if (exists && (!S_ISDIR(here.st_mode) || !isEmptyFolder(point))) {
            if (!told) note(@"~/Henge has the person's own files in it, so it stays; the shared folder is in My Shared Files");
            told = YES;
            return;
        }
        if (exists) {
            [NSFileManager.defaultManager removeItemAtPath:[point stringByAppendingPathComponent:@".DS_Store"] error:NULL];
            if (rmdir(p) != 0) return;
        }
        if (symlink(volume.fileSystemRepresentation, p) == 0) note([@"~/Henge is now " stringByAppendingString:volume]);
        return;
    }
    [NSFileManager.defaultManager createDirectoryAtPath:point withIntermediateDirectories:YES attributes:nil error:NULL];
    if (stat(p, &here) != 0 || here.st_dev != homeInfo.st_dev) return; // already something else's volume: mounted
    if (getenv("HENGE_AGENT_TEST_HOME")) return;
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/sbin/mount_webdav"];
    task.arguments = @[ @"-S", @"-v", @"Henge", @"http://" HOST "/", point ];
    task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    [task launchAndReturnError:NULL];
}

// For Install Henge Tools.app, which can't ask macOS whether another program is trusted.
static void writeStatus(BOOL isTrusted) {
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Henge"];
    NSString *text = [NSString stringWithFormat:@"trusted %d\nversion %d\n", isTrusted ? 1 : 0, HV_AGENT_VERSION];
    [text writeToFile:[dir stringByAppendingPathComponent:@"status"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

// MARK: - The link

static pthread_mutex_t linkLock = PTHREAD_MUTEX_INITIALIZER;
static int linkFd = -1;
static uint64_t linkGeneration; // one more for each link made (under linkLock)

static void linkSend(NSString *line) {
    const char *text = line.UTF8String;
    pthread_mutex_lock(&linkLock);
    if (linkFd >= 0 && send_all(linkFd, text, strlen(text)) != 0) shutdown(linkFd, SHUT_RDWR);
    pthread_mutex_unlock(&linkLock);
}

// The link now: its generation (0 while there is none), for linkSendOn.
static uint64_t linkNow(void) {
    pthread_mutex_lock(&linkLock);
    uint64_t gen = linkFd >= 0 ? linkGeneration : 0;
    pthread_mutex_unlock(&linkLock);
    return gen;
}

// Sends only on the link of that generation: an answer to something a link asked never goes to a newer one.
static void linkSendOn(uint64_t gen, NSString *line) {
    const char *text = line.UTF8String;
    pthread_mutex_lock(&linkLock);
    if (linkFd >= 0 && linkGeneration == gen && send_all(linkFd, text, strlen(text)) != 0) shutdown(linkFd, SHUT_RDWR);
    pthread_mutex_unlock(&linkLock);
}

// What this helper did, in the Mac's log and Henge's (henge.log): sent when the link is up, as it may not be yet.
static NSMutableArray<NSString *> *notes;

static void flushNotes(void) {
    pthread_mutex_lock(&linkLock);
    while (linkFd >= 0 && notes.count) {
        NSString *line = [NSString stringWithFormat:@"note %@\n", notes.firstObject];
        if (send_all(linkFd, line.UTF8String, strlen(line.UTF8String)) != 0) {
            shutdown(linkFd, SHUT_RDWR);
            break;
        }
        [notes removeObjectAtIndex:0];
    }
    pthread_mutex_unlock(&linkLock);
}

static void note(NSString *text) {
    NSLog(@"henge-agent: %@", text);
    NSString *line = [[text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" "];
    // Henge reads a line into 256 bytes, and before 2026-11-07 dropped the link at a longer one (the note with it);
    // its log keeps 159 bytes of a note. So a long note goes in pieces of at most 140 bytes of UTF-8 (plus the
    // marks between them), cut between characters.
    NSMutableArray<NSString *> *pieces = [NSMutableArray new];
    while ([line lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 140) {
        // Back from 140 to a cut between characters (half an emoji measures as 0 bytes) that is 140 bytes or less.
        NSUInteger at = MIN(line.length - 1, 140);
        while (at > 4) {
            at = [line rangeOfComposedCharacterSequenceAtIndex:at].location;
            if (at <= 4 || [[line substringToIndex:at] lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 140) break;
            at--;
        }
        if (at <= 4) at = MIN(line.length - 1, 70); // one "character" longer than a piece: cut anyhow
        [pieces addObject:[[line substringToIndex:at] stringByAppendingString:@" ..."]];
        line = [@"... " stringByAppendingString:[line substringFromIndex:at]];
    }
    [pieces addObject:line];
    if (getenv("HENGE_AGENT_TEST_HOME"))
        for (NSString *piece in pieces) printf("note: %s\n", piece.UTF8String);
    pthread_mutex_lock(&linkLock);
    if (!notes) notes = [NSMutableArray new];
    for (NSString *piece in pieces)
        if (notes.count < 16) [notes addObject:piece];
    pthread_mutex_unlock(&linkLock);
    flushNotes();
}

static BOOL trusted(void) { return AXIsProcessTrusted(); }

// macOS's request for the permission, which also puts this helper in the list in System Settings.
static void askForTrust(void) {
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

// From the card on the device ("ask"): macOS shows its own prompt once at most, so this opens the very list in
// System Settings where henge-agent is turned on.
static void showTrustSettings(void) {
    if (trusted()) return;
    askForTrust();
    NSURL *pane = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
    if (pane) [NSWorkspace.sharedWorkspace openURL:pane];
    note(@"opened Accessibility in System Settings, as asked from the device");
}

// MARK: - USB drives and the Henge Tools disk

// The device plugs disk images into the Mac as USB drives while it runs, and the Henge Tools disk the same way
// (USB serial HENGE-TOOLS) whenever the helper on the Mac isn't this version: so it's on the desktop, installer
// and all, until Henge Tools is in. This helper ejects drives when asked from the device ("eject <serial>": the
// device unplugs a drive once macOS has ejected it, so nothing it held is lost), ejects the Henge Tools disk when
// it's no longer needed (it is running, so it's up to date), and opens the installer when asked ("tools": to
// update or remove Henge Tools; the device plugs the disk in just after), ejecting the disk when it quits.
static dispatch_queue_t toolsQueue; // everything here runs on it
static _Atomic(BOOL) toolsWanted; // set on the link's thread too
static BOOL toolsBusyTold;
static uint64_t toolsTryUntil; // ejects the disk until then (it may arrive after this starts)
#define TOOLS_INSTALLER @"com.henge.tools-installer"

static NSString *toolsName(void) {
    const char *test = getenv("HENGE_AGENT_TEST_TOOLS"); // tests: their own disk image, never the real one
    return test ? @(test) : @"Henge Tools";
}

static int diskutil(NSArray<NSString *> *args, NSData **output) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/diskutil"];
    task.arguments = args;
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    if (![task launchAndReturnError:NULL]) return -1;
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (output) *output = data;
    return task.terminationStatus;
}

// Where the disk is mounted, or nil: a read-only volume of its name in /Volumes (Finder adds " 1" to a second
// of a name) with the installer on it.
static NSString *toolsVolume(void) {
    struct statfs *mounts;
    int n = getmntinfo(&mounts, MNT_NOWAIT);
    NSString *name = toolsName();
    for (int i = 0; i < n; i++) {
        if (!(mounts[i].f_flags & MNT_RDONLY)) continue;
        NSString *on = @(mounts[i].f_mntonname);
        NSString *last = on.lastPathComponent;
        if (![on.stringByDeletingLastPathComponent isEqualToString:@"/Volumes"]) continue;
        if (![last isEqualToString:name] && ![last hasPrefix:[name stringByAppendingString:@" "]]) continue;
        if ([NSFileManager.defaultManager fileExistsAtPath:[on stringByAppendingPathComponent:@"Install Henge Tools.app"]]) return on;
    }
    return nil;
}

// The disk there but not mounted (someone unmounted it in Disk Utility): its device, by the volume's name.
static NSString *toolsDevice(void) {
    NSData *data;
    if (diskutil(@[ @"list", @"-plist" ], &data) != 0 || !data.length) return nil;
    NSDictionary *list = [NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
    for (NSDictionary *disk in list[@"AllDisksAndPartitions"]) {
        NSMutableArray *volumes = [NSMutableArray arrayWithObject:disk];
        if ([disk[@"Partitions"] isKindOfClass:NSArray.class]) [volumes addObjectsFromArray:disk[@"Partitions"]];
        for (NSDictionary *v in volumes)
            if ([v[@"VolumeName"] isEqual:toolsName()] && [v[@"DeviceIdentifier"] isKindOfClass:NSString.class]) return v[@"DeviceIdentifier"];
    }
    return nil;
}

// The whole disk ("disk5") of the USB drive with this serial number, or nil.
static NSString *driveWithSerial(NSString *serial) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMedia"), &it) != KERN_SUCCESS) return nil;
    NSString *found = nil;
    for (io_object_t media; !found && (media = IOIteratorNext(it));) {
        CFTypeRef whole = IORegistryEntryCreateCFProperty(media, CFSTR("Whole"), kCFAllocatorDefault, 0);
        if (whole == kCFBooleanTrue) {
            CFTypeRef sn = IORegistryEntrySearchCFProperty(media, kIOServicePlane, CFSTR("USB Serial Number"), kCFAllocatorDefault,
                                                           kIORegistryIterateRecursively | kIORegistryIterateParents);
            if (sn && CFGetTypeID(sn) == CFStringGetTypeID() && [(__bridge NSString *)sn isEqualToString:serial]) {
                CFTypeRef bsd = IORegistryEntryCreateCFProperty(media, CFSTR("BSD Name"), kCFAllocatorDefault, 0);
                if (bsd && CFGetTypeID(bsd) == CFStringGetTypeID()) found = [(__bridge NSString *)bsd copy];
                if (bsd) CFRelease(bsd);
            }
            if (sn) CFRelease(sn);
        }
        if (whole) CFRelease(whole);
        IOObjectRelease(media);
    }
    IOObjectRelease(it);
    return found;
}

// Asked from the device: ejects the drive as Finder's eject button does. The device hears "ejected <serial> 1",
// or 0 when macOS won't (something on it is open) or no such drive is here.
static void ejectDrive(NSString *serial) {
    NSString *disk = driveWithSerial(serial);
    int ok = disk && diskutil(@[ @"eject", disk ], NULL) == 0;
    note(ok ? [NSString stringWithFormat:@"ejected the drive %@ (%@), as asked from the device", serial, disk]
            : disk ? [NSString stringWithFormat:@"couldn't eject the drive %@ (%@): something on it is in use", serial, disk]
                   : [NSString stringWithFormat:@"no drive %@ to eject", serial]);
    linkSend([NSString stringWithFormat:@"ejected %@ %d\n", serial, ok]);
}

// Asked from the device for a new disk it made (ios/Henge/Native/henge-disk.c: an empty FAT32 file system on the
// whole disk, which macOS mounts at once): once the drive is here (up to half a minute), and only if it's still
// empty, erases it as APFS called name, a Mac disk. The device hears "formatted <serial> 1", or 0.
//
// Empty: one file system on the whole disk (no partition map), mounted, with nothing in it but what macOS puts on
// a disk by itself (names starting with a dot); or nothing macOS can read at all. A disk with anything else, or
// one that isn't mounted (nothing to look in), is left alone.
static BOOL blankDisk(NSString *disk) {
    NSData *data;
    if (diskutil(@[ @"list", @"-plist", disk ], &data) != 0 || !data.length) return NO;
    NSDictionary *list = [NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
    NSArray *disks = list[@"AllDisksAndPartitions"];
    if (![disks isKindOfClass:NSArray.class] || disks.count != 1) return NO;
    NSDictionary *d = disks[0];
    if (![d[@"DeviceIdentifier"] isEqual:disk]) return NO;
    if ([d[@"Partitions"] isKindOfClass:NSArray.class] && [d[@"Partitions"] count]) return NO;
    if ([d[@"APFSVolumes"] isKindOfClass:NSArray.class] && [d[@"APFSVolumes"] count]) return NO;
    id content = d[@"Content"];
    if (content && !([content isKindOfClass:NSString.class] && [content length] == 0)) return NO;
    if (!d[@"VolumeName"] && !d[@"MountPoint"]) return YES; // nothing macOS can read
    NSData *info;
    if (diskutil(@[ @"info", @"-plist", disk ], &info) != 0 || !info.length) return NO;
    NSDictionary *about = [NSPropertyListSerialization propertyListWithData:info options:0 format:NULL error:NULL];
    NSString *on = about[@"MountPoint"];
    if (![on isKindOfClass:NSString.class] || !on.length) return NO;
    NSError *error = nil;
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:on error:&error];
    if (!names) return NO;
    for (NSString *name in names)
        if (![name hasPrefix:@"."]) return NO;
    return YES;
}

static int formatBlank(NSString *disk, NSString *name) {
    if (!blankDisk(disk)) return 0;
    return diskutil(@[ @"eraseDisk", @"APFS", name, @"GPT", disk ], NULL) == 0;
}

static void formatDrive(NSString *serial, NSString *name) {
    NSString *disk = nil;
    for (int i = 0; i < 60 && !(disk = driveWithSerial(serial)); i++) usleep(500000);
    int ok = 0;
    for (int i = 0; disk && !ok && i < 10; i++) { // macOS may still be mounting it
        if (i) sleep(1);
        ok = formatBlank(disk, name);
    }
    note(ok ? [NSString stringWithFormat:@"set up the new disk %@ (%@) as APFS, as asked from the device", name, disk]
            : disk ? [NSString stringWithFormat:@"didn't set up %@ (%@): it isn't blank, or macOS couldn't erase it", serial, disk]
                   : [NSString stringWithFormat:@"no drive %@ arrived to set up", serial]);
    linkSend([NSString stringWithFormat:@"formatted %@ %d\n", serial, ok]);
}

static BOOL installerRunning(void) {
    return [NSRunningApplication runningApplicationsWithBundleIdentifier:TOOLS_INSTALLER].count > 0;
}

// Ejects the Henge Tools disk unless it's wanted: the installer is open, or about to be.
static void ejectToolsDisk(void) {
    if (toolsWanted || installerRunning()) return;
    NSString *volume = toolsVolume();
    NSString *where = volume ?: toolsDevice();
    if (!where) return;
    if (diskutil(@[ @"eject", where ], NULL) == 0) {
        toolsBusyTold = NO;
        note(@"ejected the Henge Tools disk (Henge Tools is in; Details on the device opens the installer again)");
    } else if (!toolsBusyTold) {
        toolsBusyTold = YES;
        note(@"the Henge Tools disk is in use; it's ejected once it isn't");
    }
}

// Asked from the device, which plugs the disk in just after: waits up to a minute for macOS to mount it.
static void openToolsInstaller(void) {
    toolsWanted = YES;
    NSString *volume = nil;
    for (int i = 0; i < 120 && !(volume = toolsVolume()); i++) {
        if (i % 10 == 9) {
            NSString *device = toolsDevice();
            if (device) diskutil(@[ @"mount", device ], NULL);
        }
        usleep(500000);
    }
    if (!volume) {
        toolsWanted = NO;
        note(@"couldn't find the Henge Tools disk to open the installer from");
        return;
    }
    NSString *app = [volume stringByAppendingPathComponent:@"Install Henge Tools.app"];
    if (getenv("HENGE_AGENT_TEST_TOOLS")) {
        printf("would open %s\n", app.fileSystemRepresentation);
        return;
    }
    NSWorkspaceOpenConfiguration *how = [NSWorkspaceOpenConfiguration configuration];
    how.activates = YES;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:app] configuration:how
                                    completionHandler:^(NSRunningApplication *opened, NSError *error) {
        (void)opened;
        note(error ? [@"couldn't open the Henge Tools installer: " stringByAppendingString:error.localizedDescription]
                   : @"opened the Henge Tools installer, as asked from the device");
        if (error) dispatch_async(toolsQueue, ^{ // nothing to keep the disk for
            toolsWanted = NO;
            ejectToolsDisk();
        });
    }];
}

// For ten minutes from the start, and from each time the installer quits: the disk goes once it isn't needed.
static void watchToolsDisk(void) {
    toolsTryUntil = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 600 * NSEC_PER_SEC;
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, toolsQueue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC, NSEC_PER_SEC);
    static dispatch_source_t kept; // a timer no one holds never fires
    kept = timer;
    (void)kept;
    dispatch_source_set_event_handler(timer, ^{
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > toolsTryUntil) return;
        ejectToolsDisk();
    });
    dispatch_resume(timer);
    [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidTerminateApplicationNotification object:nil
                                                                  queue:nil usingBlock:^(NSNotification *n) {
        NSRunningApplication *app = n.userInfo[NSWorkspaceApplicationKey];
        if (![app.bundleIdentifier isEqualToString:TOOLS_INSTALLER]) return;
        dispatch_async(toolsQueue, ^{
            toolsWanted = NO;
            toolsTryUntil = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 600 * NSEC_PER_SEC;
            ejectToolsDisk();
        });
    }];
}

// MARK: - What has the keyboard's focus

// What has the keyboard's focus, so the device can show its keyboard for a text field, hide it after, and offer the
// Mac's password for a password field. Told as one numbered line per change (version 28):
//   field SERIAL ID STATE KIND [X Y W H]
// STATE is text, secure, none or unknown. "none" only when Accessibility positively says the focus is on something
// that takes no typing (or on nothing); a timeout, "cannot complete", an element that went away while a page
// re-rendered, or a missing role is "unknown", and the device changes nothing for it. ID goes up whenever the
// focused element changes (compared with CFEqual), so the device tells "the same field" from "another field of the
// same kind". KIND is the field's (fieldKind) and X Y W H where it is, as fractions of the screen. macOS turns on
// secure input for any password field (a web page's, an app's, an authentication dialog, sudo in Terminal) and no app
// can read keys then; that needs no permission. Which field has the focus needs Accessibility.
static NSString *rangeText(AXUIElementRef e, CFIndex at, CFIndex length) {
    if (length <= 0) return @"";
    CFRange r = CFRangeMake(at, length);
    AXValueRef param = AXValueCreate(kAXValueTypeCFRange, &r);
    CFTypeRef v = NULL;
    AXError error = param ? AXUIElementCopyParameterizedAttributeValue(e, kAXStringForRangeParameterizedAttribute, param, &v) : kAXErrorFailure;
    if (param) CFRelease(param);
    if (error == kAXErrorSuccess && v && CFGetTypeID(v) == CFStringGetTypeID()) return CFBridgingRelease(v);
    if (v) CFRelease(v);
    v = NULL; // apps without the range question: the whole text, if it is short
    if (AXUIElementCopyAttributeValue(e, kAXValueAttribute, &v) != kAXErrorSuccess || !v) return nil;
    NSString *s = CFBridgingRelease(v);
    if (![s isKindOfClass:NSString.class] || s.length > 20000 || at < 0 || at + length > (CFIndex)s.length) return nil;
    return [s substringWithRange:NSMakeRange((NSUInteger)at, (NSUInteger)length)];
}

// The text around a field's caret, so the device's keyboard suggests and corrects words with what is already there:
// "SELECTED BEFORE AFTER", with up to 120 characters before the caret, and from the caret what is selected (a selected
// word gets the keyboard's corrections) and 40 more, each base64 (UTF-8) or "-". Sent as "ctx ID ..." with the field's
// ID, so the device drops what is left over from another field. Only ever for a text field, never a password's; nil
// if the field won't say.
static NSString *caretContext(AXUIElementRef e) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, kAXSelectedTextRangeAttribute, &v) != kAXErrorSuccess || !v) return nil;
    CFRange sel = {0, 0};
    BOOL ok = CFGetTypeID(v) == AXValueGetTypeID() && AXValueGetType(v) == kAXValueTypeCFRange && AXValueGetValue(v, kAXValueTypeCFRange, &sel);
    CFRelease(v);
    if (!ok || sel.location < 0 || sel.length < 0) return nil;
    CFIndex total = -1;
    v = NULL;
    if (AXUIElementCopyAttributeValue(e, kAXNumberOfCharactersAttribute, &v) == kAXErrorSuccess && v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberCFIndexType, &total);
        CFRelease(v);
    }
    CFIndex start = MAX(0, sel.location - 120);
    NSString *before = rangeText(e, start, sel.location - start);
    if (!before) return nil;
    // A long selection's text isn't sent (the device only follows one on the caret's line): the 40 after it then
    // start inside it, and the device sees it doesn't reach.
    CFIndex reach = MIN(sel.length, 200) + 40;
    if (total >= 0) reach = MIN(reach, MAX(0, total - sel.location));
    NSString *after = rangeText(e, sel.location, reach);
    if (!after && total < 0 && sel.length > 0 && sel.length <= 200) after = rangeText(e, sel.location, sel.length);
    if (!after) after = @"";
    NSString *(^b64)(NSString *) = ^NSString *(NSString *t) {
        return t.length ? [[t dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0] : @"-";
    };
    // Each fits the device's 1000 characters: text far from ASCII takes up to four bytes a character.
    while (before.length && b64(before).length > 1000) before = [before substringFromIndex:MIN(before.length, 20)];
    while (after.length && b64(after).length > 1000) after = [after substringToIndex:after.length - MIN(after.length, 20)];
    return [NSString stringWithFormat:@"%ld %@ %@", (long)sel.length, b64(before), b64(after)];
}

// Apps built on Chrome (Chrome, Edge, Brave, Arc, and Electron's Slack, VS Code, Discord...) carry its page renderer
// as "NAME Helper (Renderer).app" among their frameworks, whatever they are called.
static BOOL builtOnChrome(NSRunningApplication *app) {
    NSURL *frameworks = [app.bundleURL URLByAppendingPathComponent:@"Contents/Frameworks" isDirectory:YES];
    if (!frameworks) return NO;
    NSDirectoryEnumerator<NSURL *> *walk = [NSFileManager.defaultManager enumeratorAtURL:frameworks
                                                              includingPropertiesForKeys:@[ NSURLIsDirectoryKey, NSURLIsSymbolicLinkKey ]
                                                                                 options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:nil];
    for (NSURL *u in walk) {
        NSString *name = u.lastPathComponent;
        if ([name hasSuffix:@" Helper (Renderer).app"]) return YES;
        // Skipping is only asked of a real folder: of a link (a framework's Resources) it skipped the rest of its folder.
        NSNumber *folder = nil, *link = nil;
        [u getResourceValue:&folder forKey:NSURLIsDirectoryKey error:NULL];
        [u getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:NULL];
        if (!folder.boolValue || link.boolValue) continue;
        if (walk.level >= 5 || [name hasSuffix:@".app"] || [name isEqualToString:@"Resources"] || [name hasSuffix:@".lproj"] ||
            [name isEqualToString:@"Libraries"]) [walk skipDescendants];
    }
    return NO;
}

// What a field takes, for the device's keyboard: "code" (a terminal or a code editor: no autocorrect, no capitals, as
// iOS would "fix" commands), "address" (a browser's address bar: no capitals), "plain" (an email address, a user name,
// a web address: no autocorrect, no capitals), or "prose".
static const char *fieldKind(AXUIElementRef e, NSString *bundle) {
    static NSSet<NSString *> *coders;
    if (!coders) coders = [NSSet setWithObjects:@"com.apple.Terminal", @"com.googlecode.iterm2", @"dev.warp.Warp-Stable",
                           @"com.mitchellh.ghostty", @"net.kovidgoyal.kitty", @"org.alacritty", @"io.alacritty", @"com.github.wez.wezterm",
                           @"co.zeit.hyper", @"com.apple.dt.Xcode", @"com.microsoft.VSCode", @"com.microsoft.VSCodeInsiders",
                           @"com.vscodium", @"com.todesktop.230313mzl4w4u92", @"dev.zed.Zed", @"com.sublimetext.4", @"com.panic.Nova",
                           @"com.barebones.bbedit", @"com.apple.ScriptEditor2", nil];
    if ([coders containsObject:bundle] || [bundle hasPrefix:@"com.jetbrains."]) return "code";
    NSMutableString *said = [NSMutableString string];
    for (NSString *attribute in @[ (__bridge NSString *)kAXDescriptionAttribute, (__bridge NSString *)kAXTitleAttribute,
                                   (__bridge NSString *)kAXPlaceholderValueAttribute, (__bridge NSString *)kAXIdentifierAttribute,
                                   @"AXDOMIdentifier" ]) {
        CFTypeRef v = NULL;
        if (AXUIElementCopyAttributeValue(e, (__bridge CFStringRef)attribute, &v) == kAXErrorSuccess && v) {
            if (CFGetTypeID(v) == CFStringGetTypeID()) [said appendFormat:@" %@", (__bridge NSString *)v];
            CFRelease(v);
        }
    }
    NSString *lower = said.lowercaseString;
    for (NSString *w in @[ @"address and search", @"search or enter", @"enter address", @"smart search", @"address_and_search",
                           @"omnibox", @"address bar", @"location bar", @"urlbar" ])
        if ([lower containsString:w]) return "address";
    for (NSString *w in @[ @"email", @"e-mail", @"username", @"user name", @"user id", @"login", @"url", @"website",
                           @"web address", @"apple account" ])
        if ([lower containsString:w]) return "plain";
    return "prose";
}

// A launcher's panel (Spotlight, Raycast, Alfred) takes typing without its app coming to the front, and the Mac can't
// say which app has the keyboard ("cannot complete"). Such a panel sits over the ordinary and floating windows, and its
// app has a text field focused; the menu bar, the Dock and the like have nothing focused. (macOS 27's Spotlight is
// the Siri process's, a window at layer 23.) The window list needs no permission and asks no app, so it is read on each
// look and twice a second (safetyTick); the apps behind those windows are asked when the list changes, and again
// every 0.4 s for a while after while none has a field (a panel may focus its field a moment after it shows, and an
// emulated Mac's launcher can take longer than a question's timeout to answer the first time). Returns the windows'
// signature (the app in front, then each window's number); pids gets their apps.
static NSString *overlaysSeen;  // the signature at the last scan
static uint64_t overlaysSeenAt; // when it last changed (CLOCK_UPTIME_RAW ns)
static uint64_t overlaysAskedAt; // when their apps were last asked
static unsigned overlaysCount;  // how many windows it has
static pid_t panelSeen;         // what that scan found: 0 none

static NSString *overlayWindows(pid_t front, NSMutableArray<NSNumber *> *pids) {
    NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                                    kCGNullWindowID));
    CGRect screen = CGDisplayBounds(CGMainDisplayID());
    NSMutableString *signature = [NSMutableString stringWithFormat:@"%d:", (int)front];
    unsigned count = 0;
    for (NSDictionary *w in windows) { // front to back
        int layer = [w[(__bridge id)kCGWindowLayer] intValue];
        if (layer <= 3) break; // ordinary and floating windows: the app in front has the keyboard
        pid_t pid = [w[(__bridge id)kCGWindowOwnerPID] intValue];
        CGRect bounds = CGRectZero;
        if (layer >= 1000 || pid <= 0 || pid == front || [w[(__bridge id)kCGWindowAlpha] doubleValue] <= 0 ||
            !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(__bridge id)kCGWindowBounds], &bounds) ||
            bounds.size.width < 150 || bounds.size.height < 30) continue;
        // The Dock's window covers the whole screen, and the menu bar is the window server's: neither takes typing.
        if (CGRectContainsRect(bounds, screen) || [w[(__bridge id)kCGWindowOwnerName] isEqual:@"Window Server"]) continue;
        [signature appendFormat:@" %@", w[(__bridge id)kCGWindowNumber]];
        count++;
        if (pids && ![pids containsObject:@(pid)]) [pids addObject:@(pid)];
    }
    if (pids) overlaysCount = count;
    return signature;
}

static const uint64_t panelSettles = 5000000000ull; // ns after the windows change that their apps are asked again
static const uint64_t panelAskEvery = 400000000ull;

static pid_t panelWithField(pid_t front) {
    NSMutableArray<NSNumber *> *pids = [NSMutableArray array];
    NSString *signature = overlayWindows(front, pids);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (![signature isEqualToString:overlaysSeen]) overlaysSeen = signature, overlaysSeenAt = now, panelSeen = 0;
    else if (panelSeen || !pids.count || now - overlaysSeenAt > panelSettles || now - overlaysAskedAt < panelAskEvery)
        return panelSeen; // nothing new since they were asked
    overlaysAskedAt = now;
    static NSMutableDictionary<NSNumber *, NSDate *> *slow; // apps that didn't answer: left alone for a moment
    if (!slow) slow = [NSMutableDictionary dictionary];
    panelSeen = 0;
    for (NSNumber *p in pids) {
        NSDate *when = slow[p];
        if (when && -when.timeIntervalSinceNow < 2) continue;
        AXUIElementRef app = AXUIElementCreateApplication(p.intValue);
        if (!app) continue;
        AXUIElementSetMessagingTimeout(app, 0.3f);
        CFTypeRef focused = NULL;
        AXError error = AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &focused);
        CFRelease(app);
        if (error == kAXErrorCannotComplete) slow[p] = [NSDate date];
        else [slow removeObjectForKey:p];
        if (!focused) continue;
        CFTypeRef role = NULL;
        BOOL text = CFGetTypeID(focused) == AXUIElementGetTypeID() &&
                    AXUIElementCopyAttributeValue(focused, kAXRoleAttribute, &role) == kAXErrorSuccess && role &&
                    CFGetTypeID(role) == CFStringGetTypeID() &&
                    [@[ @"AXTextField", @"AXTextArea", @"AXComboBox", @"AXSearchField" ] containsObject:(__bridge NSString *)role];
        if (role) CFRelease(role);
        CFRelease(focused);
        if (text) {
            panelSeen = p.intValue;
            break;
        }
    }
    return panelSeen;
}

// MARK: Whether something takes typing

enum { EDIT_UNKNOWN = -1, EDIT_NO = 0, EDIT_YES = 1 };

// Accessibility couldn't answer: it timed out or "cannot complete" (the app is busy), the element went away (a page
// re-rendering), or the app doesn't do Accessibility. Anything else is an answer, a "no" included.
static BOOL unsure(AXError error) {
    return error == kAXErrorCannotComplete || error == kAXErrorInvalidUIElement || error == kAXErrorNotImplemented ||
           error == kAXErrorAPIDisabled;
}

static NSString *axSaid(AXError error) {
    switch (error) {
    case kAXErrorCannotComplete: return @"timed out";
    case kAXErrorInvalidUIElement: return @"went away";
    case kAXErrorNotImplemented: return @"isn't answered by the app";
    case kAXErrorAPIDisabled: return @"needs Accessibility";
    case kAXErrorNoValue: return @"has none";
    default: return [NSString stringWithFormat:@"failed (%d)", (int)error];
    }
}

static BOOL frameOf(AXUIElementRef e, CGRect *frame); // below

// A frame in the global points Accessibility uses, as fractions of the screen; CGRectNull for none.
static CGRect screenFraction(CGRect f) {
    CGRect screen = CGDisplayBounds(CGMainDisplayID());
    if (!(f.size.width > 0) || !(f.size.height > 0) || !(screen.size.width > 0) || !(screen.size.height > 0)) return CGRectNull;
    return CGRectMake((f.origin.x - screen.origin.x) / screen.size.width, (f.origin.y - screen.origin.y) / screen.size.height,
                      f.size.width / screen.size.width, f.size.height / screen.size.height);
}

// Whether an element takes typing: EDIT_YES (*secure for a password field); EDIT_NO only once Accessibility has said
// enough to be sure; else EDIT_UNKNOWN, *why saying what didn't answer. *role is its role (and subrole), for the log.
// Fields, search fields, combo boxes and text views (a document, a note, Terminal); whatever its role, text whose value
// can be changed and that has a caret (a web page's search box may be a group); and editable web content that isn't a
// field (a page's rich text editor, a mail being written), where browsers point to where its editing starts.
static int editability(AXUIElementRef e, BOOL *secure, NSString **role, NSString **why) {
    static NSSet<NSString *> *typed;
    if (!typed) typed = [NSSet setWithObjects:(__bridge NSString *)kAXTextFieldRole, (__bridge NSString *)kAXTextAreaRole,
                                              (__bridge NSString *)kAXComboBoxRole, @"AXSearchField", nil];
    *secure = NO;
    *role = nil;
    *why = nil;
    CFTypeRef v = NULL;
    AXError error = AXUIElementCopyAttributeValue(e, kAXRoleAttribute, &v);
    if (error != kAXErrorSuccess || !v || CFGetTypeID(v) != CFStringGetTypeID()) {
        if (v) CFRelease(v);
        *why = [NSString stringWithFormat:@"its role %@", axSaid(error == kAXErrorSuccess ? kAXErrorNoValue : error)];
        return EDIT_UNKNOWN;
    }
    NSString *r = CFBridgingRelease(v);
    *role = r;
    // An app whose contents Accessibility can't see yet (Electron's, before it builds its tree) says the app itself has it.
    if ([r isEqualToString:(__bridge NSString *)kAXApplicationRole]) {
        *why = @"the app itself has the focus (its contents can't be seen yet)";
        return EDIT_UNKNOWN;
    }
    v = NULL;
    if (![r isEqualToString:@"AXWebArea"]) {
        error = AXUIElementCopyAttributeValue(e, kAXSubroleAttribute, &v);
        if (unsure(error)) {
            if (v) CFRelease(v);
            *why = [NSString stringWithFormat:@"%@'s subrole %@", r, axSaid(error)];
            return EDIT_UNKNOWN;
        }
    }
    NSString *subrole = nil;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) subrole = CFBridgingRelease(v);
    else if (v) CFRelease(v);
    if (subrole.length) *role = [NSString stringWithFormat:@"%@/%@", r, subrole];
    if ([subrole isEqualToString:(__bridge NSString *)kAXSecureTextFieldSubrole]) {
        *secure = YES;
        return EDIT_YES;
    }
    if ([typed containsObject:r]) return EDIT_YES;
    Boolean settable = false;
    error = AXUIElementIsAttributeSettable(e, kAXValueAttribute, &settable);
    if (unsure(error)) {
        *why = [NSString stringWithFormat:@"whether %@'s value can be changed %@", r, axSaid(error)];
        return EDIT_UNKNOWN;
    }
    if (error == kAXErrorSuccess && settable) {
        CFTypeRef range = NULL;
        error = AXUIElementCopyAttributeValue(e, kAXSelectedTextRangeAttribute, &range);
        BOOL caret = error == kAXErrorSuccess && range;
        if (range) CFRelease(range);
        if (caret) return EDIT_YES;
        if (unsure(error)) {
            *why = [NSString stringWithFormat:@"%@'s caret %@", r, axSaid(error)];
            return EDIT_UNKNOWN;
        }
    }
    if ([r isEqualToString:@"AXWebArea"]) {
        // An AXWebArea is the web document container; only check editable ancestor if fast.
        // If it times out or returns error, it's not a text field (EDIT_NO).
        CFTypeRef editable = NULL;
        error = AXUIElementCopyAttributeValue(e, CFSTR("AXEditableAncestor"), &editable);
        BOOL yes = error == kAXErrorSuccess && editable;
        if (editable) CFRelease(editable);
        if (yes) return EDIT_YES;
        return EDIT_NO;
    }
    if ([r isEqualToString:(__bridge NSString *)kAXGroupRole] ||
        [r isEqualToString:(__bridge NSString *)kAXStaticTextRole]) {
        CFTypeRef editable = NULL;
        error = AXUIElementCopyAttributeValue(e, CFSTR("AXEditableAncestor"), &editable);
        BOOL yes = error == kAXErrorSuccess && editable;
        if (editable) CFRelease(editable);
        if (yes) return EDIT_YES;
        if (unsure(error)) {
            *why = [NSString stringWithFormat:@"whether %@ is editable %@", r, axSaid(error)];
            return EDIT_UNKNOWN;
        }
    }
    return EDIT_NO;
}

// MARK: Watching the focus

static dispatch_queue_t focusQueue;   // everything below runs on it
static dispatch_source_t focusTimer;  // kept: a timer nothing holds is freed at once and never fires
static dispatch_source_t contextTimer;
static _Atomic(BOOL) lookPending;     // a look is queued: notifications that come meanwhile are covered by it
// The focused element at the last look (retained; NULL: nothing focused in fieldApp), and what is kept of it: asked of
// Accessibility once for each element, not on every look.
static AXUIElementRef fieldElement;
static pid_t fieldApp;
static unsigned fieldID, fieldSerial;
static int fieldEdit = EDIT_UNKNOWN;
static BOOL fieldSecure;
static const char *fieldKindSaid = "prose";
static CGRect fieldFrame;             // fractions of the screen; CGRectNull when not known
static NSString *fieldRole;
static BOOL secureSeen;               // macOS's secure input at the last look
static pid_t secureHolderSeen;         // the process holding it then (-1: macOS didn't say)
static NSString *secureHolder;         // its name, for Henge's log
static BOOL lockedSeen;                // the screen was locked at the last look, something holding secure input

// The process holding macOS's secure input: 0 while it is off, -1 if macOS doesn't say (the console session's
// kCGSSessionSecureInputPID, in the I/O Registry's IOConsoleUsers).
static pid_t secureInputHolder(void) {
    if (!IsSecureEventInputEnabled()) return 0;
    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    CFTypeRef users = root ? IORegistryEntryCreateCFProperty(root, CFSTR("IOConsoleUsers"), NULL, 0) : NULL;
    if (root) IOObjectRelease(root);
    // The session on the screen's: the keys go to it (-1 if it doesn't say). Another session's only while none says
    // it is on the screen.
    pid_t onScreen = -1, other = -1;
    BOOL screenSeen = NO;
    if (users && CFGetTypeID(users) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount(users); i++) {
            CFDictionaryRef user = CFArrayGetValueAtIndex(users, i);
            if (CFGetTypeID(user) != CFDictionaryGetTypeID()) continue;
            BOOL console = CFDictionaryGetValue(user, CFSTR("kCGSSessionOnConsoleKey")) == kCFBooleanTrue;
            CFNumberRef pid = CFDictionaryGetValue(user, CFSTR("kCGSSessionSecureInputPID"));
            int value = 0;
            BOOL said = pid && CFGetTypeID(pid) == CFNumberGetTypeID() && CFNumberGetValue(pid, kCFNumberIntType, &value) && value > 0;
            if (console && !screenSeen) {
                screenSeen = YES;
                if (said) onScreen = value;
            } else if (!console && said && other < 0) {
                other = value;
            }
        }
    }
    if (users) CFRelease(users);
    return screenSeen ? onScreen : other;
}

// Whether the screen is locked (the console session's CGSSessionScreenIsLocked).
static BOOL screenLockedNow(void) {
    BOOL locked = NO;
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (session) {
        CFTypeRef value = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
        locked = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
        CFRelease(session);
    }
    return locked;
}

static NSString *processName(pid_t pid) {
    char name[2 * MAXCOMLEN + 1] = {0};
    return pid > 0 && proc_name(pid, name, sizeof name) > 0 ? @(name) : nil;
}
static pid_t observedPid;             // the app whose notifications are heard (the front one, or a launcher's)
static NSString *sentField, *sentContext, *saidWhat;
static NSString *stateSent;
static int saidLast = FOCUS_SAID_UNKNOWN; // FOCUS_SAID_* at the last look
static NSMutableDictionary<NSString *, NSNumber *> *unsureCounts; // looks that couldn't tell, for each app
static uint64_t slowNoted;
static void observeApp(pid_t pid);    // on the main thread, below
static void lookAtFocus(void);

static void requestLook(void) {
    if (!focusQueue || atomic_exchange(&lookPending, YES)) return;
    dispatch_async(focusQueue, ^{ lookAtFocus(); });
}

// The element that has the focus now, or nothing. Another one than at the last look gets a new ID.
static void setField(AXUIElementRef element, pid_t pid) {
    BOOL same = element ? fieldElement && CFEqual(fieldElement, element) : !fieldElement && fieldApp == pid;
    if (same) return;
    if (fieldElement) CFRelease(fieldElement);
    fieldElement = element ? (AXUIElementRef)CFRetain(element) : NULL;
    fieldApp = pid;
    fieldID++;
    fieldEdit = element ? EDIT_UNKNOWN : EDIT_NO;
    fieldSecure = NO;
    fieldKindSaid = "prose";
    fieldFrame = CGRectNull;
    fieldRole = element ? nil : @"nothing";
}

// One look: FOCUS_SAID_* (focus-said.h). *why says why it is unknown, or what has the focus when it isn't a text field,
// for Henge's log; *appName which app was asked.
static int lookNow(NSString **why, NSString **appName) {
    *why = nil;
    *appName = nil;
    // Which app holds macOS's secure input, if any: it counts only for that app, or for macOS's own password prompts
    // (focus-said.h).
    pid_t holder = secureInputHolder();
    secureSeen = holder != 0;
    secureHolderSeen = holder;
    NSString *holderName = processName(holder);
    secureHolder = holderName ?: (holder < 0 ? @"an app macOS doesn't say" : nil);
    static BOOL holderUnsaid;
    if (holder < 0 && !holderUnsaid) {
        holderUnsaid = YES;
        note(@"secure input is on and macOS doesn't say which app holds it: taken as held by the app asked only in SecurityAgent, or loginwindow while the screen is locked");
    }
    // loginwindow counts as a password prompt only while the screen is locked: it may keep secure input a moment after,
    // and shows the Log Out and Shut Down confirmations. Looked up only while some process holds secure input.
    BOOL screenLocked = holder != 0 && screenLockedNow();
    lockedSeen = screenLocked;
    FocusHolder held = focus_holder(holder, holderName.UTF8String, screenLocked, 0, NULL);
    if (!trusted()) {
        *why = @"no Accessibility permission";
        return focus_said(FOCUS_CANT_TELL, NO, held);
    }
    // Asked of the app in front: the system-wide object often can't answer ("cannot complete"). Its timeout would also
    // have been every Accessibility call's in this helper, the touch answers' too.
    // macOS's own password prompt holding secure input has the keys, over whatever app is in front: it is asked.
    NSRunningApplication *front = focus_ask_prompt(holder, holderName.UTF8String, screenLocked)
                                      ? [NSRunningApplication runningApplicationWithProcessIdentifier:holder] : nil;
    if (!front) {
        front = NSWorkspace.sharedWorkspace.frontmostApplication;
        pid_t panel = panelWithField(front.processIdentifier);
        if (panel) front = [NSRunningApplication runningApplicationWithProcessIdentifier:panel] ?: front;
    }
    pid_t pid = front.processIdentifier;
    NSString *bundle = front.bundleIdentifier ?: @"";
    *appName = front.localizedName ?: bundle;
    // The process asked, not the app in front behind a launcher's panel or the prompt: the keys go to it.
    held = focus_holder(holder, holderName.UTF8String, screenLocked, pid, bundle.UTF8String);
    if (pid <= 0) {
        *why = @"no app is in front";
        return focus_said(FOCUS_CANT_TELL, NO, held);
    }
    if (pid != observedPid) {
        observedPid = pid;
        dispatch_async(dispatch_get_main_queue(), ^{ observeApp(pid); });
    }
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) {
        *why = @"its app can't be asked";
        return focus_said(FOCUS_CANT_TELL, NO, held);
    }
    AXUIElementSetMessagingTimeout(app, 0.25f);
    // Apps built on Chrome show their pages' fields to Accessibility only once asked, as a screen reader does:
    // Electron's (Slack, VS Code, Discord...) take AXManualAccessibility, and browsers (Edge, Chrome, Brave, Arc...)
    // AXEnhancedUserInterface (YouTube's search box got no keyboard). Asked once for each app, and the second only of
    // apps built on Chrome, as others can slow their windows for it. Chrome takes it even as it answers with an error.
    static NSMutableSet<NSNumber *> *asked;
    if (!asked) asked = [NSMutableSet set];
    if (![asked containsObject:@(pid)]) {
        [asked addObject:@(pid)];
        AXError manual = AXUIElementSetAttributeValue(app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
        if (manual != kAXErrorSuccess && builtOnChrome(front)) {
            AXError enhanced = AXUIElementSetAttributeValue(app, CFSTR("AXEnhancedUserInterface"), kCFBooleanTrue);
            note([NSString stringWithFormat:@"asked %@, built on Chrome, to show its pages' fields (it answered %d)", bundle, (int)enhanced]);
        }
    }
    CFTypeRef focused = NULL;
    AXError error = AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &focused);
    CFRelease(app);
    if (focused && CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        CFRelease(focused);
        focused = NULL;
        error = kAXErrorFailure;
    }
    if (error == kAXErrorNoValue || error == kAXErrorAttributeUnsupported || (error == kAXErrorSuccess && !focused)) {
        setField(NULL, pid); // the app says nothing of it has the focus
        *why = @"nothing";
        return focus_said(FOCUS_NOTHING, NO, held);
    }
    if (error != kAXErrorSuccess) {
        // Another app in front that can't answer yet (one just opening): whatever has the focus, it isn't the field
        // last looked at, so it gets another ID and the device knows the focus left that field.
        if (pid != fieldApp) setField(NULL, pid);
        *why = [NSString stringWithFormat:@"its focused element %@", axSaid(error)];
        return focus_said(FOCUS_CANT_TELL, NO, held);
    }
    setField((AXUIElementRef)focused, pid);
    CFRelease(focused);
    if (fieldEdit == EDIT_UNKNOWN) {
        AXUIElementSetMessagingTimeout(fieldElement, 0.25f);
        BOOL secure = NO;
        NSString *role = nil, *whyNot = nil;
        int edit = editability(fieldElement, &secure, &role, &whyNot);
        fieldRole = role;
        if (edit == EDIT_UNKNOWN) {
            *why = whyNot;
            return focus_said(FOCUS_CANT_TELL, NO, held);
        }
        fieldEdit = edit;
        fieldSecure = secure;
        if (edit == EDIT_YES) {
            fieldKindSaid = secure ? "plain" : fieldKind(fieldElement, bundle);
            CGRect f;
            fieldFrame = frameOf(fieldElement, &f) ? screenFraction(f) : CGRectNull;
        }
    }
    if (fieldEdit == EDIT_NO) {
        *why = fieldRole ?: @"no role";
        return focus_said(FOCUS_NOT_FIELD, NO, held);
    }
    return focus_said(FOCUS_FIELD, fieldSecure, held);
}

static BOOL linkUp(void) {
    pthread_mutex_lock(&linkLock);
    BOOL linked = linkFd >= 0;
    pthread_mutex_unlock(&linkLock);
    return linked;
}

// Whether the focus has moved off the element last looked at without the app saying so: one question.
static BOOL focusMovedQuietly(void) {
    if (fieldApp <= 0) return NO;
    AXUIElementRef app = AXUIElementCreateApplication(fieldApp);
    if (!app) return NO;
    AXUIElementSetMessagingTimeout(app, 0.25f);
    CFTypeRef focused = NULL;
    AXError error = AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &focused);
    CFRelease(app);
    BOOL moved = NO;
    if (error == kAXErrorSuccess && focused) moved = !fieldElement || !CFEqual(fieldElement, focused);
    else if (error == kAXErrorNoValue) moved = fieldElement != NULL;
    if (focused) CFRelease(focused);
    return moved;
}

// The text around the caret of the field with the focus ("ctx ID ..."), when it changed. check: first make sure the
// focus is still there (the caret moved: some apps move the focus without a word).
static void readContext(BOOL check) {
    @autoreleasepool {
        NSString *line;
        if (fieldElement && fieldEdit == EDIT_YES && !fieldSecure && !IsSecureEventInputEnabled()) {
            if (check && focusMovedQuietly()) return requestLook();
            NSString *context = caretContext(fieldElement);
            line = [NSString stringWithFormat:@"ctx %u %@\n", fieldID, context ?: @"none"];
        } else {
            line = [NSString stringWithFormat:@"ctx %u none\n", fieldID];
        }
        if ([line isEqualToString:sentContext] || !linkUp()) return;
        sentContext = line;
        linkSend(line);
    }
}

// Looked at when the app says its focus moved, when another app comes to the front, and by safetyTick. Each change
// is one "field" line; for a text field the text around its caret follows.
static void lookAtFocus(void) {
    @autoreleasepool {
        atomic_store(&lookPending, NO);
        uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        NSString *why = nil, *app = nil;
        int said = lookNow(&why, &app);
        const char *state = focus_said_word(said);
        uint64_t took = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000;
        if (took > 100 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - slowNoted > 10 * NSEC_PER_SEC) {
            slowNoted = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            note([NSString stringWithFormat:@"a look at the focus took %llu ms (%@)", took, app ?: @"?"]);
        }
        saidLast = said;
        BOOL typing = !strcmp(state, "text") || !strcmp(state, "secure");
        NSString *body = [NSString stringWithFormat:@"%u %s %s", fieldID, state, typing ? fieldKindSaid : "prose"];
        if (typing && !CGRectIsNull(fieldFrame)) {
            CGRect f = fieldFrame;
            body = [body stringByAppendingFormat:@" %.4f %.4f %.4f %.4f", f.origin.x, f.origin.y, f.size.width, f.size.height];
        }
        if (!linkUp()) return; // told once linked
        NSString *now = @(state);
        if (![body isEqualToString:sentField]) {
            // Why, for Henge's log: what took the focus when it isn't a text field, and why the helper can't tell.
            if ([now isEqualToString:@"secure"] && ![stateSent isEqualToString:@"secure"] && (why || !fieldSecure)) {
                // Not the field saying it is a password field: taken from who holds secure input (focus-said.h).
                note([NSString stringWithFormat:@"secure input is held by %@ and %@ shows %@: taken as a password prompt%@",
                                                secureHolder ?: @"?", app ?: @"?", why ? [NSString stringWithFormat:@"no field (%@)", why] : @"a field that doesn't say",
                                                said == FOCUS_SAID_HELD ? @" (no Return after the password)" : @""]);
            } else if ([now isEqualToString:@"unknown"] && ![stateSent isEqualToString:@"unknown"]) {
                if (!unsureCounts) unsureCounts = [NSMutableDictionary dictionary];
                NSString *key = app ?: @"?";
                int count = unsureCounts[key].intValue + 1;
                unsureCounts[key] = @(count);
                note([NSString stringWithFormat:@"can't tell what has the focus in %@: %@ (%d time%s so far)", key, why ?: @"?", count,
                                                count == 1 ? "" : "s"]);
            } else if ([now isEqualToString:@"none"] && ![why isEqualToString:saidWhat]) {
                note([NSString stringWithFormat:@"focus on %@ in %@, not a text field", why ?: @"?", app ?: @"?"]);
            }
            if (![now isEqualToString:@"unknown"]) saidWhat = [now isEqualToString:@"none"] ? why : nil;
            stateSent = now;
            sentField = body;
            linkSend([NSString stringWithFormat:@"field %u %@\n", ++fieldSerial, body]);
        }
        if (![now isEqualToString:@"unknown"]) readContext(NO);
    }
}

// "look N" (helper 34), as the password's Return is due: a look now, its field line first if anything changed, then
// "looked N" with what has the focus ("held": a password field only as its app holds secure input, no Return).
static void answerLook(unsigned id) {
    if (!focusQueue) return linkSend([NSString stringWithFormat:@"looked %u unknown\n", id]);
    dispatch_async(focusQueue, ^{
        lookAtFocus();
        linkSend([NSString stringWithFormat:@"looked %u %s\n", id, focus_look_word(saidLast)]);
    });
}

// Twice a second, and cheap: the safety net for what no notification says. Secure input turning on or off, or passing
// to another app; a launcher's panel coming or going (the window list, no app asked); and once a second, whether the
// focused element is still the one last looked at (one question), or another try at a look that couldn't tell.
static void safetyTick(void) {
    @autoreleasepool {
        static unsigned ticks;
        ticks++;
        // Another app taking it over counts too (a password manager's left on, then Terminal's sudo).
        pid_t holder = secureInputHolder();
        if ((holder != 0) != secureSeen || holder != secureHolderSeen) return lookAtFocus();
        // The screen unlocking (or locking) while something holds it: loginwindow keeping it is a prompt no longer (or
        // again), as is loginwindow asked while macOS doesn't say who holds it.
        if ((holder < 0 || [secureHolder isEqualToString:@"loginwindow"]) && screenLockedNow() != lockedSeen) return lookAtFocus();
        if (!trusted()) return;
        static pid_t lastFrontPid;
        pid_t frontPid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        BOOL appChanged = (frontPid != lastFrontPid);
        lastFrontPid = frontPid;
        BOOL settling = (!panelSeen && overlaysCount && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - overlaysSeenAt < panelSettles);
        if (appChanged || settling || (ticks % 4 == 0)) {
            NSString *signature = overlayWindows(frontPid, nil);
            // The windows changed, or a panel showing may yet focus its field (or answer at last).
            if (![signature isEqualToString:overlaysSeen] || settling) return lookAtFocus();
        }
        if (ticks % 2) return;
        if ([stateSent isEqualToString:@"unknown"]) {
            // When focus could not be determined (e.g. browser is busy compiling or loading a page),
            // back off to every ~5s instead of hammering it every second with 250ms timeouts.
            static unsigned unknownTicks;
            if (++unknownTicks < 5) return;
            unknownTicks = 0;
            lookAtFocus();
            return;
        }
        if (focusMovedQuietly()) lookAtFocus();
    }
}

// Told by the app with the keyboard the moment its focus moves, so the device's keyboard needn't wait. The caret
// moving (every key typed) only reads the text around it again, once typing has paused for 150 ms.
static AXObserverRef focusObserver;

static void focusMoved(AXObserverRef observer, AXUIElementRef element, CFStringRef notification, void *context) {
    (void)observer; (void)element; (void)context;
    if (CFEqual(notification, kAXSelectedTextChangedNotification)) {
        if (contextTimer)
            dispatch_source_set_timer(contextTimer, dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), DISPATCH_TIME_FOREVER,
                                      20 * NSEC_PER_MSEC);
    } else {
        requestLook();
    }
}

static void observeApp(pid_t pid) { // on the main thread
    if (focusObserver) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(focusObserver), kCFRunLoopDefaultMode);
        CFRelease(focusObserver);
        focusObserver = NULL;
    }
    if (pid <= 0 || !trusted() || AXObserverCreate(pid, focusMoved, &focusObserver) != kAXErrorSuccess) {
        focusObserver = NULL;
        dispatch_async(focusQueue, ^{ if (observedPid == pid) observedPid = 0; }); // tried again at the next look
        return;
    }
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    AXObserverAddNotification(focusObserver, app, kAXFocusedUIElementChangedNotification, NULL);
    AXObserverAddNotification(focusObserver, app, kAXFocusedWindowChangedNotification, NULL);
    AXObserverAddNotification(focusObserver, app, kAXSelectedTextChangedNotification, NULL); // the caret moved: its context
    CFRelease(app);
    CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(focusObserver), kCFRunLoopDefaultMode);
}

static void watchFocus(void) {
    focusQueue = dispatch_queue_create("henge.focus", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    fieldFrame = CGRectNull;
    contextTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, focusQueue);
    dispatch_source_set_timer(contextTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    dispatch_source_set_event_handler(contextTimer, ^{ readContext(YES); });
    dispatch_resume(contextTimer);
    dispatch_source_t timer = focusTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, focusQueue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), NSEC_PER_SEC / 2, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(timer, ^{ safetyTick(); });
    dispatch_resume(timer);
    requestLook();
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil
                                                                     queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
            (void)n;
            requestLook(); // the look watches the new app
        }];
        [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification object:nil
                                                                     queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
            (void)n;
            requestLook(); // active space/desktop changed
        }];
    });
}

// A new link starts knowing nothing: the next look tells it where things stand (the same field keeps its ID).
static void focusRelinked(void) {
    if (focusQueue) dispatch_async(focusQueue, ^{
        sentField = nil;
        sentContext = nil;
        saidWhat = nil;
        stateSent = nil;
        lookAtFocus();
    });
}

// The screen in the global points that events and Accessibility use. Henge's Mac has the one display.
static CGPoint onScreen(double x, double y) {
    CGRect b = CGDisplayBounds(CGMainDisplayID());
    return CGPointMake(b.origin.x + x * b.size.width, b.origin.y + y * b.size.height);
}

static CGSize screenSize(void) { return CGDisplayBounds(CGMainDisplayID()).size; }

// MARK: - What is under a finger

static dispatch_queue_t hitQueue, tapQueue;

static NSString *roleOf(AXUIElementRef e) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, kAXRoleAttribute, &v) != kAXErrorSuccess || !v) return @"";
    NSString *role = CFGetTypeID(v) == CFStringGetTypeID() ? (__bridge NSString *)v : @"";
    NSString *copy = [role copy];
    CFRelease(v);
    return copy;
}

static AXUIElementRef copyElement(AXUIElementRef e, CFStringRef attribute) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, attribute, &v) != kAXErrorSuccess || !v) return NULL;
    if (CFGetTypeID(v) != AXUIElementGetTypeID()) {
        CFRelease(v);
        return NULL;
    }
    return (AXUIElementRef)v;
}

static BOOL frameOf(AXUIElementRef e, CGRect *frame) {
    CFTypeRef p = NULL, s = NULL;
    BOOL ok = AXUIElementCopyAttributeValue(e, kAXPositionAttribute, &p) == kAXErrorSuccess && p &&
              AXUIElementCopyAttributeValue(e, kAXSizeAttribute, &s) == kAXErrorSuccess && s &&
              AXValueGetValue(p, kAXValueTypeCGPoint, &frame->origin) && AXValueGetValue(s, kAXValueTypeCGSize, &frame->size);
    if (p) CFRelease(p);
    if (s) CFRelease(s);
    return ok;
}

static BOOL boolOf(AXUIElementRef e, CFStringRef attribute) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, attribute, &v) != kAXErrorSuccess || !v) return NO;
    BOOL yes = CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v);
    CFRelease(v);
    return yes;
}

static NSString *stringOf(AXUIElementRef e, CFStringRef attribute) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, attribute, &v) != kAXErrorSuccess || !v) return @"";
    NSString *s = CFGetTypeID(v) == CFStringGetTypeID() ? [(__bridge NSString *)v copy] : @"";
    CFRelease(v);
    return s;
}

// The strip across the top of a window that moves it: the title bar, and the toolbar that shares it. The window's
// buttons sit in the middle of that strip, however tall the toolbar makes it. A full-screen window has none showing:
// its top is its content (a page, a video), and macOS only slides the title bar in when the pointer goes up there.
static BOOL inTitleBar(AXUIElementRef window, CGPoint at) {
    CGRect w;
    if (!frameOf(window, &w) || at.y < w.origin.y || at.x < w.origin.x || at.x > CGRectGetMaxX(w)) return NO;
    if (boolOf(window, CFSTR("AXFullScreen"))) return NO;
    CGFloat bottom = 0;
    AXUIElementRef close = copyElement(window, kAXCloseButtonAttribute);
    CGRect b;
    if (close && frameOf(close, &b)) bottom = 2 * (CGRectGetMidY(b) - w.origin.y);
    if (close) CFRelease(close);
    AXUIElementRef toolbar = copyElement(window, CFSTR("AXToolbar"));
    if (toolbar && frameOf(toolbar, &b)) bottom = MAX(bottom, CGRectGetMaxY(b) - w.origin.y);
    if (toolbar) CFRelease(toolbar);
    return bottom > 0 && at.y - w.origin.y < MIN(bottom, 120);
}

// A drag that takes hold at once (moves a window, a slider, a split, or picks from a menu), or one that scrolls. Where it
// can't tell, scrolling: a touch and hold still drags anything. A scroll bar scrolls too, as the page beside it does on
// an iPad (a thumb at the screen's edge is usually on one): touched and held, it is grabbed like anything else.
static BOOL probing; // henge-agent --probe X Y: what it finds there, for checking by hand

enum { HIT_UNKNOWN = -1, HIT_SCROLL = 0, HIT_DRAG = 1 };

static AXUIElementRef systemWide(void) {
    static AXUIElementRef everything;
    if (!everything) {
        everything = AXUIElementCreateSystemWide();
        AXUIElementSetMessagingTimeout(everything, 0.25f); // an app that doesn't answer is scrolled
    }
    return everything;
}

// HIT_UNKNOWN when nothing there answers (an app that is stuck, or a point Accessibility can't see into).
static int dragsAt(AXUIElementRef hit, CGPoint at) {
    static NSSet *grabbed, *scrollers;
    if (!grabbed) {
        grabbed = [NSSet setWithArray:@[ @"AXSlider", @"AXSplitter", @"AXMenu", @"AXMenuItem", @"AXMenuBar", @"AXMenuBarItem",
                                         @"AXLevelIndicator", @"AXRuler", @"AXRulerMarker" ]];
        // A scroll bar's knob (AXValueIndicator) is passed over to the bar; a slider's, to the slider.
        scrollers = [NSSet setWithArray:@[ @"AXScrollArea", @"AXScrollBar", @"AXWebArea", @"AXTable", @"AXOutline", @"AXList", @"AXBrowser",
                                           @"AXTextArea", @"AXGrid", @"AXLayoutArea", @"AXCollection", @"AXSheet" ]];
    }
    // Each question is a round trip to the app, so the cheapest answers first, and stop at the first that settles it.
    AXUIElementRef window = copyElement(hit, kAXWindowAttribute);
    BOOL drag = window && inTitleBar(window, at);
    if (window) CFRelease(window);
    if (probing && drag) printf("  in the title bar\n");
    AXUIElementRef e = drag ? NULL : (AXUIElementRef)CFRetain(hit);
    for (int depth = 0; e && depth < 24; depth++) {
        NSString *role = roleOf(e);
        if (probing) printf("  %s\n", role.UTF8String);
        if ([grabbed containsObject:role]) drag = YES;
        else if ([role isEqualToString:@"AXWindow"]) drag = depth == 0; // its own background, as a drag would move it
        else if (![scrollers containsObject:role]) {
            AXUIElementRef up = copyElement(e, kAXParentAttribute);
            CFRelease(e);
            e = up;
            continue;
        }
        break; // settled: a grabbed control, the window, or something that scrolls
    }
    if (e) CFRelease(e);
    return drag ? HIT_DRAG : HIT_SCROLL;
}

// A button that can be pressed the moment a finger lands on it, so it lights up under the finger as an iPad's do, and
// clicks as the finger lifts. A press that turns out to be a swipe or a hold is let go off the button, which macOS takes
// as no click; but on a button that can itself be dragged (a browser's tab, a bookmark, a toolbar item) that would drag
// it. So only buttons that can't be: those in a dialog, an alert or a sheet, and a window's own close, minimize and
// zoom buttons, and none inside anything that scrolls or lists. Its frame in the global points events use.
static BOOL pressableAt(AXUIElementRef hit, CGRect *frame) {
    static NSSet *buttons, *windowButtons, *dialogs, *blockers;
    if (!buttons) {
        buttons = [NSSet setWithArray:@[ @"AXButton", @"AXCheckBox", @"AXRadioButton", @"AXDisclosureTriangle" ]];
        windowButtons = [NSSet setWithArray:@[ @"AXCloseButton", @"AXMinimizeButton", @"AXZoomButton", @"AXFullScreenButton" ]];
        dialogs = [NSSet setWithArray:@[ @"AXDialog", @"AXSystemDialog" ]];
        blockers = [NSSet setWithArray:@[ @"AXScrollArea", @"AXScrollBar", @"AXWebArea", @"AXTable", @"AXOutline", @"AXList", @"AXBrowser",
                                          @"AXTextArea", @"AXGrid", @"AXLayoutArea", @"AXCollection", @"AXTabGroup", @"AXToolbar",
                                          @"AXMenu", @"AXMenuBar", @"AXMenuBarItem", @"AXMenuItem", @"AXPopover", @"AXDockItem" ]];
    }
    if (![buttons containsObject:roleOf(hit)]) return NO;
    CGRect f;
    if (!frameOf(hit, &f) || f.size.width < 4 || f.size.height < 4 || f.size.width > 400 || f.size.height > 160) return NO;
    BOOL windowButton = [windowButtons containsObject:stringOf(hit, kAXSubroleAttribute)];
    BOOL ok = NO;
    AXUIElementRef e = copyElement(hit, kAXParentAttribute);
    for (int depth = 0; e && depth < 24; depth++) {
        NSString *role = roleOf(e);
        if ([blockers containsObject:role]) break;
        if ([role isEqualToString:@"AXSheet"]) {
            ok = YES;
            break;
        }
        if ([role isEqualToString:@"AXWindow"]) {
            ok = windowButton || [dialogs containsObject:stringOf(e, kAXSubroleAttribute)];
            break;
        }
        AXUIElementRef up = copyElement(e, kAXParentAttribute);
        CFRelease(e);
        e = up;
    }
    if (e) CFRelease(e);
    if (probing) printf("  %s pressed as it is touched\n", ok ? "is" : "isn't");
    if (ok) *frame = f;
    return ok;
}

// The quick answer, from the window server alone: no app is asked. The menu bar and the status items take hold at once
// (HIT_DRAG). In an ordinary window's top strip it is most likely the title bar (2), which the app is asked to settle
// when it can be; elsewhere it is up to the app (HIT_UNKNOWN). A window that fills the screen is a full-screen one, whose
// top is its content.
static int dragsFromWindows(CGPoint at) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return HIT_UNKNOWN;
    int answer = HIT_UNKNOWN;
    CGRect screen = CGDisplayBounds(CGMainDisplayID());
    for (NSDictionary *w in (__bridge NSArray *)list) { // front to back
        CGRect frame;
        NSNumber *alpha = w[(__bridge NSString *)kCGWindowAlpha];
        if (alpha && alpha.doubleValue <= 0) continue;
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(__bridge NSString *)kCGWindowBounds], &frame)) continue;
        if (!CGRectContainsPoint(frame, at)) continue;
        int layer = [w[(__bridge NSString *)kCGWindowLayer] intValue];
        // The Dock's window, and overlays like it, cover the whole screen and let touches through where they draw nothing.
        if (layer != kCGNormalWindowLevel && CGRectContainsRect(frame, screen)) continue;
        if (layer == kCGMainMenuWindowLevel || layer == kCGStatusWindowLevel) answer = HIT_DRAG;
        else if (layer == kCGNormalWindowLevel && at.y - frame.origin.y < 28 && frame.size.height > 60 && !CGRectContainsRect(frame, screen)) answer = 2;
        break; // the window on top settles it, or leaves it to the app
    }
    CFRelease(list);
    return answer;
}

// The text field or text area a scroll area holds (retained), or NULL: one of its first few children.
static AXUIElementRef textInside(AXUIElementRef scroll, BOOL *secure) {
    CFTypeRef children = NULL;
    if (AXUIElementCopyAttributeValue(scroll, kAXChildrenAttribute, &children) != kAXErrorSuccess || !children) return NULL;
    AXUIElementRef found = NULL;
    if (CFGetTypeID(children) == CFArrayGetTypeID()) {
        CFArrayRef list = children;
        for (CFIndex i = 0; i < CFArrayGetCount(list) && i < 4 && !found; i++) {
            AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(list, i);
            if (CFGetTypeID(child) != AXUIElementGetTypeID()) continue;
            AXUIElementSetMessagingTimeout(child, 0.15f);
            CFTypeRef role = NULL;
            BOOL text = AXUIElementCopyAttributeValue(child, kAXRoleAttribute, &role) == kAXErrorSuccess && role &&
                        CFGetTypeID(role) == CFStringGetTypeID() &&
                        [@[ @"AXTextField", @"AXTextArea", @"AXSearchField" ] containsObject:(__bridge NSString *)role];
            if (role) CFRelease(role);
            if (!text) continue;
            NSString *r = nil, *why = nil;
            if (editability(child, secure, &r, &why) == EDIT_YES) found = (AXUIElementRef)CFRetain(child);
        }
    }
    CFRelease(children);
    return found;
}

// What a tap at a point lands in, so the device can show its keyboard as the finger lifts rather than wait for the
// app to move its focus: "tap ID editable|secure|no|unknown ROLE [X Y W H]" (version 28). The element under the
// finger and up to three of its parents (a web page's text is inside the field that takes it), answered within
// tapPatience or "unknown"; X Y W H is the field, as fractions of the screen, ROLE the element's ("-" when not known).
// On an emulated Mac an app busy with the tap itself (Safari loading a page) often takes 200-600 ms to say what is
// at a point: 150 ms (version 28) gave up on most of them, and the keyboard then waited for the focus to move,
// seconds later. It runs on its own queue (tapQueue), so a slow answer holds up no hit's drag-or-scroll.
static const double tapPatience = 0.9; // seconds; the device waits a little longer (KeyboardOwner.tapWait)
static NSString *tapAnswer(unsigned id, CGPoint at, AXUIElementRef under) {
    uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), deadline = began + (uint64_t)(tapPatience * NSEC_PER_SEC);
    // Each call may take what is left of the time, at most half a second (an app that has hung doesn't take it all).
    float (^left)(void) = ^float(void) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        double s = now >= deadline ? 0.02 : (double)(deadline - now) / NSEC_PER_SEC;
        return (float)fmin(fmax(s, 0.02), 0.5);
    };
    const char *answer = "unknown";
    NSString *firstRole = nil;
    CGRect field = CGRectNull;
    AXUIElementRef e = under ? (AXUIElementRef)CFRetain(under) : NULL;
    if (!e && trusted()) {
        static AXUIElementRef everything;
        if (!everything) everything = AXUIElementCreateSystemWide();
        // The system-wide element's timeout is every element's that has none of its own (the hit answers' too):
        // set back as soon as this one call is done.
        AXUIElementSetMessagingTimeout(everything, left());
        if (AXUIElementCopyElementAtPosition(everything, (float)at.x, (float)at.y, &e) != kAXErrorSuccess) e = NULL;
        AXUIElementSetMessagingTimeout(everything, 0.25f);
    }
    for (int depth = 0; e && depth < 4 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline; depth++) {
        AXUIElementSetMessagingTimeout(e, left());
        BOOL secure = NO;
        NSString *role = nil, *why = nil;
        int edit = editability(e, &secure, &role, &why);
        if (depth == 0) firstRole = role;
        if (edit == EDIT_YES) {
            answer = secure ? "secure" : "editable";
            CGRect f;
            if (frameOf(e, &f)) field = screenFraction(f);
            break;
        }
        if (edit == EDIT_UNKNOWN) {
            if (depth == 0) break; // what is under the finger couldn't be told: unknown
            answer = "no";         // a parent couldn't be told: the element itself said no
            break;
        }
        answer = "no";
        // A text view's scroll area reaches past its text (Spotlight's field is a line of text in a taller scroll
        // area): a tap in it is a tap in the text it holds.
        if ([role hasPrefix:@"AXScrollArea"]) {
            AXUIElementRef inside = textInside(e, &secure);
            if (inside) {
                answer = secure ? "secure" : "editable";
                CGRect f;
                if (frameOf(inside, &f)) field = screenFraction(f);
                CFRelease(inside);
                break;
            }
        }
        // Windows, the menu bar and the like have nothing above them that takes typing.
        if ([role hasPrefix:@"AXWindow"] || [role hasPrefix:@"AXMenu"] || [role hasPrefix:@"AXApplication"]) break;
        AXUIElementRef parent = copyElement(e, kAXParentAttribute);
        CFRelease(e);
        e = parent;
    }
    if (e) CFRelease(e);
    if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline && !strcmp(answer, "unknown"))
        firstRole = firstRole ?: @"timed-out";
    NSString *line = [NSString stringWithFormat:@"tap %u %s %@", id, answer,
                                                [firstRole stringByReplacingOccurrencesOfString:@" " withString:@"_"] ?: @"-"];
    if (!CGRectIsNull(field))
        line = [line stringByAppendingFormat:@" %.4f %.4f %.4f %.4f", field.origin.x, field.origin.y, field.size.width, field.size.height];
    uint64_t took = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000;
    if (took > 300) note([NSString stringWithFormat:@"telling what a tap landed in took %llu ms (%s)", took, answer]);
    return [line stringByAppendingString:@"\n"];
}

// "tap ID X Y": a tap on the touchscreen, or a click (it lands as macOS's own, so no "hit" was asked).
static void answerTap(unsigned id, double x, double y) {
    dispatch_async(tapQueue, ^{
        @autoreleasepool {
            linkSend(tapAnswer(id, onScreen(x, y), NULL));
        }
    });
}

// "hit ID drag|scroll", and " press X Y W H" (fractions of the screen) for a button pressed as it is touched; then
// what the touch lands in (tapAnswer) with the same ID, for when it turns out to be a tap.
static void answerHit(unsigned id, double x, double y) {
    dispatch_async(hitQueue, ^{
        @autoreleasepool {
            CGPoint at = onScreen(x, y);
            int quick = dragsFromWindows(at), answer = quick == HIT_DRAG ? HIT_DRAG : HIT_UNKNOWN;
            CGRect press = CGRectNull;
            AXUIElementRef hit = NULL;
            if (trusted() && AXUIElementCopyElementAtPosition(systemWide(), (float)at.x, (float)at.y, &hit) == kAXErrorSuccess && hit) {
                if (answer == HIT_UNKNOWN) answer = dragsAt(hit, at);
                CGRect f;
                if (pressableAt(hit, &f)) press = f;
            } else {
                hit = NULL;
            }
            if (answer == HIT_UNKNOWN) answer = quick == 2 ? HIT_DRAG : HIT_SCROLL; // the app didn't say: the window server's guess
            NSString *line = [NSString stringWithFormat:@"hit %u %s", id, answer == HIT_DRAG ? "drag" : "scroll"];
            if (!CGRectIsNull(press)) {
                CGRect s = CGDisplayBounds(CGMainDisplayID());
                line = [line stringByAppendingFormat:@" press %.5f %.5f %.5f %.5f", (press.origin.x - s.origin.x) / s.size.width,
                                                     (press.origin.y - s.origin.y) / s.size.height, press.size.width / s.size.width,
                                                     press.size.height / s.size.height];
            }
            linkSend([line stringByAppendingString:@"\n"]);
            // What it lands in, on the taps' own queue: that may take a while, and the next hit mustn't wait for it.
            dispatch_async(tapQueue, ^{
                @autoreleasepool {
                    linkSend(tapAnswer(id, at, hit));
                    if (hit) CFRelease(hit);
                }
            });
        }
    });
}

// MARK: - Menus on the screen

// How many pop-up menus are showing: a context menu, the Dock's menus, a pop-up button's. Each is its own window at the
// pop-up menu level, until it has faded right out. The window server's list, so no permission is needed.
static dispatch_queue_t menuQueue;

static int menusShowing(void) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return 0;
    int n = 0;
    for (NSDictionary *w in (__bridge NSArray *)list) {
        NSNumber *alpha = w[(__bridge NSString *)kCGWindowAlpha];
        if ([w[(__bridge NSString *)kCGWindowLayer] intValue] == kCGPopUpMenuWindowLevel && !(alpha && alpha.doubleValue <= 0)) n++;
    }
    CFRelease(list);
    return n;
}

static void answerMenu(unsigned id) {
    dispatch_async(menuQueue, ^{ linkSend([NSString stringWithFormat:@"menu %u %d\n", id, menusShowing()]); });
}

// Closes the pop-up menus that are showing the way VoiceOver does, with each menu's own Cancel action (AXCancel):
// the menu goes as if clicked away from, and nothing else on the Mac sees a key (an Escape that missed the menu
// would cancel a dialog, leave full screen or undo a rename). Each menu window's app is asked for its open menus:
// a context menu is the app's own child, the Dock's hangs from its item; windows (big, and with no context menus
// in them) and the menu bar (its menus are there, closed or not) are left alone. How many it closed: 0 when it
// can't (no Accessibility permission, or an app that doesn't answer), and Henge falls back to its guarded Escape.
static int cancelMenusUnder(AXUIElementRef e, int depth, CFAbsoluteTime until) {
    if (CFAbsoluteTimeGetCurrent() > until) return 0; // taking too long: what's left stays (Henge waits for the count)
    CFArrayRef kids = NULL;
    if (AXUIElementCopyAttributeValue(e, kAXChildrenAttribute, (CFTypeRef *)&kids) != kAXErrorSuccess || !kids) return 0;
    int n = 0;
    if (CFGetTypeID(kids) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount(kids) && i < 200; i++) {
            AXUIElementRef kid = (AXUIElementRef)CFArrayGetValueAtIndex(kids, i);
            if (CFGetTypeID(kid) != AXUIElementGetTypeID()) continue;
            NSString *role = roleOf(kid);
            if ([role isEqualToString:@"AXMenu"]) {
                if (AXUIElementPerformAction(kid, kAXCancelAction) == kAXErrorSuccess) n++;
            } else if (depth < 3 && ![role isEqualToString:@"AXWindow"] && ![role isEqualToString:@"AXMenuBar"]) {
                n += cancelMenusUnder(kid, depth + 1, until);
            }
        }
    }
    CFRelease(kids);
    return n;
}

static int closeMenus(void) {
    if (!trusted()) return 0;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return 0;
    NSMutableOrderedSet<NSNumber *> *owners = [NSMutableOrderedSet orderedSet];
    for (NSDictionary *w in (__bridge NSArray *)list) {
        NSNumber *alpha = w[(__bridge NSString *)kCGWindowAlpha];
        NSNumber *pid = w[(__bridge NSString *)kCGWindowOwnerPID];
        if ([w[(__bridge NSString *)kCGWindowLayer] intValue] == kCGPopUpMenuWindowLevel && !(alpha && alpha.doubleValue <= 0) && pid)
            [owners addObject:pid];
    }
    CFRelease(list);
    // Every element's calls wait 0.25 s at most (a timeout set on the system-wide element is everyone's; one set
    // on the app would cover the app alone, not the elements found under it), and the whole walk half a second.
    AXUIElementRef everyone = AXUIElementCreateSystemWide();
    if (everyone) AXUIElementSetMessagingTimeout(everyone, 0.25f), CFRelease(everyone);
    CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + 0.5;
    int n = 0;
    for (NSNumber *pid in owners) {
        AXUIElementRef app = AXUIElementCreateApplication(pid.intValue);
        if (!app) continue;
        AXUIElementSetMessagingTimeout(app, 0.25f);
        n += cancelMenusUnder(app, 0, until);
        CFRelease(app);
    }
    return n;
}

static void answerMenuClose(unsigned id) {
    dispatch_async(menuQueue, ^{ linkSend([NSString stringWithFormat:@"menu %u %d\n", id, closeMenus()]); });
}

// MARK: - Scrolling, as a trackpad

static dispatch_queue_t scrollQueue; // everything below runs on it
static CGPoint scrollAt;
static double restX, restY;                // pixels not yet sent
static dispatch_source_t glide;
static double glideX, glideY;              // points a second
static uint64_t glideLast;
static BOOL glideStarted;

static void postScroll(double dx, double dy, CGScrollPhase phase, CGMomentumScrollPhase momentum) {
    restX += dx, restY += dy;
    int32_t ix = (int32_t)restX, iy = (int32_t)restY;
    restX -= ix, restY -= iy;
    CGEventRef e = CGEventCreateScrollWheelEvent2(NULL, kCGScrollEventUnitPixel, 2, iy, ix, 0);
    if (!e) return;
    CGEventSetIntegerValueField(e, kCGScrollWheelEventIsContinuous, 1);
    CGEventSetIntegerValueField(e, kCGScrollWheelEventScrollPhase, phase);
    CGEventSetIntegerValueField(e, kCGScrollWheelEventMomentumPhase, momentum);
    CGEventSetLocation(e, scrollAt);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

// A finger that lands on a page gliding along only stops it, as on an iPad: Henge doesn't click there. Henge hears
// "glide 1" while the glide is quick enough to be seen moving, and "glide 0" once it isn't, or has stopped. Below that
// speed a tap goes through, so a page that has all but stopped never swallows one.
static const double catchSpeed = 250; // points a second
static BOOL glideCatches;
static _Atomic unsigned glideNumber;      // which glide the watcher below is watching

static void sayCatches(BOOL catches) {
    if (catches == glideCatches) return;
    glideCatches = catches;
    linkSend(catches ? @"glide 1\n" : @"glide 0\n");
}

static void stopGlide(void) {
    sayCatches(NO);
    if (!glide) return;
    atomic_fetch_add(&glideNumber, 1);
    dispatch_source_cancel(glide);
    glide = nil;
    if (glideStarted) postScroll(0, 0, 0, kCGMomentumScrollPhaseEnd);
}

// Slows as an iPad's pages do (0.995 a millisecond), until it is too slow to see.
static void glideStep(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    double dt = MIN((now - glideLast) / 1e9, 0.05);
    glideLast = now;
    double slow = pow(0.995, dt * 1000);
    glideX *= slow, glideY *= slow;
    double speed = hypot(glideX, glideY);
    if (speed < 15) {
        stopGlide();
        return;
    }
    if (speed < catchSpeed) sayCatches(NO);
    postScroll(glideX * dt, glideY * dt, 0, glideStarted ? kCGMomentumScrollPhaseContinue : kCGMomentumScrollPhaseBegin);
    glideStarted = YES;
}

// A page that glides into its end stops there, but the glide would run on for seconds, and a tap meanwhile would only
// stop it. So the glide watches the scroll bars of what it scrolls (Accessibility), and ends when the page has moved
// and then sits at one end. A page whose scroll bars never change (an app that doesn't keep them up to date) is left to
// glide out: it is only stopped on what was seen to move.
static dispatch_queue_t glideWatchQueue;

static BOOL scrollValue(AXUIElementRef bar, double *value) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(bar, kAXValueAttribute, &v) != kAXErrorSuccess || !v) return NO;
    BOOL ok = CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, value);
    CFRelease(v);
    return ok;
}

static void watchGlide(unsigned number, CGPoint at, BOOL across, BOOL down) {
    if (!trusted()) return;
    dispatch_async(glideWatchQueue, ^{
        AXUIElementRef hit = NULL, area = NULL;
        if (AXUIElementCopyElementAtPosition(systemWide(), (float)at.x, (float)at.y, &hit) != kAXErrorSuccess || !hit) return;
        AXUIElementRef e = hit;
        for (int depth = 0; e && depth < 24; depth++) {
            if ([roleOf(e) isEqualToString:@"AXScrollArea"]) {
                area = e;
                break;
            }
            AXUIElementRef up = copyElement(e, kAXParentAttribute);
            if (e != hit) CFRelease(e);
            e = up;
        }
        if (e && e != area && e != hit) CFRelease(e);
        AXUIElementRef bars[2] = {down && area ? copyElement(area, kAXVerticalScrollBarAttribute) : NULL,
                                  across && area ? copyElement(area, kAXHorizontalScrollBarAttribute) : NULL};
        if (area && area != hit) CFRelease(area);
        CFRelease(hit);
        double last[2] = {NAN, NAN};
        BOOL moved = NO;
        int still = 0;
        for (int i = 0; i < 100 && (bars[0] || bars[1]); i++) { // checked every 50 ms, for as long as a glide can last
            usleep(50000);
            if (atomic_load(&glideNumber) != number) break;
            BOOL changed = NO, atEnd = YES, read = YES;
            for (int k = 0; k < 2; k++) {
                if (!bars[k]) continue;
                double v;
                if (!scrollValue(bars[k], &v)) {
                    read = NO;
                    break;
                }
                if (last[k] == last[k] && fabs(v - last[k]) > 1e-5) changed = moved = YES;
                if (v > 0.0005 && v < 0.9995) atEnd = NO;
                last[k] = v;
            }
            if (!read) break;
            still = changed ? 0 : still + 1;
            if (moved && atEnd && still >= 2) { // at its end for a tenth of a second
                dispatch_async(scrollQueue, ^{
                    if (glide && glideNumber == number) stopGlide();
                });
                break;
            }
        }
        for (int k = 0; k < 2; k++)
            if (bars[k]) CFRelease(bars[k]);
    });
}

static void scrollLine(char phase, double x, double y, double dx, double dy) {
    CGSize size = screenSize();
    dx *= size.width, dy *= size.height;
    switch (phase) {
    case 'b':
        stopGlide();
        scrollAt = onScreen(x, y);
        restX = restY = 0;
        postScroll(dx, dy, kCGScrollPhaseBegan, kCGMomentumScrollPhaseNone);
        break;
    case 'c':
        postScroll(dx, dy, kCGScrollPhaseChanged, kCGMomentumScrollPhaseNone);
        break;
    case 'e':
        postScroll(0, 0, kCGScrollPhaseEnded, kCGMomentumScrollPhaseNone);
        if (hypot(dx, dy) < 150) break;
        glideX = MAX(MIN(dx, 6000), -6000), glideY = MAX(MIN(dy, 6000), -6000);
        glideLast = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        glideStarted = NO;
        unsigned currentGlide = atomic_fetch_add(&glideNumber, 1) + 1;
        glide = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, scrollQueue);
        dispatch_source_set_timer(glide, DISPATCH_TIME_NOW, NSEC_PER_SEC / 120, NSEC_PER_SEC / 1000);
        dispatch_source_set_event_handler(glide, ^{ glideStep(); });
        dispatch_resume(glide);
        sayCatches(hypot(glideX, glideY) >= catchSpeed);
        watchGlide(currentGlide, scrollAt, glideX != 0, glideY != 0);
        break;
    }
}

// MARK: - Updating

static NSData *gunzip(NSData *gz) {
    z_stream z = {0};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return nil;
    NSMutableData *out = [NSMutableData dataWithLength:gz.length * 4];
    z.next_in = (Bytef *)gz.bytes;
    z.avail_in = (uInt)gz.length;
    int rc;
    do {
        if (z.total_out >= out.length) out.length *= 2;
        z.next_out = (Bytef *)out.mutableBytes + z.total_out;
        z.avail_out = (uInt)(out.length - z.total_out);
        rc = inflate(&z, Z_NO_FLUSH);
    } while (rc == Z_OK);
    out.length = z.total_out;
    inflateEnd(&z);
    return rc == Z_STREAM_END ? out : nil;
}

// The helper this Henge carries, in place of this one; launchd starts it as this exits. It keeps the same
// path and signer, so macOS keeps its permission.
static void update(void) {
    int status = 0;
    NSData *gz = request(@"GET", @"/.henge/agent", nil, &status, NULL);
    NSData *binary = status == 200 && gz.length ? gunzip(gz) : nil;
    if (binary.length < 4096) return;
    char path[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, real)) return;
    NSString *fresh = [@(real) stringByAppendingString:@".new"];
    if (![binary writeToFile:fresh atomically:NO]) return;
    chmod(fresh.fileSystemRepresentation, 0755);
    if (rename(fresh.fileSystemRepresentation, real) != 0) return;
    NSLog(@"henge-agent: updated; starting again");
    exit(0);
}

// MARK: - The link, kept open

// Henge is about to pause the Mac (and iOS may close Henge while it is paused): everything macOS holds
// in memory for its disks goes to them now, and Henge hears when it is there. sync() alone only starts the
// writes; each writable local volume is then synced and waited for.
static void syncDisks(unsigned id) {
    static dispatch_queue_t syncQueue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ syncQueue = dispatch_queue_create("henge.sync", DISPATCH_QUEUE_SERIAL); });
    dispatch_async(syncQueue, ^{
        sync();
        struct statfs *mounts = NULL;
        int n = getmntinfo(&mounts, MNT_NOWAIT), synced = 0, failed = 0;
        for (int i = 0; i < n; i++) {
            const struct statfs *f = &mounts[i];
            if (!(f->f_flags & MNT_LOCAL) || (f->f_flags & MNT_RDONLY) || !strcmp(f->f_fstypename, "devfs")) continue;
            if (sync_volume_np(f->f_mntonname, SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT) == 0) synced++;
            else failed++;
        }
        linkSend([NSString stringWithFormat:@"synced %u %d %d\n", id, synced, failed]);
    });
}

// MARK: - Shut Down and Restart, asked from the device

// "power shutdown <id>" or "power restart <id>": macOS shuts down or restarts as choosing Shut Down or Restart in
// the Apple menu does, without that menu's own "Are you sure?" (Henge asked already): loginwindow is sent the same
// Apple event (kAEShutDown or kAERestart, Technical Q&A QA1134). Every app is asked to quit, so each can save, and an
// app with unsaved changes asks about them on the Mac's screen; a Cancel there (or an app that won't quit) stops it.
// The device hears "power <id> sent" once loginwindow has the request, "power <id> asking" while macOS asks the
// person whether Henge Tools may do this (only the first time), "power <id> failed <why>" if it couldn't be asked
// (the device then presses the Mac's power button instead), and "power <id> cancelled" if an app stopped it.
// Nothing at all when it works: macOS turns the Mac off (or restarts it), which the device sees for itself.
static _Atomic unsigned powerAsked;     // the device's id for the shutdown or restart under way
static _Atomic uint64_t powerAskedUntil; // when a "logout cancelled" stops counting as this one's (uptime, ns)

static void watchPowerCancel(void) { // main thread
    static id observer;
    if (observer) return;
    // loginwindow posts this when an app stops a log out, restart or shut down (it answered a quit with a Cancel).
    observer = [NSDistributedNotificationCenter.defaultCenter
        addObserverForName:@"com.apple.logoutCancelled" object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) {
                    (void)n;
                    unsigned id = atomic_exchange(&powerAsked, 0);
                    if (!id || clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > atomic_load(&powerAskedUntil)) return;
                    note([NSString stringWithFormat:@"an app stopped the shutdown or restart asked from the device (%u)", id]);
                    linkSend([NSString stringWithFormat:@"power %u cancelled\n", id]);
                }];
}

static void askPower(BOOL restart, unsigned id) { // a queue of its own: loginwindow may take its time answering
    const char *what = restart ? "restart" : "shut down";
    AEEventID kind = restart ? kAERestart : kAEShutDown;
    AEAddressDesc target = {typeNull, NULL};
    static const char loginwindow[] = "com.apple.loginwindow";
    OSStatus err = AECreateDesc(typeApplicationBundleID, loginwindow, sizeof loginwindow - 1, &target);
    // Sending it needs the person's OK once (Privacy & Security > Automation): the first time, macOS asks on the Mac's
    // screen, and the device is told ("power <id> asking") so it waits for that rather than pressing the power button.
    // Turned down (now or before): it failed, and the device presses the power button, which needs no permission.
    if (err == noErr) err = AEDeterminePermissionToAutomateTarget(&target, kCoreEventClass, kind, false);
    if (err == errAEEventWouldRequireUserConsent) {
        note(@"asking on the Mac's screen to be allowed to shut it down and restart it");
        linkSend([NSString stringWithFormat:@"power %u asking\n", id]);
        err = AEDeterminePermissionToAutomateTarget(&target, kCoreEventClass, kind, true);
    }
    AppleEvent event = {typeNull, NULL}, reply = {typeNull, NULL};
    if (err == noErr) err = AECreateAppleEvent(kCoreEventClass, kind, &target, kAutoGenerateReturnID, kAnyTransactionID, &event);
    if (err == noErr) {
        atomic_store(&powerAskedUntil, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 15 * 60 * NSEC_PER_SEC); // a save dialog may stay up
        atomic_store(&powerAsked, id);
        dispatch_async(dispatch_get_main_queue(), ^{ watchPowerCancel(); });
        // loginwindow answers once it has taken the request in hand; no answer in time still means it has it.
        err = AESendMessage(&event, &reply, kAEWaitReply | kAENeverInteract, 20 * 60);
        if (err == errAETimeout) err = noErr;
    }
    AEDisposeDesc(&reply);
    AEDisposeDesc(&event);
    AEDisposeDesc(&target);
    if (err == noErr) {
        note([NSString stringWithFormat:@"asked macOS to %s, as asked from the device", what]);
        linkSend([NSString stringWithFormat:@"power %u sent\n", id]);
    } else {
        unsigned mine = id;
        atomic_compare_exchange_strong(&powerAsked, &mine, 0);
        note([NSString stringWithFormat:@"couldn't ask macOS to %s (error %d)", what, (int)err]);
        linkSend([NSString stringWithFormat:@"power %u failed %d\n", id, (int)err]);
    }
}

// MARK: - The benchmark (helper 36)

// The same work on every build, so Henge can measure it the same way (host/bench.h). Asked "bench STEPS", the helper
// runs it in a process of its own (henge-agent --bench STEPS: an app with windows, apart from the touch handling
// here) and passes on what that says: "bench begin NAME TARGET_MS" as a step starts, "bench end NAME ok|skip|fail
// [WHY] [KEY=NUMBER ...]" as it ends, "bench note TEXT", and "bench done" at the end. Henge reads its own counters
// at each begin and end. Nothing here needs the network or the person: the video comes from Henge.
extern char **environ;
static const char *benchSteps[] = {"idle", "video", "scroll", "apps", "cpu", "rosetta"};
static BOOL benchTest; // HENGE_AGENT_TEST_BENCH: tests: short steps, little work, nothing put on this Mac's screen
static int benchWorkMB = 48; // what each CPU step process compresses

static void benchSay(NSString *line) {
    printf("bench %s\n", line.UTF8String);
    fflush(stdout);
}

static double benchNow(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
static void benchWait(double seconds) { usleep((useconds_t)((benchTest ? MIN(seconds, 0.2) : seconds) * 1e6)); }
static void onMain(dispatch_block_t block) { dispatch_sync(dispatch_get_main_queue(), block); }

// Until the Mac has settled after starting (2 minutes up, and the load average under 1.5), at most 5 minutes.
static void benchSettle(void) {
    if (benchTest) return;
    double t0 = benchNow(), load = 0;
    for (;;) {
        getloadavg(&load, 1);
        if ((NSProcessInfo.processInfo.systemUptime >= 120 && load < 1.5) || benchNow() - t0 >= 300) break;
        sleep(5);
    }
    benchSay([NSString stringWithFormat:@"note settled in %.0f s: up %.0f s, load %.2f", benchNow() - t0, NSProcessInfo.processInfo.systemUptime, load]);
}

static NSWindow *benchWindow(NSString *title, NSSize size) {
    NSRect screen = NSScreen.mainScreen.visibleFrame;
    double scale = MIN(1, MIN(screen.size.width * 0.9 / size.width, (screen.size.height - 40) * 0.9 / size.height));
    size = NSMakeSize(round(size.width * scale), round(size.height * scale));
    NSRect at = NSMakeRect(NSMidX(screen) - size.width / 2, NSMidY(screen) - size.height / 2, size.width, size.height);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:at styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;
    w.title = title;
    w.level = NSFloatingWindowLevel;
    [w orderFrontRegardless];
    return w;
}

static BOOL benchLoad(const char *framework) {
    char path[256];
    snprintf(path, sizeof path, "/System/Library/Frameworks/%s.framework/%s", framework, framework);
    return dlopen(path, RTLD_LAZY) != NULL; // the helper itself links neither: only this process needs them
}

// A 30 fps 1280x720 H.264 video, played on a loop in a window: what watching a video asks of the Mac.
static void benchVideo(void) {
    if (benchTest) return benchSay(@"end video skip test");
    int status = 0;
    NSData *video = request(@"GET", @"/.henge/bench-video", nil, &status, NULL);
    NSString *file = [home() stringByAppendingPathComponent:@"Library/Caches/henge-bench.mp4"];
    if (status != 200 || video.length < 1000 || ![video writeToFile:file atomically:YES]) return benchSay(@"end video fail novideo");
    if (!benchLoad("AVFoundation")) return benchSay(@"end video fail noavfoundation");
    __block NSWindow *window;
    __block AVQueuePlayer *player;
    __block AVPlayerLooper *looper;
    onMain(^{
        window = benchWindow(@"Henge benchmark: video", NSMakeSize(1280, 720));
        AVPlayerItem *item = [NSClassFromString(@"AVPlayerItem") playerItemWithURL:[NSURL fileURLWithPath:file]];
        player = [NSClassFromString(@"AVQueuePlayer") queuePlayerWithItems:@[]];
        player.muted = YES;
        looper = [NSClassFromString(@"AVPlayerLooper") playerLooperWithPlayer:player templateItem:item];
        AVPlayerLayer *layer = [NSClassFromString(@"AVPlayerLayer") playerLayerWithPlayer:player];
        layer.videoGravity = @"AVLayerVideoGravityResizeAspect"; // AVLayerVideoGravityResizeAspect's value: nothing linked
        window.contentView.wantsLayer = YES;
        layer.frame = window.contentView.bounds;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        [window.contentView.layer addSublayer:layer];
        [player play];
    });
    __block BOOL playing = NO;
    for (double t0 = benchNow(); !playing && benchNow() - t0 < 20; usleep(100000))
        onMain(^{ playing = player.timeControlStatus == AVPlayerTimeControlStatusPlaying; });
    if (playing) {
        benchWait(3); // under way
        benchSay(@"begin video 33.3");
        benchWait(60);
        onMain(^{ playing = player.timeControlStatus == AVPlayerTimeControlStatusPlaying; });
        benchSay(playing ? @"end video ok" : @"end video fail stopped");
    } else {
        benchSay(@"end video fail noplay");
    }
    onMain(^{
        [player pause];
        [looper disableLooping];
        [window close];
    });
}

// A long web page scrolling itself, frame by frame (requestAnimationFrame): what reading a page asks of the Mac.
static NSString *benchPage(void) {
    NSMutableString *page = [NSMutableString stringWithString:
        @"<!doctype html><meta charset=utf-8><style>body{font:16px -apple-system;margin:0 auto;max-width:900px}"
        @"section{margin:24px;padding:16px;border-radius:12px;box-shadow:0 4px 16px rgba(0,0,0,.25)}"
        @".b{height:120px;border-radius:8px;background:linear-gradient(90deg,#c33,#36c)}</style>"];
    for (int i = 0; i < 400; i++)
        [page appendFormat:@"<section style='background:hsl(%d,60%%,92%%)'><h2>Section %d</h2><div class=b style='opacity:%.2f'></div>"
                           @"<p>Every build of Henge scrolls this same page at the same speed, so how evenly it moves can be compared "
                           @"from one build to the next. Paragraph %d of 400.</p></section>", (i * 37) % 360, i + 1, 0.4 + (i % 7) / 10.0, i + 1];
    [page appendString:@"<script>window.benchFrames=0;let t0=null;function step(t){if(t0===null)t0=t;benchFrames++;"
                       @"const max=document.documentElement.scrollHeight-innerHeight,y=((t-t0)*0.6)%(2*max);"
                       @"scrollTo(0,y<max?y:2*max-y);requestAnimationFrame(step)}requestAnimationFrame(step)</script>"];
    return page;
}

static double benchFrames(WKWebView *web) {
    __block double frames = -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        [web evaluateJavaScript:@"benchFrames" completionHandler:^(id value, NSError *error) {
            if (!error && [value respondsToSelector:@selector(doubleValue)]) frames = [value doubleValue];
            dispatch_semaphore_signal(done);
        }];
    });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    return frames;
}

static void benchScroll(void) {
    if (benchTest) return benchSay(@"end scroll skip test");
    if (!benchLoad("WebKit")) return benchSay(@"end scroll fail nowebkit");
    __block NSWindow *window;
    __block WKWebView *web;
    onMain(^{
        window = benchWindow(@"Henge benchmark: scrolling", NSMakeSize(1100, 800));
        web = [[NSClassFromString(@"WKWebView") alloc] initWithFrame:window.contentView.bounds configuration:[NSClassFromString(@"WKWebViewConfiguration") new]];
        web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [window.contentView addSubview:web];
        [web loadHTMLString:benchPage() baseURL:nil];
    });
    double before = -1;
    for (double t0 = benchNow(); before <= 0 && benchNow() - t0 < 30; usleep(250000)) before = benchFrames(web);
    if (before > 0) {
        benchWait(2);
        before = benchFrames(web);
        double t0 = benchNow();
        benchSay(@"begin scroll 16.7");
        benchWait(30);
        double after = benchFrames(web), took = benchNow() - t0;
        benchSay(after >= before ? [NSString stringWithFormat:@"end scroll ok raf_fps=%.1f", (after - before) / took] : @"end scroll fail noframes");
    } else {
        benchSay(@"end scroll fail noload");
    }
    onMain(^{
        [web removeFromSuperview];
        [window close];
    });
}

// Whether the app has a window on the screen yet.
static BOOL benchHasWindow(pid_t pid) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    BOOL found = NO;
    for (NSDictionary *w in (__bridge NSArray *)list)
        if ([w[(__bridge NSString *)kCGWindowOwnerPID] intValue] == pid && [w[(__bridge NSString *)kCGWindowLayer] intValue] == 0) found = YES;
    if (list) CFRelease(list);
    return found;
}

static double benchMedian(NSMutableArray<NSNumber *> *v) {
    [v sortUsingSelector:@selector(compare:)];
    NSUInteger n = v.count;
    return n % 2 ? v[n / 2].doubleValue : (v[n / 2 - 1].doubleValue + v[n / 2].doubleValue) / 2;
}

// Opening apps, twice each, from the moment they are asked for to their first window, then quitting them: what
// starting work asks of the Mac. Apps the person has open are left alone.
static void benchApps(void) {
    if (benchTest) return benchSay(@"end apps skip test");
    NSString *text = [home() stringByAppendingPathComponent:@"Library/Caches/henge-bench.txt"];
    [@"Henge benchmark\n" writeToFile:text atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    NSArray<NSString *> *apps = @[@"/System/Applications/Calculator.app", @"/System/Applications/TextEdit.app", @"/System/Applications/Dictionary.app"];
    NSMutableArray<NSNumber *> *times = [NSMutableArray new];
    int noWindow = 0;
    benchSay(@"begin apps 0");
    for (int round = 0; round < 2; round++)
        for (NSString *path in apps) {
            NSBundle *bundle = [NSBundle bundleWithPath:path];
            if (!bundle.bundleIdentifier) continue;
            if ([NSRunningApplication runningApplicationsWithBundleIdentifier:bundle.bundleIdentifier].count) {
                if (!round) benchSay([NSString stringWithFormat:@"note %@ was open: left out", path.lastPathComponent]);
                continue;
            }
            NSWorkspaceOpenConfiguration *how = [NSWorkspaceOpenConfiguration configuration];
            how.arguments = @[@"-ApplePersistenceIgnoreState", @"YES"]; // the same first window every time
            __block NSRunningApplication *app;
            dispatch_semaphore_t opened = dispatch_semaphore_create(0);
            void (^done)(NSRunningApplication *, NSError *) = ^(NSRunningApplication *a, NSError *e) {
                (void)e;
                app = a;
                dispatch_semaphore_signal(opened);
            };
            double t0 = benchNow();
            if ([path hasSuffix:@"TextEdit.app"]) // a document of its own, not the open panel
                [NSWorkspace.sharedWorkspace openURLs:@[[NSURL fileURLWithPath:text]] withApplicationAtURL:[NSURL fileURLWithPath:path] configuration:how completionHandler:done];
            else
                [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:path] configuration:how completionHandler:done];
            dispatch_semaphore_wait(opened, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
            if (!app) {
                benchSay([NSString stringWithFormat:@"note %@ didn't open", path.lastPathComponent]);
                continue;
            }
            BOOL shown = NO;
            while (!(shown = app.finishedLaunching && benchHasWindow(app.processIdentifier)) && benchNow() - t0 < 30) usleep(20000);
            if (shown) [times addObject:@((benchNow() - t0) * 1000)];
            else noWindow++;
            [app terminate];
            for (double q = benchNow(); !app.terminated && benchNow() - q < 10;) usleep(50000);
            if (!app.terminated) [app forceTerminate];
            for (double q = benchNow(); !app.terminated && benchNow() - q < 5;) usleep(50000);
            benchWait(1);
        }
    if (!times.count) return benchSay([NSString stringWithFormat:@"end apps fail none nowindow=%d", noWindow]);
    double longest = [[times valueForKeyPath:@"@max.self"] doubleValue];
    benchSay([NSString stringWithFormat:@"end apps ok launch_ms_median=%.0f launch_ms_max=%.0f launches=%lu nowindow=%d", benchMedian(times), longest,
                                        (unsigned long)times.count, noWindow]);
}

// henge-agent --bench-work MB [x86_64]: compresses MB of made-up text, the same every time. With x86_64, only as
// the Intel half of this program (under Rosetta): 3 otherwise.
static int benchWork(int mb, BOOL intel) {
#if defined(__x86_64__)
    (void)intel;
#else
    if (intel) return 3;
#endif
    static const char *words[] = {"henge", "stone", "circle", "summer", "solstice", "sunrise", "lintel", "sarsen", "bluestone",
                                  "avenue", "ditch", "bank", "barrow", "chalk", "plain", "wessex", "ancient", "builders",
                                  "rolled", "raised", "timber", "antler", "pick", "midwinter", "alignment", "monument",
                                  "trilithon", "heel", "slaughter", "altar", "station", "aubrey"};
    size_t n = (size_t)mb << 20, i = 0;
    uint8_t *text = malloc(n);
    if (!text) return 1;
    uint32_t s = 12345;
    while (i < n) {
        s = s * 1103515245u + 12345u;
        char word[24];
        int len = (s >> 28) == 0 ? snprintf(word, sizeof word, "%u", s % 100000) : snprintf(word, sizeof word, "%s", words[(s >> 16) & 31]);
        for (int k = 0; k < len && i < n; k++) text[i++] = (uint8_t)word[k];
        if (i < n) text[i++] = (s >> 8) % 13 ? ' ' : '\n';
    }
    uLong chunk = 1 << 20, room = compressBound(chunk), total = 0;
    uint8_t *out = malloc(room);
    for (size_t at = 0; out && at < n; at += chunk) {
        uLongf got = room;
        if (compress2(out, &got, text + at, MIN(chunk, n - at), 6) != Z_OK) return 1;
        total += got;
    }
    free(out);
    free(text);
    return total ? 0 : 1;
}

static NSString *benchSelf(void) {
    char path[PATH_MAX];
    uint32_t size = sizeof path;
    return _NSGetExecutablePath(path, &size) == 0 ? @(path) : nil;
}

// count processes of --bench-work at once (as Intel code if intel); the seconds they took together, -1 failed,
// -3 no Intel half.
static double benchWorkers(int count, int mb, BOOL intel) {
    NSString *me = benchSelf();
    if (!me) return -1;
    char size[16];
    snprintf(size, sizeof size, "%d", mb);
    char *args[] = {(char *)me.fileSystemRepresentation, "--bench-work", size, intel ? "x86_64" : NULL, NULL};
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT); // only what the actions below give it
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addinherit_np(&files, STDERR_FILENO);
    if (intel) {
        cpu_type_t want = CPU_TYPE_X86_64;
        size_t got = 0;
        posix_spawnattr_setbinpref_np(&attr, 1, &want, &got);
    }
    pid_t pids[64];
    int started = 0, failed = 0, noIntel = 0;
    double t0 = benchNow();
    for (; started < count && started < 64; started++)
        if (posix_spawn(&pids[started], args[0], &files, &attr, args, environ) != 0) break;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&files);
    if (started < count) failed = 1;
    for (int k = 0; k < started; k++) {
        int st = 0;
        while (waitpid(pids[k], &st, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(st) && WEXITSTATUS(st) == 3) noIntel = 1;
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) failed = 1;
    }
    return noIntel ? -3 : failed ? -1 : benchNow() - t0;
}

// Every CPU busy compressing: how fast the Mac's own code runs.
static void benchCPU(void) {
    int n = (int)NSProcessInfo.processInfo.activeProcessorCount;
    benchSay(@"begin cpu 0");
    double took = benchWorkers(n, benchWorkMB, NO);
    benchSay(took < 0 ? @"end cpu fail work" : [NSString stringWithFormat:@"end cpu ok work_s=%.2f procs=%d", took, n]);
}

// The same work as Intel code, through Rosetta, after a first small run that leaves the translation cached.
static void benchRosetta(void) {
    if (access("/Library/Apple/usr/libexec/oah/libRosettaRuntime", F_OK) != 0) return benchSay(@"end rosetta skip norosetta");
    double warm = benchWorkers(1, 1, YES);
    if (warm == -3) return benchSay(@"end rosetta skip nointel");
    if (warm < 0) return benchSay(@"end rosetta fail warmup");
    benchSay(@"begin rosetta 0");
    double took = benchWorkers(1, benchWorkMB, YES);
    benchSay(took < 0 ? @"end rosetta fail work" : [NSString stringWithFormat:@"end rosetta ok work_s=%.2f", took]);
}

static void benchRunSteps(NSString *asked) {
    NSArray<NSString *> *names = [asked isEqualToString:@"all"] ? nil : [asked componentsSeparatedByString:@","];
    if (!names) {
        NSMutableArray *every = [NSMutableArray new];
        for (size_t k = 0; k < sizeof benchSteps / sizeof *benchSteps; k++) [every addObject:@(benchSteps[k])];
        names = every;
    }
    benchSettle();
    BOOL first = YES;
    for (NSString *name in names) {
        if (!first) benchWait(3); // what the last step started dies down
        first = NO;
        if ([name isEqualToString:@"idle"]) {
            benchSay(@"begin idle 0");
            benchWait(20);
            benchSay(@"end idle ok");
        } else if ([name isEqualToString:@"video"]) benchVideo();
        else if ([name isEqualToString:@"scroll"]) benchScroll();
        else if ([name isEqualToString:@"apps"]) benchApps();
        else if ([name isEqualToString:@"cpu"]) benchCPU();
        else if ([name isEqualToString:@"rosetta"]) benchRosetta();
        else {
            NSString *clean = [[name componentsSeparatedByCharactersInSet:NSCharacterSet.alphanumericCharacterSet.invertedSet] componentsJoinedByString:@""];
            benchSay([NSString stringWithFormat:@"end %@ skip unknown", clean.length ? [clean substringToIndex:MIN(clean.length, 24)] : @"-"]);
        }
    }
    benchSay(@"done");
}

// henge-agent --bench STEPS: the steps, as an app (the video and the page need windows), on a thread of their own.
static int benchChild(const char *steps) {
    benchTest = getenv("HENGE_AGENT_TEST_BENCH") != NULL;
    if (benchTest) benchWorkMB = 1;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    NSString *asked = @(steps);
    [NSThread detachNewThreadWithBlock:^{
        benchRunSteps(asked);
        exit(0);
    }];
    [NSApp run];
    return 0;
}

// The link's side: one run at a time, passing on its lines to the link that asked for it (none to a later one: a
// run stopped as its link went must not end one a new link asked for); if it ends without saying so, Henge hears why.
static _Atomic(BOOL) benchBusy;
static _Atomic(pid_t) benchPid; // the run's process while its lines are read (it leads a process group of its own)

// The link went: Henge has taken the run as failed ("failed gone"), so it stops, with the work processes it started,
// and a new link can ask for another at once.
static void benchStop(void) {
    pid_t pid = atomic_load(&benchPid);
    if (pid > 0) kill(-pid, SIGTERM);
}

static void benchRelay(NSString *steps, uint64_t gen) {
    NSString *me = benchSelf();
    int out[2];
    if (!me || pipe(out) != 0) {
        atomic_store(&benchBusy, NO);
        linkSendOn(gen, @"bench failed spawn\n");
        return;
    }
    // Only its stdout (to here), stdin and stderr: not the link's socket, which must close when this helper goes.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0); // a group of its own, its work processes in it: benchStop ends them all
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addinherit_np(&files, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&files, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&files, STDERR_FILENO);
    char *args[] = {(char *)me.fileSystemRepresentation, "--bench", (char *)steps.UTF8String, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, args[0], &files, &attr, args, environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        atomic_store(&benchBusy, NO);
        linkSendOn(gen, @"bench failed spawn\n");
        return;
    }
    atomic_store(&benchPid, pid);
    FILE *from = fdopen(out[0], "r");
    char line[512];
    NSString *last; // "done" or "failed ...": held until the process has gone, so another run can start the moment Henge hears it
    while (from && fgets(line, sizeof line, from)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "bench ", 6)) continue;
        NSString *said = [NSString stringWithFormat:@"%s\n", line];
        if (!strcmp(line, "bench done") || !strncmp(line, "bench failed", 12)) last = said;
        else linkSendOn(gen, said);
    }
    if (from) fclose(from);
    else close(out[0]);
    atomic_store(&benchPid, 0); // not reaped yet, so the pid can't be another process's while it was set
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!last)
        last = WIFSIGNALED(st) ? [NSString stringWithFormat:@"bench failed signal-%d\n", WTERMSIG(st)]
                               : [NSString stringWithFormat:@"bench failed exit-%d\n", WEXITSTATUS(st)];
    atomic_store(&benchBusy, NO);
    linkSendOn(gen, last);
}

static void benchAsked(const char *steps) {
    if (atomic_exchange(&benchBusy, YES)) {
        linkSend(@"bench failed busy\n");
        return;
    }
    NSString *asked = @(steps);
    uint64_t gen = linkNow(); // asked on this link: what the run says goes to it alone
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ benchRelay(asked, gen); });
}

// MARK: - Lightweight macOS responsiveness

// Makes macOS lighter in the guest by disabling costly window/dock/finder animations
// and enabling reduce motion / reduce transparency.
static void applyLightweightUIFlags(void) {
    @autoreleasepool {
        // NSGlobalDomain UI speedups
        CFPreferencesSetAppValue(CFSTR("NSAutomaticWindowAnimationsEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSWindowResizeTime"), (__bridge CFPropertyListRef)@0.001, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSScrollAnimationEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("QLPanelAnimationDuration"), (__bridge CFPropertyListRef)@0, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSWindowDocumentVersionsFileProviderSpringLoadingDelay"), (__bridge CFPropertyListRef)@0, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSToolbarTitleViewRolloverDelay"), (__bridge CFPropertyListRef)@0, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSDocumentSaveNewDocumentsToCloud"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSNavPanelExpandedStateForSaveMode"), kCFBooleanTrue, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSNavPanelExpandedStateForSaveMode2"), kCFBooleanTrue, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("PMPrintingExpandedStateForPrint"), kCFBooleanTrue, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("PMPrintingExpandedStateForPrint2"), kCFBooleanTrue, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("CGDisableCursorLocationMagnification"), kCFBooleanTrue, kCFPreferencesAnyApplication);

        // Keyboard responsiveness & input latency: fast repeat rate, zero delay, disable synchronous autocorrect/substitutions
        CFPreferencesSetAppValue(CFSTR("KeyRepeat"), (__bridge CFPropertyListRef)@1, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("InitialKeyRepeat"), (__bridge CFPropertyListRef)@10, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSAutomaticSpellingCorrectionEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSAutomaticCapitalizationEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSAutomaticPeriodSubstitutionEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSAutomaticQuoteSubstitutionEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSAutomaticDashSubstitutionEnabled"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("AppleEnableSwipeNavigateWithScrolls"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("AppleMiniaturizeOnDoubleClick"), kCFBooleanFalse, kCFPreferencesAnyApplication);
        // Keep Electron/Chromium IDEs (e.g. Antigravity, VS Code) and helper workers unthrottled by App Nap
        CFPreferencesSetAppValue(CFSTR("NSAppSleepDisabled"), kCFBooleanTrue, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("NSDisableAutomaticTermination"), kCFBooleanTrue, kCFPreferencesAnyApplication);
        CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);

        // Avoid .DS_Store metadata writes over VirtIOFS shared folders and USB drives, and skip Time Machine disk prompts
        CFStringRef ds = CFSTR("com.apple.desktopservices");
        CFPreferencesSetAppValue(CFSTR("DSDontWriteNetworkStores"), kCFBooleanTrue, ds);
        CFPreferencesSetAppValue(CFSTR("DSDontWriteUSBStores"), kCFBooleanTrue, ds);
        CFPreferencesAppSynchronize(ds);
        CFStringRef tm = CFSTR("com.apple.TimeMachine");
        CFPreferencesSetAppValue(CFSTR("DoNotOfferNewDisksForBackup"), kCFBooleanTrue, tm);
        CFPreferencesAppSynchronize(tm);

        // Dock: expose animation, autohide delays, launch animations, swoosh
        CFStringRef dock = CFSTR("com.apple.dock");
        CFPreferencesSetAppValue(CFSTR("launchanim"), kCFBooleanFalse, dock);
        CFPreferencesSetAppValue(CFSTR("expose-animation-duration"), (__bridge CFPropertyListRef)@0.05, dock);
        CFPreferencesSetAppValue(CFSTR("autohide-time-modifier"), (__bridge CFPropertyListRef)@0, dock);
        CFPreferencesSetAppValue(CFSTR("autohide-delay"), (__bridge CFPropertyListRef)@0, dock);
        CFPreferencesSetAppValue(CFSTR("mineffect"), CFSTR("scale"), dock);
        CFPreferencesSetAppValue(CFSTR("workspaces-swoosh-animation-off"), kCFBooleanTrue, dock);
        CFPreferencesSetAppValue(CFSTR("workspaces-auto-swoosh"), kCFBooleanFalse, dock);
        CFPreferencesSetAppValue(CFSTR("appswitcher-all-displays"), kCFBooleanTrue, dock);
        CFPreferencesSetAppValue(CFSTR("mru-spaces"), kCFBooleanFalse, dock);
        CFPreferencesSetAppValue(CFSTR("expose-group-apps"), kCFBooleanFalse, dock);
        CFPreferencesAppSynchronize(dock);

        // Finder animations
        CFStringRef finder = CFSTR("com.apple.finder");
        CFPreferencesSetAppValue(CFSTR("DisableAllAnimations"), kCFBooleanTrue, finder);
        CFPreferencesSetAppValue(CFSTR("AnimateWindowZoom"), kCFBooleanFalse, finder);
        CFPreferencesSetAppValue(CFSTR("AnimateInfoPanes"), kCFBooleanFalse, finder);
        CFPreferencesSetAppValue(CFSTR("AnimateSnapToGrid"), kCFBooleanFalse, finder);
        CFPreferencesAppSynchronize(finder);

        // Accessibility: reduce motion and transparency
        CFStringRef ua = CFSTR("com.apple.universalaccess");
        CFPreferencesSetAppValue(CFSTR("reduceMotion"), kCFBooleanTrue, ua);
        CFPreferencesSetAppValue(CFSTR("reduceTransparency"), kCFBooleanTrue, ua);
        CFPreferencesAppSynchronize(ua);

        // Broadcast notifications so running processes immediately pick up changes
        // Control Center: show Battery in Menu Bar
        CFStringRef cc = CFSTR("com.apple.controlcenter");
        CFPreferencesSetAppValue(CFSTR("NSStatusItem Visible Battery"), kCFBooleanTrue, cc);
        CFPreferencesSetAppValue(CFSTR("Battery"), (__bridge CFPropertyListRef)@18, cc);
        CFPreferencesAppSynchronize(cc);

        CFNotificationCenterRef dist = CFNotificationCenterGetDistributedCenter();
        if (dist) {
            CFNotificationCenterPostNotification(dist, CFSTR("com.apple.dock.prefschanged"), NULL, NULL, YES);
            CFNotificationCenterPostNotification(dist, CFSTR("com.apple.keyboard.prefschanged"), NULL, NULL, YES);
            CFNotificationCenterPostNotification(dist, CFSTR("com.apple.universalaccess.reduceMotionChanged"), NULL, NULL, YES);
            CFNotificationCenterPostNotification(dist, CFSTR("com.apple.universalaccess.reduceTransparencyChanged"), NULL, NULL, YES);
            CFNotificationCenterPostNotification(dist, CFSTR("com.apple.controlcenter.prefschanged"), NULL, NULL, YES);
            CFNotificationCenterPostNotification(dist, CFSTR("AppleNoRedisplayAppearancePreferenceChanged"), NULL, NULL, YES);
        }
        note(@"applied lightweight macOS UI preferences (reduced motion/transparency, zero-delay animations, App Nap disabled)");
    }
}

// Restarts Dock once per boot/agent-version so snapshot-resumed Macs flush cached notification badge
// textures and pick up updated Dock preferences without crash-looping.
static void refreshDockCacheOnce(void) {
    char marker[64];
    snprintf(marker, sizeof marker, "/tmp/.henge-agent-dock-v%d-%u", HV_AGENT_VERSION, (unsigned)getuid());
    int fd = open(marker, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return;
    close(fd);
    char *args[] = {"/usr/bin/killall", "Dock", NULL};
    pid_t pid = 0;
    if (posix_spawn(&pid, args[0], NULL, NULL, args, environ) == 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        note(@"refreshed Dock badge cache (one-time restart for v42)");
    }
}

// Populates System Profiler's "CPU Names" cache from IODeviceTree:/ model-number so About This Mac
// displays the host device's model name instead of "Virtual Mac".
static void applyHostModelName(void) {
    @autoreleasepool {
        io_registry_entry_t dt = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/");
        if (!dt) return;
        CFTypeRef prop = IORegistryEntryCreateCFProperty(dt, CFSTR("model-number"), kCFAllocatorDefault, 0);
        IOObjectRelease(dt);
        if (!prop) return;

        NSString *modelName = nil;
        if (CFGetTypeID(prop) == CFDataGetTypeID()) {
            NSData *data = (__bridge NSData *)prop;
            const char *bytes = data.bytes;
            if (bytes && data.length) {
                size_t len = strnlen(bytes, data.length);
                if (len > 0) modelName = [[NSString alloc] initWithBytes:bytes length:len encoding:NSUTF8StringEncoding];
            }
        } else if (CFGetTypeID(prop) == CFStringGetTypeID()) {
            modelName = [(__bridge NSString *)prop copy];
        }
        CFRelease(prop);

        modelName = [modelName stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!modelName.length || [modelName isEqualToString:@"VM0001"]) return;

        NSString *last4 = @"1234";
        io_service_t pe = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
        if (pe) {
            CFTypeRef sn = IORegistryEntryCreateCFProperty(pe, CFSTR("IOPlatformSerialNumber"), kCFAllocatorDefault, 0);
            IOObjectRelease(pe);
            if (sn) {
                if (CFGetTypeID(sn) == CFStringGetTypeID()) {
                    NSString *serial = [(__bridge NSString *)sn stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                    if (serial.length >= 4) last4 = [serial substringFromIndex:serial.length - 4];
                    else if (serial.length > 0) last4 = serial;
                }
                CFRelease(sn);
            }
        }

        CFStringRef sp = CFSTR("com.apple.SystemProfiler");
        CFPropertyListRef existing = CFPreferencesCopyAppValue(CFSTR("CPU Names"), sp);
        NSMutableDictionary *cpuNames = [NSMutableDictionary new];
        if (existing) {
            if (CFGetTypeID(existing) == CFDictionaryGetTypeID()) {
                [cpuNames addEntriesFromDictionary:(__bridge NSDictionary *)existing];
            }
            CFRelease(existing);
        }

        NSMutableSet<NSString *> *locales = [NSMutableSet setWithObjects:@"en_US", @"en-US_US", nil];
        NSString *autoLoc = NSLocale.autoupdatingCurrentLocale.localeIdentifier;
        if (autoLoc.length) [locales addObject:autoLoc];
        NSString *curLoc = NSLocale.currentLocale.localeIdentifier;
        if (curLoc.length) [locales addObject:curLoc];

        for (NSString *loc in locales) {
            cpuNames[[NSString stringWithFormat:@"%@-%@", last4, loc]] = modelName;
        }
        CFPreferencesSetAppValue(CFSTR("CPU Names"), (__bridge CFDictionaryRef)cpuNames, sp);
        CFPreferencesAppSynchronize(sp);
    }
}

// MARK: - Native battery reporting (helper 39)
// On macOS 14 Sonoma and macOS 15 Sequoia, AppleVirtualPlatform.kext carries
// AppleVirtualPlatformPowerSource which attaches to virtual PCI 0x106b:0x1a07.
// On macOS 13 Ventura and earlier, that kernel driver does not exist.
// We make battery percentage and charging state show natively in macOS Ventura by:
// 1. Publishing an IOPMPowerSource via IOPSCreatePowerSource & IOPSSetPowerSourceDetails
//    into powerd/IOKit so the entire system recognises the internal battery.
// 2. Displaying a native NSStatusItem in the menu bar with dynamic SF Symbols, percentage,
//    and dropdown status menu.
// 3. Enabling Control Center battery menu bar preferences.

typedef struct OpaqueIOPSPowerSourceID *IOPSPowerSourceID;
static IOPSPowerSourceID hengePowerSourceID = NULL;
static IOReturn (*fn_IOPSCreatePowerSource)(IOPSPowerSourceID *outPS) = NULL;
static IOReturn (*fn_IOPSSetPowerSourceDetails)(IOPSPowerSourceID whichPS, CFDictionaryRef details) = NULL;

static void ensurePowerSourceSymbols(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
        if (h) {
            fn_IOPSCreatePowerSource = (IOReturn (*)(IOPSPowerSourceID *))dlsym(h, "IOPSCreatePowerSource");
            fn_IOPSSetPowerSourceDetails = (IOReturn (*)(IOPSPowerSourceID, CFDictionaryRef))dlsym(h, "IOPSSetPowerSourceDetails");
        }
    });
}

static void updatePowerSource(double pct, int ext, int chg, int fl) {
    ensurePowerSourceSymbols();
    if (fn_IOPSCreatePowerSource && !hengePowerSourceID) {
        IOReturn kr = fn_IOPSCreatePowerSource(&hengePowerSourceID);
        if (kr != kIOReturnSuccess) hengePowerSourceID = NULL;
    }
    if (hengePowerSourceID && fn_IOPSSetPowerSourceDetails) {
        int cap = (int)round(pct);
        if (cap < 0) cap = 0;
        if (cap > 100) cap = 100;
        NSDictionary *details = @{
            @"Name": @"InternalBattery-0",
            @"Type": @"InternalBattery",
            @"Current Capacity": @(cap),
            @"Max Capacity": @100,
            @"DesignCapacity": @100,
            @"Is Charging": @(chg != 0),
            @"Is Charged": @(fl != 0),
            @"Power Source State": ext ? @"AC Power" : @"Battery Power",
            @"Is Present": @YES,
            @"BatteryHealth": @"Good",
            @"ExternalConnected": @(ext != 0),
        };
        fn_IOPSSetPowerSourceDetails(hengePowerSourceID, (__bridge CFDictionaryRef)details);
    }
}

@interface HengeBatteryAction : NSObject
+ (HengeBatteryAction *)shared;
- (void)openBatterySettings:(id)sender;
@end

@implementation HengeBatteryAction
+ (HengeBatteryAction *)shared {
    static HengeBatteryAction *act;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ act = [[HengeBatteryAction alloc] init]; });
    return act;
}
- (void)openBatterySettings:(id)sender {
    (void)sender;
    NSURL *url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.Battery-Settings.extension"];
    if (![NSWorkspace.sharedWorkspace openURL:url]) {
        [NSWorkspace.sharedWorkspace openFile:@"/System/Applications/System Settings.app"];
    }
}
@end

static NSStatusItem *batteryStatusItem = nil;

static BOOL hasKernelBatteryDriver(void) {
    if (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 14) {
        io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleVirtualPlatformPowerSource"));
        if (s) {
            IOObjectRelease(s);
            return YES;
        }
    }
    return NO;
}

static void updateStatusItem(double pct, int ext, int chg, int fl) {
    if (hasKernelBatteryDriver()) {
        if (batteryStatusItem) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (batteryStatusItem) {
                    [NSStatusBar.systemStatusBar removeStatusItem:batteryStatusItem];
                    batteryStatusItem = nil;
                }
            });
        }
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        int cap = (int)round(pct);
        if (cap < 0) cap = 0;
        if (cap > 100) cap = 100;

        if (!batteryStatusItem) {
            batteryStatusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
            batteryStatusItem.visible = YES;
        }

        NSStatusBarButton *button = batteryStatusItem.button;
        if (button) {
            NSString *sym;
            if (chg) {
                if (cap >= 90) sym = @"battery.100.bolt";
                else if (cap >= 65) sym = @"battery.75.bolt";
                else if (cap >= 35) sym = @"battery.50.bolt";
                else if (cap >= 15) sym = @"battery.25.bolt";
                else sym = @"battery.0.bolt";
            } else {
                if (cap >= 90) sym = @"battery.100";
                else if (cap >= 65) sym = @"battery.75";
                else if (cap >= 35) sym = @"battery.50";
                else if (cap >= 15) sym = @"battery.25";
                else sym = @"battery.0";
            }

            NSImage *img = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:@"Battery"];
            if (img) {
                img.template = YES;
                button.image = img;
                button.imagePosition = NSImageLeft;
            }
            button.title = [NSString stringWithFormat:@"%d%%", cap];
            button.toolTip = [NSString stringWithFormat:@"Battery: %d%%%s",
                              cap,
                              ext ? (fl ? ", Full on AC Power" : chg ? ", Charging" : ", On AC Power") : ", On Battery"];
        }

        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Battery"];

        NSMenuItem *headerItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Battery: %d%%", cap] action:nil keyEquivalent:@""];
        headerItem.enabled = NO;
        [menu addItem:headerItem];

        NSString *srcStr = ext ? @"Power Source: Power Adapter" : @"Power Source: Battery";
        NSMenuItem *srcItem = [[NSMenuItem alloc] initWithTitle:srcStr action:nil keyEquivalent:@""];
        srcItem.enabled = NO;
        [menu addItem:srcItem];

        NSString *stateStr = fl ? @"Fully Charged" : chg ? [NSString stringWithFormat:@"Charging (%d%%)", cap] : (ext ? @"Not Charging" : @"On Battery");
        NSMenuItem *stateItem = [[NSMenuItem alloc] initWithTitle:stateStr action:nil keyEquivalent:@""];
        stateItem.enabled = NO;
        [menu addItem:stateItem];

        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *prefItem = [[NSMenuItem alloc] initWithTitle:@"Battery Settings..." action:@selector(openBatterySettings:) keyEquivalent:@""];
        prefItem.target = [HengeBatteryAction shared];
        [menu addItem:prefItem];

        batteryStatusItem.menu = menu;
    });
}

static void updateBattery(double pct, int ext, int chg, int fl) {
    updatePowerSource(pct, ext, chg, fl);
    updateStatusItem(pct, ext, chg, fl);
}

#ifndef NX_KEYTYPE_SOUND_UP
#define NX_KEYTYPE_SOUND_UP 0
#define NX_KEYTYPE_SOUND_DOWN 1
#define NX_KEYTYPE_MUTE 7
#endif

static dispatch_queue_t volumeQueue;
static dispatch_queue_t volumeListenQueue;
static pthread_mutex_t volumeLock = PTHREAD_MUTEX_INITIALIZER;
static double pendingVolumeTarget = -1.0;
static int pendingVolumeHud = 0;
static uint64_t pendingVolumeSeq = 0;
static BOOL volumeWorkerRunning = NO;

static double lastAppliedVolume = -1.0; // volumeQueue only
static AudioDeviceID watchedOutputDevice = kAudioObjectUnknown;
static AudioObjectPropertyListenerBlock volumeScalarBlock;
static Float32 trackedCurVolume = -1.0f;
static Float32 trackedPrevVolume = -1.0f;
static uint64_t trackedChangeTimeNs = 0;
static BOOL agentChangingVolume = NO;
static uint64_t agentVolumeIgnoreUntilNs = 0;

static void postMediaKey(int key) {
    for (int down = 1; down >= 0; down--) {
        NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                         location:NSZeroPoint
                                    modifierFlags:(down ? 0xa00 : 0xb00)
                                        timestamp:0
                                     windowNumber:0
                                          context:nil
                                          subtype:8
                                            data1:(key << 16) | ((down ? 0xa : 0xb) << 8)
                                            data2:-1];
        CGEventRef cg = [ev CGEvent];
        if (cg) CGEventPost(kCGHIDEventTap, cg);
    }
}

static AudioDeviceID defaultOutputDevice(void) {
    AudioDeviceID deviceID = kAudioObjectUnknown;
    UInt32 propSize = sizeof(deviceID);
    AudioObjectPropertyAddress devAddr = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devAddr, 0, NULL, &propSize, &deviceID) != noErr)
        return kAudioObjectUnknown;
    return deviceID;
}

static BOOL getMasterVolume(Float32 *outVol) {
    AudioDeviceID deviceID = defaultOutputDevice();
    if (deviceID == kAudioObjectUnknown) return NO;

    Float32 vol = 0.0f;
    UInt32 volSize = sizeof(vol);
    AudioObjectPropertyAddress volAddr = {
        .mSelector = kAudioHardwareServiceDeviceProperty_VirtualMasterVolume,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMaster
    };
    if (AudioHardwareServiceGetPropertyData(deviceID, &volAddr, 0, NULL, &volSize, &vol) == noErr ||
        AudioObjectGetPropertyData(deviceID, &volAddr, 0, NULL, &volSize, &vol) == noErr) {
        if (outVol) *outVol = vol;
        return YES;
    }

    AudioObjectPropertyAddress scalarAddr = {
        .mSelector = kAudioDevicePropertyVolumeScalar,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMaster
    };
    if (AudioObjectGetPropertyData(deviceID, &scalarAddr, 0, NULL, &volSize, &vol) == noErr) {
        if (outVol) *outVol = vol;
        return YES;
    }
    scalarAddr.mElement = 1;
    if (AudioObjectGetPropertyData(deviceID, &scalarAddr, 0, NULL, &volSize, &vol) == noErr) {
        if (outVol) *outVol = vol;
        return YES;
    }
    return NO;
}

static void setMasterVolume(double level) {
    Float32 vol = (Float32)level;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    AudioDeviceID deviceID = defaultOutputDevice();
    if (deviceID == kAudioObjectUnknown) return;

    if (vol > 0.001f) {
        AudioObjectPropertyAddress muteAddr = {
            .mSelector = kAudioDevicePropertyMute,
            .mScope = kAudioDevicePropertyScopeOutput,
            .mElement = kAudioObjectPropertyElementMaster
        };
        UInt32 muted = 0;
        UInt32 muteSize = sizeof(muted);
        if (AudioObjectGetPropertyData(deviceID, &muteAddr, 0, NULL, &muteSize, &muted) == noErr && muted != 0) {
            Boolean settable = false;
            if (AudioObjectIsPropertySettable(deviceID, &muteAddr, &settable) == noErr && settable) {
                muted = 0;
                AudioObjectSetPropertyData(deviceID, &muteAddr, 0, NULL, muteSize, &muted);
            }
        }
    }

    AudioObjectPropertyAddress volAddr = {
        .mSelector = kAudioHardwareServiceDeviceProperty_VirtualMasterVolume,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMaster
    };
    UInt32 volSize = sizeof(vol);

    OSStatus err = AudioHardwareServiceSetPropertyData(deviceID, &volAddr, 0, NULL, volSize, &vol);
    if (err == noErr) return;

    err = AudioObjectSetPropertyData(deviceID, &volAddr, 0, NULL, volSize, &vol);
    if (err == noErr) return;

    AudioObjectPropertyAddress scalarAddr = {
        .mSelector = kAudioDevicePropertyVolumeScalar,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMaster
    };
    err = AudioObjectSetPropertyData(deviceID, &scalarAddr, 0, NULL, volSize, &vol);
    if (err == noErr) return;

    for (UInt32 ch = 1; ch <= 2; ch++) {
        scalarAddr.mElement = ch;
        Boolean settable = false;
        if (AudioObjectIsPropertySettable(deviceID, &scalarAddr, &settable) == noErr && settable) {
            AudioObjectSetPropertyData(deviceID, &scalarAddr, 0, NULL, volSize, &vol);
        }
    }
}

static void attachVolumeDeviceListener(void) {
    AudioDeviceID next = defaultOutputDevice();
    if (watchedOutputDevice == next) return;
    if (watchedOutputDevice != kAudioObjectUnknown && volumeScalarBlock) {
        for (UInt32 elem = 0; elem <= 1; elem++) {
            AudioObjectPropertyAddress addr = {
                .mSelector = kAudioDevicePropertyVolumeScalar,
                .mScope = kAudioDevicePropertyScopeOutput,
                .mElement = elem
            };
            AudioObjectRemovePropertyListenerBlock(watchedOutputDevice, &addr, volumeListenQueue, volumeScalarBlock);
        }
    }
    watchedOutputDevice = next;
    if (next != kAudioObjectUnknown && volumeScalarBlock) {
        for (UInt32 elem = 0; elem <= 1; elem++) {
            AudioObjectPropertyAddress addr = {
                .mSelector = kAudioDevicePropertyVolumeScalar,
                .mScope = kAudioDevicePropertyScopeOutput,
                .mElement = elem
            };
            AudioObjectAddPropertyListenerBlock(next, &addr, volumeListenQueue, volumeScalarBlock);
        }
    }
    Float32 v = -1.0f;
    if (getMasterVolume(&v)) {
        pthread_mutex_lock(&volumeLock);
        trackedCurVolume = v;
        trackedPrevVolume = v;
        pthread_mutex_unlock(&volumeLock);
    }
}

static void watchVolume(void) {
    volumeListenQueue = dispatch_queue_create("henge.volume-listen", DISPATCH_QUEUE_SERIAL);
    volumeScalarBlock = ^(UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses) {
        (void)inNumberAddresses;
        (void)inAddresses;
        Float32 v = -1.0f;
        if (!getMasterVolume(&v)) return;
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        pthread_mutex_lock(&volumeLock);
        if (!agentChangingVolume && now >= agentVolumeIgnoreUntilNs && fabsf(v - trackedCurVolume) > 0.002f) {
            trackedPrevVolume = trackedCurVolume;
            trackedCurVolume = v;
            trackedChangeTimeNs = now;
        }
        pthread_mutex_unlock(&volumeLock);
    };
    attachVolumeDeviceListener();
    AudioObjectPropertyAddress devAddr = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster
    };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &devAddr, volumeListenQueue,
                                        ^(UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses) {
        (void)inNumberAddresses;
        (void)inAddresses;
        attachVolumeDeviceListener();
    });
}

static void processPendingVolume(void) {
    for (;;) {
        pthread_mutex_lock(&volumeLock);
        double target = pendingVolumeTarget;
        int hud = pendingVolumeHud;
        uint64_t seq = pendingVolumeSeq;
        pendingVolumeHud = 0;
        pthread_mutex_unlock(&volumeLock);

        if (target < 0.0) break;

        if (hud) {
            // Brief settle window so AppleUSBAudio's UAC2 interrupt and any KVO burst coalesce.
            usleep(15000);
            pthread_mutex_lock(&volumeLock);
            uint64_t latestSeq = pendingVolumeSeq;
            pthread_mutex_unlock(&volumeLock);
            if (latestSeq != seq) continue;
        }

        if (!hud) {
            pthread_mutex_lock(&volumeLock);
            agentChangingVolume = YES;
            pthread_mutex_unlock(&volumeLock);
            setMasterVolume(target);
            pthread_mutex_lock(&volumeLock);
            agentChangingVolume = NO;
            agentVolumeIgnoreUntilNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200 * NSEC_PER_MSEC;
            trackedCurVolume = (Float32)target;
            pthread_mutex_unlock(&volumeLock);
            lastAppliedVolume = target;
        } else {
            Float32 cur = (Float32)target;
            BOOL haveCur = getMasterVolume(&cur);
            double ref = haveCur ? (double)cur : (lastAppliedVolume >= 0.0 ? lastAppliedVolume : target);

            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            pthread_mutex_lock(&volumeLock);
            Float32 extPrev = trackedPrevVolume;
            uint64_t extAge = trackedChangeTimeNs ? (now - trackedChangeTimeNs) : UINT64_MAX;
            pthread_mutex_unlock(&volumeLock);

            // If AppleUSBAudio already applied this target via UAC2 just moments ago, use the level before it.
            if (haveCur && fabs((double)cur - target) < 0.005 && extPrev >= 0.0f && extAge < 500 * NSEC_PER_MSEC) {
                ref = (double)extPrev;
            } else if (haveCur && fabs((double)cur - target) < 0.005 && lastAppliedVolume >= 0.0 &&
                       fabs(lastAppliedVolume - target) >= 0.005) {
                ref = lastAppliedVolume;
            }

            int dir = (target > ref + 0.002) ? 1 : (target < ref - 0.002) ? -1 : 0;
            if (dir == 0) {
                pthread_mutex_lock(&volumeLock);
                agentChangingVolume = YES;
                pthread_mutex_unlock(&volumeLock);
                setMasterVolume(target);
                pthread_mutex_lock(&volumeLock);
                agentChangingVolume = NO;
                agentVolumeIgnoreUntilNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200 * NSEC_PER_MSEC;
                trackedCurVolume = (Float32)target;
                pthread_mutex_unlock(&volumeLock);
                lastAppliedVolume = target;
            } else {
                // Pre-position CoreAudio one macOS step (1/16) before target so posting a single
                // NX_KEYTYPE_SOUND_UP/DOWN lands directly on target and renders a smooth native HUD.
                const double step = 1.0 / 16.0;
                double pre = (dir > 0) ? MAX(0.0, target - step) : MIN(1.0, target + step);

                pthread_mutex_lock(&volumeLock);
                agentChangingVolume = YES;
                pthread_mutex_unlock(&volumeLock);

                if (!haveCur || fabs((double)cur - pre) > 0.005) {
                    setMasterVolume(pre);
                    usleep(15000);
                    pthread_mutex_lock(&volumeLock);
                    uint64_t latestSeq = pendingVolumeSeq;
                    pthread_mutex_unlock(&volumeLock);
                    if (latestSeq != seq) {
                        pthread_mutex_lock(&volumeLock);
                        agentChangingVolume = NO;
                        agentVolumeIgnoreUntilNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200 * NSEC_PER_MSEC;
                        pthread_mutex_unlock(&volumeLock);
                        continue;
                    }
                }

                dispatch_sync(dispatch_get_main_queue(), ^{
                    postMediaKey(dir > 0 ? NX_KEYTYPE_SOUND_UP : NX_KEYTYPE_SOUND_DOWN);
                });
                lastAppliedVolume = target;

                usleep(60000);
                pthread_mutex_lock(&volumeLock);
                uint64_t latestSeq = pendingVolumeSeq;
                pthread_mutex_unlock(&volumeLock);
                if (latestSeq != seq) {
                    pthread_mutex_lock(&volumeLock);
                    agentChangingVolume = NO;
                    agentVolumeIgnoreUntilNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200 * NSEC_PER_MSEC;
                    pthread_mutex_unlock(&volumeLock);
                    continue;
                }

                Float32 after = (Float32)target;
                if (getMasterVolume(&after) && fabs((double)after - target) > 0.005) {
                    setMasterVolume(target);
                }

                pthread_mutex_lock(&volumeLock);
                agentChangingVolume = NO;
                agentVolumeIgnoreUntilNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200 * NSEC_PER_MSEC;
                trackedCurVolume = (Float32)target;
                pthread_mutex_unlock(&volumeLock);
            }
        }

        pthread_mutex_lock(&volumeLock);
        if (pendingVolumeSeq == seq) {
            volumeWorkerRunning = NO;
            pthread_mutex_unlock(&volumeLock);
            break;
        }
        pthread_mutex_unlock(&volumeLock);
    }
}

static BOOL enqueueVolumeLevel(const char *line) {
    double volLevel = 0.0;
    int volHud = 0;
    int parsed = sscanf(line, "vollevel %lf %d", &volLevel, &volHud);
    if (parsed < 1) parsed = sscanf(line, "volume_level %lf %d", &volLevel, &volHud);
    if (parsed < 1) return NO;

    if (volLevel < 0.0) volLevel = 0.0;
    if (volLevel > 1.0) volLevel = 1.0;

    pthread_mutex_lock(&volumeLock);
    pendingVolumeTarget = volLevel;
    if (parsed >= 2 && volHud != 0) pendingVolumeHud = 1;
    pendingVolumeSeq++;
    BOOL needStart = !volumeWorkerRunning;
    volumeWorkerRunning = YES;
    pthread_mutex_unlock(&volumeLock);

    if (needStart && volumeQueue) {
        dispatch_async(volumeQueue, ^{ processPendingVolume(); });
    }
    return YES;
}

static void handleLine(const char *line) {
    unsigned id;
    int vol;
    double x, y, dx, dy;
    char phase;
    double bpct;
    int bext, bchg, bfl;
    if (sscanf(line, "hit %u %lf %lf", &id, &x, &y) == 3) answerHit(id, x, y);
    else if (sscanf(line, "tap %u %lf %lf", &id, &x, &y) == 3) answerTap(id, x, y);
    else if (sscanf(line, "look %u", &id) == 1) answerLook(id);
    else if (sscanf(line, "touchwatch %u", &id) == 1) answerTouchWatch(id);
    else if (sscanf(line, "volume %d", &vol) == 1) {
        if (volumeQueue) {
            dispatch_async(volumeQueue, ^{
                pthread_mutex_lock(&volumeLock);
                agentChangingVolume = YES;
                pthread_mutex_unlock(&volumeLock);
                dispatch_sync(dispatch_get_main_queue(), ^{
                    postMediaKey(vol > 0 ? NX_KEYTYPE_SOUND_UP : vol < 0 ? NX_KEYTYPE_SOUND_DOWN : NX_KEYTYPE_MUTE);
                });
                pthread_mutex_lock(&volumeLock);
                agentChangingVolume = NO;
                agentVolumeIgnoreUntilNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200 * NSEC_PER_MSEC;
                pthread_mutex_unlock(&volumeLock);
            });
        }
    }
    else if (enqueueVolumeLevel(line)) {}
    else if (sscanf(line, "battery %lf %d %d %d", &bpct, &bext, &bchg, &bfl) == 4) {
        updateBattery(bpct, bext, bchg, bfl);
    }
    else if (sscanf(line, "menuclose %u", &id) == 1) answerMenuClose(id);
    else if (sscanf(line, "menu %u", &id) == 1) answerMenu(id);
    else if (sscanf(line, "s %c %lf %lf %lf %lf", &phase, &x, &y, &dx, &dy) == 5) dispatch_async(scrollQueue, ^{ scrollLine(phase, x, y, dx, dy); });
    else if (sscanf(line, "stop %u", &id) == 1) dispatch_async(scrollQueue, ^{
        stopGlide(); // on the queue the glide's steps run on: none comes after this
        linkSend([NSString stringWithFormat:@"stopped %u\n", id]); // a click Henge holds for it goes now
    });
    else if (!strcmp(line, "stop")) dispatch_async(scrollQueue, ^{ stopGlide(); });
    else if (!strcmp(line, "lighten")) dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        applyLightweightUIFlags();
        applyHostModelName();
    });
    else if (!strcmp(line, "ask")) dispatch_async(dispatch_get_main_queue(), ^{ showTrustSettings(); });
    else if (!strcmp(line, "tools")) {
        toolsWanted = YES; // now, before the device plugs the disk in: not ejected on sight
        dispatch_async(toolsQueue, ^{ openToolsInstaller(); });
    } else if (!strncmp(line, "format ", 7) && strlen(line) < 160) { // "format <serial> <name, spaces and all>"
        const char *sp = strchr(line + 7, ' ');
        if (sp && sp[1]) {
            NSString *serial = [[NSString alloc] initWithBytes:line + 7 length:(NSUInteger)(sp - line - 7) encoding:NSUTF8StringEncoding];
            NSString *name = [@(sp + 1) stringByReplacingOccurrencesOfString:@":" withString:@"-"];
            if (serial && name) dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ formatDrive(serial, name); });
        }
    } else if (!strncmp(line, "eject ", 6) && strlen(line) < 64) {
        NSString *serial = @(line + 6);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ ejectDrive(serial); });
    }
    else if (!strcmp(line, "update")) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ update(); });
    else if (sscanf(line, "sync %u", &id) == 1) syncDisks(id);
    else if (sscanf(line, "power shutdown %u", &id) == 1) dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ askPower(NO, id); });
    else if (sscanf(line, "power restart %u", &id) == 1) dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ askPower(YES, id); });
    else if (!strncmp(line, "bench ", 6) && line[6] && strlen(line) < 220 && !strchr(line + 6, ' ')) benchAsked(line + 6);
}

static uint64_t startedAt; // when this process started, for how long the link took

// MARK: - The touchscreen (macOS 27 and later)

// On macOS 27, once the system part has turned on WindowServer's support for an external touchscreen, Henge plugs in
// a USB touchscreen and macOS takes the iPad's fingers as touches of its own. Whether macOS did take it is what the
// screen says (its touchCapabilities); the device waits for "touchscreen on" before fingers go there, and on
// anything else keeps imitating touch with the pointer. Asked by name: the helper builds for macOS 12 too.
static int touchscreenSent = -1;

static int touchscreenNow(void) {
    NSScreen *screen = NSScreen.screens.firstObject; // the screen with the menu bar: where macOS puts the touchscreen
    if (!screen || ![screen respondsToSelector:NSSelectorFromString(@"touchCapabilities")]) return 0;
    id caps = [screen valueForKey:@"touchCapabilities"];
    return [caps respondsToSelector:@selector(unsignedIntegerValue)] && ([caps unsignedIntegerValue] & 1) ? 1 : 0;
}

static void touchscreenTell(BOOL always) {
    dispatch_async(dispatch_get_main_queue(), ^{ // NSScreen on the main thread
        int now = touchscreenNow();
        if (!always && now == touchscreenSent) return;
        touchscreenSent = now;
        linkSend(now ? @"touchscreen on\n" : @"touchscreen off\n");
    });
}

static void touchscreenRelinked(void) { touchscreenTell(YES); }
static void screenRelinked(void); // whether the screen is locked or asleep: below

// Screens change as the touchscreen comes and goes; a timer as well, as nothing promises a notification for that.
static void watchTouchscreen(void) {
    if (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27) return; // "off" for good: said once linked
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil
                                                usingBlock:^(NSNotification *note) { (void)note; touchscreenTell(NO); }];
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) { (void)timer; touchscreenTell(NO); }];
}

// Whether macOS does anything with the touchscreen's touches. touchCapabilities says macOS has a touchscreen, but
// WindowServer can have one and still drop every touch (macOS 27.2 does when it finds no screen for it: "no usable
// displayUUID"), and then a finger would do nothing at all. So as a gesture starts on the touchscreen the device asks
// "touchwatch <id>", and the helper answers "touchseen <id> 1" once macOS makes any event that isn't a key's (a click,
// a move, a scroll, a gesture: what touches become), "touchseen <id> 0" if none comes in a second, or -1 if it can't
// see events (no Accessibility permission; the device tries again once it has it). On a 0 the device stops using the touchscreen and imitates touch with
// the pointer, as on older macOS. A listen-only event tap: it sees events, it never holds or changes them.
static _Atomic uint64_t touchEventAt; // CLOCK_UPTIME_RAW ns of the last event that isn't a key's
// The last events seen (main thread writes, the watch reads after): what macOS made of a touch, for Henge's log.
static struct { _Atomic uint64_t at; _Atomic int type; } touchSeen[64];
static _Atomic unsigned touchSeenNext;
static int touchTypesSaid;
static CFMachPortRef touchTap;        // main thread
static dispatch_queue_t touchWatchQueue;

static CGEventRef touchTapSaw(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *info) {
    (void)proxy, (void)info;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (touchTap) CGEventTapEnable(touchTap, true);
    } else if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != getpid()) {
        // Not the helper's own (its scroll glide): those would say macOS took a touch it may have dropped.
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        atomic_store(&touchEventAt, now);
        unsigned i = atomic_fetch_add(&touchSeenNext, 1) % 64;
        atomic_store(&touchSeen[i].at, now);
        atomic_store(&touchSeen[i].type, (int)type);
    }
    return event;
}

// Main thread. Made as the helper starts, so it already sees the first touch, and again if it couldn't be
// (Accessibility turned on since). One macOS turned off (it took too long) is turned on again.
static BOOL touchTapReady(void) {
    if (touchTap) {
        if (!CGEventTapIsEnabled(touchTap)) CGEventTapEnable(touchTap, true);
        return CGEventTapIsEnabled(touchTap);
    }
    CGEventMask notPointer = CGEventMaskBit(kCGEventNull) | CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                             CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(14 /* NX_SYSDEFINED: media keys and the like */);
    touchTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly, ~notPointer, touchTapSaw, NULL);
    if (!touchTap) return NO;
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL, touchTap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRelease(source);
    CGEventTapEnable(touchTap, true);
    note(@"watching for what touches on the touchscreen do");
    return YES;
}

static void answerTouchWatch(unsigned id) {
    // The touch can reach macOS a moment before the question reaches the helper: what came just before counts.
    uint64_t asked = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), since = asked - 300 * NSEC_PER_MSEC;
    dispatch_async(dispatch_get_main_queue(), ^{
        // Without Accessibility macOS makes the tap and then hides every event from it, a real click too (seen on
        // macOS 27, where CGPreflightListenEventAccess says yes all the same): a silence that says nothing.
        BOOL ready = trusted() && touchTapReady();
        if (!ready) {
            linkSend([NSString stringWithFormat:@"touchseen %u -1\n", id]);
            return;
        }
        dispatch_async(touchWatchQueue, ^{
            int seen = 0;
            for (int i = 0; i < 24 && !(seen = atomic_load(&touchEventAt) >= since); i++) usleep(50000); // 1.2 s
            linkSend([NSString stringWithFormat:@"touchseen %u %d\n", id, seen]);
            // For the first gestures: which events macOS made of it (mouse events: macOS treats the touchscreen as a
            // mouse; touch or gesture events: as touch), over the second after.
            if (touchTypesSaid >= 12) return;
            touchTypesSaid++;
            usleep(1000000);
            NSDictionary *names = @{@1: @"click", @2: @"release", @3: @"right-click", @4: @"right-release", @5: @"move",
                                    @6: @"drag", @7: @"right-drag", @22: @"scroll", @29: @"gesture or direct touch", @30: @"magnify",
                                    // macOS 27.2's WindowServer sends a touchscreen's touches as type 18 (29 with its
                                    // DirectTouchEventType feature off), and apps take them as real touches. Often a
                                    // tap can't see them at all (they need an entitlement): "nothing" is no failure.
                                    @18: @"direct touch", @19: @"gesture-begin", @20: @"gesture-end", @31: @"swipe",
                                    @32: @"smart-magnify", @34: @"pressure", @37: @"direct-touch"};
            NSCountedSet *types = [NSCountedSet new];
            for (int i = 0; i < 64; i++)
                if (atomic_load(&touchSeen[i].at) >= since) [types addObject:@(atomic_load(&touchSeen[i].type))];
            NSMutableArray *said = [NSMutableArray array];
            for (NSNumber *type in [types.allObjects sortedArrayUsingSelector:@selector(compare:)])
                [said addObject:[NSString stringWithFormat:@"%@ x%lu", names[type] ?: [NSString stringWithFormat:@"type %@", type],
                                                           (unsigned long)[types countForObject:type]]];
            note([NSString stringWithFormat:@"a touch on the touchscreen became: %@",
                                            said.count ? [said componentsJoinedByString:@", "] : @"nothing"]);
        });
    });
}

static void keepLinked(void) {
    // Everything that needs the device waits for the link, so it is tried at once and often: the device's
    // network may still be coming up as macOS logs in.
    for (;; usleep(500000)) {
        int fd = connect_host(0);
        if (fd < 0) continue;
        uint64_t up = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - startedAt) / 1000000;
        // When the Mac started (its clock): the device asks afresh whether macOS takes touches once that changes.
        struct timeval bootedAt = {0};
        size_t bootedSize = sizeof bootedAt;
        if (sysctlbyname("kern.boottime", &bootedAt, &bootedSize, NULL, 0) != 0) bootedAt.tv_sec = 0;
        NSString *head = [NSString stringWithFormat:@"GET /.henge/link?v=%d&trusted=%d&up=%llu&boot=%.0f&booted=%ld HTTP/1.1\r\nHost: " HOST "\r\nUpgrade: henge-link\r\nConnection: Upgrade\r\n\r\n",
                                                    HV_AGENT_VERSION, trusted() ? 1 : 0, up, NSProcessInfo.processInfo.systemUptime, (long)bootedAt.tv_sec];
        if (send_all(fd, head.UTF8String, strlen(head.UTF8String)) != 0) {
            close(fd);
            continue;
        }
        char buf[4096], line[512];
        size_t n = 0;
        BOOL upgraded = NO, inHead = YES, answered = NO;
        for (;;) {
            ssize_t got = recv(fd, buf, sizeof buf, 0);
            if (got <= 0) break;
            for (ssize_t i = 0; i < got; i++) {
                char ch = buf[i];
                if (ch == '\r') continue;
                if (ch != '\n') {
                    if (n + 1 < sizeof line) line[n++] = ch;
                    continue;
                }
                line[n] = 0;
                n = 0;
                if (inHead) {
                    if (!upgraded) {
                        answered = YES;
                        upgraded = !strncmp(line, "HTTP/1.1 101", 12);
                        if (!upgraded) goto done; // an older Henge, without the link
                        pthread_mutex_lock(&linkLock);
                        linkFd = fd;
                        linkGeneration++;
                        pthread_mutex_unlock(&linkLock);
                        flushNotes();
                        focusRelinked();
                        touchscreenRelinked();
                        screenRelinked();
                    } else if (!line[0]) {
                        inHead = NO;
                    }
                    continue;
                }
                handleLine(line);
            }
        }
    done:
        pthread_mutex_lock(&linkLock);
        linkFd = -1;
        pthread_mutex_unlock(&linkLock);
        close(fd);
        dispatch_async(scrollQueue, ^{ stopGlide(); });
        benchStop();
        // Nothing to link to here (an older Henge answered): check now and then, in case Henge is updated. Closed
        // with no answer, it was not ready yet: again at once.
        if (!upgraded && answered) sleep(28);
    }
}

static void *linkThread(void *unused) {
    (void)unused;
    keepLinked();
    return NULL;
}

// MARK: - The system part (henge-agent --system)

// The same program, run by a LaunchDaemon (/Library/LaunchDaemons/com.henge.agent.system.plist, as root, installed
// with the Mac's password once): from the Mac's start, before anyone logs in, until it shuts down. It has no
// window server, so it does none of the above; it tells the device what the screen is at (macOS starting, the
// login window, someone's desktop) on a link of its own (share.c, "the system link"), and when someone logs in it
// starts their helper (the per-user LaunchAgent) at once, instead of when macOS gets round to it.

static dispatch_queue_t sessionQueue;
static NSString *sentSession;
static uid_t sessionUser; // whose desktop it is, while it's someone's

// SCDynamicStoreCopyConsoleUser, looked up when first wanted: the Makefile links no SystemConfiguration.
static CFStringRef consoleUser(uid_t *uid) {
    static CFStringRef (*copyUser)(void *, uid_t *, gid_t *);
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *sc = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_LAZY);
        if (sc) copyUser = (CFStringRef (*)(void *, uid_t *, gid_t *))dlsym(sc, "SCDynamicStoreCopyConsoleUser");
    });
    if (!copyUser) return NULL;
    gid_t gid;
    return copyUser(NULL, uid, &gid);
}

static BOOL processRunning(const char *name) {
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return NO;
    pid_t *pids = calloc((size_t)n + 32, sizeof *pids);
    n = proc_listallpids(pids, (int)(((size_t)n + 32) * sizeof *pids));
    BOOL found = NO;
    for (int i = 0; i < n && !found; i++) {
        char got[2 * MAXCOMLEN + 1] = "";
        if (proc_name(pids[i], got, sizeof got) > 0 && !strcmp(got, name)) found = YES;
    }
    free(pids);
    return found;
}

// "user" while someone has the screen (their desktop, or its lock screen), "login" at the login window, "starting"
// before the login window is up. Who is "on the console" is what macOS itself goes by: no one, or loginwindow,
// at the login window.
static NSString *sessionNow(uid_t *uid) {
    *uid = 0;
    const char *test = getenv("HENGE_AGENT_TEST_SESSION"); // tests: a file that says it
    if (test) {
        NSString *said = [NSString stringWithContentsOfFile:@(test) encoding:NSUTF8StringEncoding error:NULL];
        said = [said stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if ([said isEqualToString:@"user"]) *uid = getuid();
        return said.length ? said : @"starting";
    }
    uid_t who = 0;
    CFStringRef name = consoleUser(&who);
    BOOL someone = name && who != 0 && CFStringCompare(name, CFSTR("loginwindow"), 0) != kCFCompareEqualTo;
    if (name) CFRelease(name);
    if (!name) { // no answer: whoever owns the console device
        struct stat st;
        someone = stat("/dev/console", &st) == 0 && st.st_uid != 0;
        who = someone ? st.st_uid : 0;
    }
    if (someone) {
        *uid = who;
        return @"user";
    }
    return processRunning("loginwindow") ? @"login" : @"starting";
}

// MARK: - Never idle asleep (helper 26)

// The iPad decides when the Mac rests: Henge keeps the iPad awake while the Mac is on its screen, and pauses or
// suspends the Mac when it leaves. macOS's own idle sleep only got in the way: it turned the display off (Henge then
// kept showing the last picture, which looked like a freeze), locked the screen behind it, and touches on Henge's
// touchscreen didn't count as someone using the Mac. Both parts hold these for as long as they run (the system part
// covers the login window); a lock the person asks for (the menu, a key) still works.
static void keepAwake(void) {
    static IOPMAssertionID display, systemSleep;
    CFStringRef why = CFSTR("Henge: the iPad decides when this Mac sleeps");
    IOReturn d = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, why, &display);
    IOReturn s = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleSystemSleep, kIOPMAssertionLevelOn, why, &systemSleep);
    if (d != kIOReturnSuccess || s != kIOReturnSuccess)
        fprintf(stderr, "henge-agent: couldn't keep the Mac awake (display 0x%x, system 0x%x)\n", d, s);
}

// MARK: - Whether the screen can be used (helper 26)

// Locked, its display asleep, or behind the screen saver: macOS's lock screen and a sleeping display do nothing with
// the touchscreen's touches, so with fingers going there as touches the Mac would never wake or unlock, and Henge
// would look frozen. While this says "screen away 1" the device sends fingers with the pointer instead: a pointer
// move wakes the display, a click reaches the password field. Looked at once a second, and at once as macOS says so.
static dispatch_queue_t screenQueue;
static dispatch_source_t screenTimer; // kept: a timer nothing holds is freed at once and never fires
static int screenAwaySent = -1;       // on screenQueue

static int screenAwayNow(void) {
    int away = CGDisplayIsAsleep(CGMainDisplayID()) || screenLockedNow() ? 1 : 0;
    if (!away && processRunning("ScreenSaverEngine")) away = 1;
    return away;
}

static void screenTell(BOOL always) {
    if (!screenQueue) return;
    dispatch_async(screenQueue, ^{
        int now = screenAwayNow();
        if (!always && now == screenAwaySent) return;
        screenAwaySent = now;
        linkSend(now ? @"screen away 1\n" : @"screen away 0\n");
    });
}

static void screenRelinked(void) { screenTell(YES); }

static void watchScreen(void) {
    screenQueue = dispatch_queue_create("henge.screen", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_t timer = screenTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, screenQueue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(timer, ^{ screenTell(NO); });
    dispatch_resume(timer);
    NSDistributedNotificationCenter *dnc = NSDistributedNotificationCenter.defaultCenter;
    for (NSString *name in @[@"com.apple.screenIsLocked", @"com.apple.screenIsUnlocked", @"com.apple.screensaver.didstart",
                             @"com.apple.screensaver.didstop"])
        [dnc addObserverForName:name object:nil queue:nil usingBlock:^(NSNotification *note) { (void)note; screenTell(NO); }];
    NSNotificationCenter *wnc = NSWorkspace.sharedWorkspace.notificationCenter;
    for (NSNotificationName name in @[NSWorkspaceScreensDidSleepNotification, NSWorkspaceScreensDidWakeNotification])
        [wnc addObserverForName:name object:nil queue:nil usingBlock:^(NSNotification *note) { (void)note; screenTell(NO); }];
}

// launchctl as that person, as if they ran it: nothing of theirs runs as root. Returns its exit status, and what it
// printed if out is given.
static int launchctlAs(uid_t uid, NSArray<NSString *> *args, NSString **out) {
    struct passwd *pw = getpwuid(uid);
    if (!pw) return -1;
    gid_t gid = pw->pw_gid;
    int groups[64], count = 64;
    if (getgrouplist(pw->pw_name, (int)gid, groups, &count) != 0) count = 0;
    gid_t gids[64];
    for (int i = 0; i < count; i++) gids[i] = (gid_t)groups[i];
    const char *argv[16] = {"/bin/launchctl"};
    int argc = 1;
    for (NSString *a in args)
        if (argc < 15) argv[argc++] = a.fileSystemRepresentation;
    argv[argc] = NULL;
    int pipeFd[2];
    if (pipe(pipeFd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) { // only what is safe after fork in a program with threads, up to exec
        dup2(pipeFd[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        close(pipeFd[0]);
        if ((count && setgroups(count, gids) != 0) || setgid(gid) != 0 || setuid(uid) != 0) _exit(126);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipeFd[1]);
    if (pid < 0) {
        close(pipeFd[0]);
        return -1;
    }
    NSMutableData *data = [NSMutableData data];
    char buf[4096];
    ssize_t got;
    while ((got = read(pipeFd[0], buf, sizeof buf)) > 0) [data appendBytes:buf length:(NSUInteger)got];
    close(pipeFd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (out) *out = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Someone logged in: their helper now, not when macOS gets round to its agents. Started if macOS has loaded it, loaded
// from their LaunchAgents if not yet, tried for a minute and a half (their session may still be setting up).
static void startUserHelper(uid_t uid, BOOL justNow) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    struct passwd *pw = getpwuid(uid);
    if (!pw) return;
    NSString *plist = [@(pw->pw_dir) stringByAppendingPathComponent:@"Library/LaunchAgents/com.henge.agent.plist"];
    NSString *service = [NSString stringWithFormat:@"gui/%u/com.henge.agent", uid];
    if (getenv("HENGE_AGENT_TEST_PORT")) { // tests: this Mac's own helper is left alone
        printf("would start %s\n", service.UTF8String);
        fflush(stdout);
        return;
    }
    if (![NSFileManager.defaultManager fileExistsAtPath:plist]) {
        note([NSString stringWithFormat:@"someone logged in who doesn't have Henge Tools (uid %u)", uid]);
        return;
    }
    for (int i = 0; i < 180; i++) {
        if (i) usleep(500000);
        uid_t still = 0;
        if (![sessionNow(&still) isEqualToString:@"user"] || still != uid) return; // logged out again
        double after = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9;
        NSString *printed = nil;
        int found = launchctlAs(uid, @[ @"print", service ], &printed);
        if (found == 0 && [printed rangeOfString:@"state = running"].location != NSNotFound) {
            if (justNow || i)
                note([NSString stringWithFormat:@"someone logged in; their helper was running %.1f s later%@", after,
                                                i ? @" (started from here)" : @" (macOS had started it)"]);
            return;
        }
        if (found == 0) launchctlAs(uid, @[ @"kickstart", service ], NULL); // loaded, not running yet
        else launchctlAs(uid, @[ @"bootstrap", [NSString stringWithFormat:@"gui/%u", uid], plist ], NULL); // not loaded yet
    }
    note(@"someone logged in, but their helper didn't start within 90 s");
}

static void lookAtSession(void) {
    @autoreleasepool {
        static NSString *seen; // what the last look found, linked or not
        uid_t uid = 0;
        NSString *now = sessionNow(&uid);
        BOOL isUser = [now isEqualToString:@"user"];
        if (isUser && uid != sessionUser) { // someone logged in, or was in when this started (it checks, then)
            BOOL justNow = seen != nil;
            sessionUser = uid;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ startUserHelper(uid, justNow); });
        }
        if (!isUser) sessionUser = 0;
        seen = now;
        if ([now isEqualToString:sentSession]) return;
        pthread_mutex_lock(&linkLock);
        BOOL linked = linkFd >= 0;
        pthread_mutex_unlock(&linkLock);
        if (!linked) return; // told once linked
        linkSend([NSString stringWithFormat:@"session %@\n", now]);
        sentSession = now;
    }
}

// WindowServer takes an external USB touchscreen as the main screen's only with backboardd's SupportExternalTouchscreen
// preference (macOS 27 and later; BackBoardHIDTouchEventProcessor reads it as a device arrives). Set for every user
// (/Library/Preferences/backboardd.plist, so WindowServer's own user sees it) through cfprefsd, which serves it at
// once. Then the device plugs the touchscreen in: "touchscreen ready". "touchscreen no" on older macOS, where it
// stays out and touch is imitated with the pointer as before.
static BOOL touchscreenSupport;

static void turnOnTouchscreen(void) {
    if (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27) return;
    if (![NSFileManager.defaultManager fileExistsAtPath:@"/System/Library/PrivateFrameworks/BackBoardHIDTouchEventProcessor.framework"]) return;
    CFStringRef key = CFSTR("SupportExternalTouchscreen"), domain = CFSTR("backboardd");
    Boolean valid = false;
    if (!CFPreferencesGetAppBooleanValue(key, domain, &valid) || !valid) {
        CFPreferencesSetValue(key, kCFBooleanTrue, domain, kCFPreferencesAnyUser, kCFPreferencesAnyHost);
        CFPreferencesSynchronize(domain, kCFPreferencesAnyUser, kCFPreferencesAnyHost);
    }
    CFPropertyListRef set = CFPreferencesCopyValue(key, domain, kCFPreferencesAnyUser, kCFPreferencesAnyHost);
    touchscreenSupport = set && CFGetTypeID(set) == CFBooleanGetTypeID() && CFBooleanGetValue(set);
    if (set) CFRelease(set);
    if (touchscreenSupport) note(@"macOS's external touchscreen support is on");
}

// WindowServer takes a digitizer for a screen's only by that screen's ID on the digitizer's HID service: a string
// property "displayUUID" (SkyLight's WSHIDDisplayEventListener::handle_digitizer_added: "no usable displayUUID
// property; treating as no digitizer added"; BackBoard's touch processor drops the touches of a USB one without it,
// "ignoring digitizer event for display (null)"). Nothing in macOS puts it on a USB touchscreen. So the system part
// (root) puts it there: on the touchscreen's IOHIDDevice (AppleUserHIDDevice keeps what root sets; the HID service
// under it doesn't, and macOS's HID clients see its parents' properties), the moment the device appears, which is
// tens of milliseconds before WindowServer asks. Henge plugs the touchscreen in only once the screen's ID is known
// ("touchscreen ready"); a touchscreen tagged after its HID service was already there may have been asked about
// first, so Henge is asked to plug it in again ("touchscreen replug"), twice at most.
static _Atomic(BOOL) tracing;         // Henge asked for the touch trace ("trace"): the tests below run too
static dispatch_queue_t tagQueue;     // all of the below, on it
static NSString *screenID;            // the main screen's (CGDisplayCreateUUIDFromDisplayID), once found
static uint64_t taggedEntry;          // the IORegistry entry ID of the touchscreen last tagged
static NSDate *taggedAt;              // and when
static uint64_t taggedNs;
static int replugsAsked;
static IONotificationPortRef tagPort;

// henge-agent --display-uuid: the main screen's ID, from WindowServer (a process of its own, as the system part has
// no window server connection of its own and mustn't hang on one).
static int printDisplayUUID(void) {
    CGDirectDisplayID main = CGMainDisplayID();
    if (!main) return 1;
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(main);
    if (!uuid) return 1;
    CFStringRef text = CFUUIDCreateString(NULL, uuid);
    CFRelease(uuid);
    if (!text) return 1;
    printf("%s\n", ((__bridge NSString *)text).UTF8String);
    CFRelease(text);
    return 0;
}

static NSString *screenIDFromWindowServer(void) {
    char path[PATH_MAX];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return nil;
    int pipeFd[2];
    if (pipe(pipeFd) != 0) return nil;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipeFd[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        close(pipeFd[0]);
        execl(path, path, "--display-uuid", (char *)NULL);
        _exit(127);
    }
    close(pipeFd[1]);
    if (pid < 0) {
        close(pipeFd[0]);
        return nil;
    }
    char buf[128];
    size_t n = 0;
    uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 4 * NSEC_PER_SEC;
    for (;;) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now >= until) break;
        struct pollfd pfd = {pipeFd[0], POLLIN, 0};
        int ready = poll(&pfd, 1, (int)((until - now) / 1000000) + 1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        ssize_t got = read(pipeFd[0], buf + n, sizeof buf - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
        if (n >= sizeof buf - 1) break;
    }
    close(pipeFd[0]);
    kill(pid, SIGKILL); // gone already, or stuck
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    buf[n] = 0;
    NSString *text = [@(buf) stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [[NSUUID alloc] initWithUUIDString:text] ? text.uppercaseString : nil;
}

// WindowServer's record of the screens it has had: the guest has one screen, so one ID there is it.
static NSString *screenIDFromPreferences(void) {
    NSDictionary *prefs = [NSDictionary dictionaryWithContentsOfFile:@"/Library/Preferences/com.apple.windowserver.displays.plist"];
    NSArray *configs = [prefs[@"DisplayAnyUserSets"] isKindOfClass:NSDictionary.class] ? prefs[@"DisplayAnyUserSets"][@"Configs"] : nil;
    if (![configs isKindOfClass:NSArray.class]) return nil;
    NSMutableSet<NSString *> *ids = [NSMutableSet set];
    for (NSDictionary *config in configs) {
        if (![config isKindOfClass:NSDictionary.class] || ![config[@"DisplayConfig"] isKindOfClass:NSArray.class]) continue;
        for (NSDictionary *display in config[@"DisplayConfig"])
            if ([display isKindOfClass:NSDictionary.class] && [display[@"UUID"] isKindOfClass:NSString.class] &&
                [[NSUUID alloc] initWithUUIDString:display[@"UUID"]])
                [ids addObject:[display[@"UUID"] uppercaseString]];
    }
    return ids.count == 1 ? ids.anyObject : nil;
}

static BOOL hasHIDService(io_registry_entry_t device) {
    io_iterator_t it = 0;
    if (IORegistryEntryCreateIterator(device, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS) return NO;
    BOOL found = NO;
    for (io_registry_entry_t child; !found && (child = IOIteratorNext(it));) {
        found = IOObjectConformsTo(child, "IOHIDEventService");
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
    return found;
}

static BOOL isTouchscreen(io_registry_entry_t device) {
    if (!IOObjectConformsTo(device, "IOHIDDevice")) return NO;
    CFTypeRef product = IORegistryEntryCreateCFProperty(device, CFSTR("Product"), kCFAllocatorDefault, 0);
    BOOL ours = product && CFGetTypeID(product) == CFStringGetTypeID() && CFEqual(product, CFSTR("Henge Touchscreen"));
    if (product) CFRelease(product);
    return ours;
}

// WindowServer's own word on the touchscreen, in Henge's log: what it said since `since`, whole.
static void reportWindowServer(NSDate *since) {
    NSDateFormatter *format = [NSDateFormatter new];
    format.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    format.dateFormat = @"yyyy-MM-dd HH:mm:ss";
    NSString *start = [format stringFromDate:[since dateByAddingTimeInterval:-2]];
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/log"];
    task.arguments = @[@"show", @"--start", start, @"--style", @"compact", @"--predicate",
                       @"process == \"WindowServer\" AND (eventMessage CONTAINS[c] \"digitizer\" OR eventMessage CONTAINS[c] \"displayUUID\" OR eventMessage CONTAINS[c] \"touch\")"];
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![task launchAndReturnError:NULL]) return note(@"WindowServer's log: couldn't run log");
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    int said = 0;
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange at = [line rangeOfString:@" WindowServer"];
        if (at.location == NSNotFound) continue;
        NSString *when = line.length > 23 ? [line substringWithRange:NSMakeRange(11, 12)] : @"";
        NSString *what = [line substringFromIndex:NSMaxRange(at)];
        if (what.length > 300) what = [what substringToIndex:300];
        note([NSString stringWithFormat:@"WindowServer %@%@", when, what]);
        if (++said == 10) break;
    }
    if (!said) note([NSString stringWithFormat:@"WindowServer said nothing about touch since %@", start]);
}

static void tagTouchscreen(io_registry_entry_t device, BOOL wasThere) {
    if (!screenID || !isTouchscreen(device)) return;
    uint64_t entry = 0;
    IORegistryEntryGetRegistryEntryID(device, &entry);
    if (entry && entry == taggedEntry) return; // seen through its interface too
    CFTypeRef had = IORegistryEntryCreateCFProperty(device, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
    BOOL already = had && CFEqual(had, (__bridge CFStringRef)screenID);
    if (had) CFRelease(had);
    taggedEntry = entry;
    if (already) return;
    BOOL late = hasHIDService(device); // WindowServer may have asked already
    kern_return_t kr = IORegistryEntrySetCFProperty(device, CFSTR("displayUUID"), (__bridge CFStringRef)screenID);
    CFTypeRef now = IORegistryEntryCreateCFProperty(device, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
    BOOL kept = now && CFEqual(now, (__bridge CFStringRef)screenID);
    if (now) CFRelease(now);
    if (!kept) {
        note([NSString stringWithFormat:@"couldn't tag the touchscreen with the screen's ID (0x%x)", kr]);
        return;
    }
    note([NSString stringWithFormat:@"tagged the touchscreen with the screen's ID %@%@", screenID,
                                    wasThere ? @" (it was in already)" : late ? @" (its HID service was there already)" : @""]);
    if ((late || wasThere) && replugsAsked < 2) {
        replugsAsked++;
        linkSend(@"touchscreen replug\n");
    }
    taggedAt = [NSDate date];
    taggedNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// MARK: What macOS makes of the tag (a test, while the touchscreen isn't taken)

// The touchscreen's IOHIDDevice above this HID service, if it is one of its (0 if not); retained.
static io_registry_entry_t touchscreenAbove(io_registry_entry_t service) {
    io_registry_entry_t at = service;
    IOObjectRetain(at);
    for (int up = 0; up < 6; up++) {
        io_registry_entry_t parent = 0;
        kern_return_t kr = IORegistryEntryGetParentEntry(at, kIOServicePlane, &parent);
        IOObjectRelease(at);
        if (kr != KERN_SUCCESS || !parent) return 0;
        if (isTouchscreen(parent)) return parent;
        at = parent;
    }
    IOObjectRelease(at);
    return 0;
}

static NSString *describe(CFTypeRef value) {
    if (!value) return @"none";
    NSString *text = [(__bridge id)value description];
    return text.length > 60 ? [text substringToIndex:60] : text;
}

// What each kind of HID client (WindowServer's among them) gets for displayUUID on the touchscreen's HID service,
// where in the registry it is, and what WindowServer logged.
static void diagnoseTouchscreen(uint64_t serviceID) {
    io_registry_entry_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IORegistryEntryIDMatching(serviceID));
    if (!service) return note(@"test: the touchscreen's HID service is gone");
    io_name_t cls = "";
    IOObjectGetClass(service, cls);
    CFTypeRef own = IORegistryEntryCreateCFProperty(service, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
    CFTypeRef searched = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("displayUUID"), kCFAllocatorDefault,
                                                         kIORegistryIterateRecursively | kIORegistryIterateParents);
    NSMutableString *chain = [NSMutableString string];
    io_registry_entry_t at = service;
    IOObjectRetain(at);
    for (int up = 0; at && up < 6; up++) {
        io_name_t name = "";
        IOObjectGetClass(at, name);
        CFTypeRef has = IORegistryEntryCreateCFProperty(at, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
        [chain appendFormat:@"%s%s < ", name, has ? "*" : ""];
        if (has) CFRelease(has);
        io_registry_entry_t parent = 0;
        if (IORegistryEntryGetParentEntry(at, kIOServicePlane, &parent) != KERN_SUCCESS) parent = 0;
        IOObjectRelease(at);
        at = parent;
    }
    if (at) IOObjectRelease(at);
    note([NSString stringWithFormat:@"test: service %s 0x%llx: own displayUUID %@, with parents %@; chain (* has it): %@", cls, serviceID,
                                    describe(own), describe(searched), chain]);
    if (own) CFRelease(own);
    if (searched) CFRelease(searched);
    CFMutableDictionaryRef props = NULL;
    if (IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
        NSArray *keys = [[(__bridge NSDictionary *)props allKeys] sortedArrayUsingSelector:@selector(compare:)];
        note([@"test: its keys: " stringByAppendingString:[keys componentsJoinedByString:@" "]]);
        CFRelease(props);
    }
    IOObjectRelease(service);
    // As the HID event system serves it, to each kind of client (0 admin, 1 monitor, 2 passive, 3 rate-controlled, 4 simple).
    typedef IOHIDEventSystemClientRef (*CreateWithType)(CFAllocatorRef, int, CFDictionaryRef);
    CreateWithType create = (CreateWithType)dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientCreateWithType");
    NSMutableString *seen = [NSMutableString string];
    for (int type = 0; type <= 4; type++) {
        IOHIDEventSystemClientRef client = type == 4 ? IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault)
                                         : create ? create(kCFAllocatorDefault, type, NULL) : NULL;
        if (!client) {
            [seen appendFormat:@"%d: no client; ", type];
            continue;
        }
        CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
        NSString *got = @"no service";
        for (CFIndex i = 0; services && i < CFArrayGetCount(services); i++) {
            IOHIDServiceClientRef one = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
            CFTypeRef rid = IOHIDServiceClientGetRegistryID(one);
            uint64_t id = 0;
            if (rid && CFGetTypeID(rid) == CFNumberGetTypeID()) CFNumberGetValue(rid, kCFNumberSInt64Type, &id);
            if (id != serviceID) continue;
            CFTypeRef value = IOHIDServiceClientCopyProperty(one, CFSTR("displayUUID"));
            got = describe(value);
            if (value) CFRelease(value);
            break;
        }
        if (services) CFRelease(services);
        CFRelease(client);
        [seen appendFormat:@"%d: %@; ", type, got];
    }
    note([@"test: HID clients see displayUUID: " stringByAppendingString:seen]);
    // Not WindowServer's log any more: macOS reads the tag now, and `log show` kept a busy Mac busier for minutes.
    (void)reportWindowServer;
}

// The touchscreen's HID service appeared: the ID put on it too (where AppleUserHIDEventService may or may not keep
// it, directly or as the HID event system sets a service's properties), as quickly as can be, then the test.
static void touchscreenServiceAppeared(void *refcon, io_iterator_t it) {
    (void)refcon;
    for (io_registry_entry_t service; (service = IOIteratorNext(it));) {
        io_registry_entry_t device = screenID ? touchscreenAbove(service) : 0;
        if (device) {
            IOObjectRelease(device);
            uint64_t serviceID = 0;
            IORegistryEntryGetRegistryEntryID(service, &serviceID);
            double ms = taggedNs ? (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - taggedNs) / 1e6 : -1;
            CFStringRef uuid = (__bridge CFStringRef)screenID;
            kern_return_t direct = IORegistryEntrySetCFProperty(service, CFSTR("displayUUID"), uuid);
            CFTypeRef kept = IORegistryEntryCreateCFProperty(service, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
            BOOL directKept = kept != NULL;
            if (kept) CFRelease(kept);
            kern_return_t wrapped = KERN_SUCCESS;
            BOOL wrappedKept = directKept;
            if (!directKept) {
                NSDictionary *wrap = @{@"displayUUID": screenID};
                wrapped = IORegistryEntrySetCFProperty(service, CFSTR("HIDEventServiceProperties"), (__bridge CFDictionaryRef)wrap);
                kept = IORegistryEntryCreateCFProperty(service, CFSTR("displayUUID"), kCFAllocatorDefault, 0);
                wrappedKept = kept != NULL;
                if (kept) CFRelease(kept);
            }
            if (!tracing) { // the tag is on it; what macOS makes of it is a test, for when Henge asks
                IOObjectRelease(service);
                continue;
            }
            note([NSString stringWithFormat:@"test: the touchscreen's HID service 0x%llx appeared %.0f ms after the tag; set on it 0x%x %@, as service properties 0x%x %@",
                                            serviceID, ms, direct, directKept ? @"kept" : @"not kept", wrapped, wrappedKept ? @"kept" : @"not kept"]);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                diagnoseTouchscreen(serviceID);
            });
        }
        IOObjectRelease(service);
    }
}


static void touchscreenAppeared(void *refcon, io_iterator_t it) {
    BOOL wasThere = refcon != NULL; // the first look: what was in before the system part started
    for (io_registry_entry_t entry; (entry = IOIteratorNext(it));) {
        io_registry_entry_t device = entry;
        if (IOObjectConformsTo(entry, "IOHIDInterface")) { // its device is its provider
            device = 0;
            IORegistryEntryGetParentEntry(entry, kIOServicePlane, &device);
        } else {
            IOObjectRetain(device);
        }
        if (device) {
            tagTouchscreen(device, wasThere);
            IOObjectRelease(device);
        }
        IOObjectRelease(entry);
    }
}

// What the device is told about the touchscreen: ready once the screen's ID is known, no on older macOS, nothing
// while still finding the ID (Henge waits).
static void touchscreenSay(void) {
    if (!touchscreenSupport) linkSend(@"touchscreen no\n");
    else if (screenID) linkSend(@"touchscreen ready\n");
}

static void watchForTouchscreen(void) {
    if (!touchscreenSupport) return;
    tagQueue = dispatch_queue_create("henge.touchscreen", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    dispatch_async(tagQueue, ^{
        tagPort = IONotificationPortCreate(MACH_PORT_NULL);
        if (!tagPort) {
            note(@"can't watch for the touchscreen");
            return;
        }
        IONotificationPortSetDispatchQueue(tagPort, tagQueue);
        const char *const classes[] = {"IOHIDDevice", "IOHIDInterface"};
        for (int i = 0; i < 2; i++) {
            io_iterator_t it = 0;
            if (IOServiceAddMatchingNotification(tagPort, kIOFirstPublishNotification, IOServiceMatching(classes[i]),
                                                 touchscreenAppeared, NULL, &it) != KERN_SUCCESS) {
                note(@"can't watch for the touchscreen");
                continue;
            }
            // What is in already: drained now (arming the notification), looked at again once the ID is known.
            for (io_registry_entry_t e; (e = IOIteratorNext(it));) IOObjectRelease(e);
        }
        io_iterator_t services = 0;
        if (IOServiceAddMatchingNotification(tagPort, kIOFirstPublishNotification, IOServiceMatching("IOHIDEventService"),
                                             touchscreenServiceAppeared, NULL, &services) == KERN_SUCCESS)
            for (io_registry_entry_t e; (e = IOIteratorNext(services));) IOObjectRelease(e);
    });
    // The screen's ID: WindowServer may not be up yet as the Mac starts, so tried every two seconds until found.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        NSString *found = nil, *from = nil;
        for (int tries = 0; !found; tries++) {
            if ((found = screenIDFromWindowServer())) from = @"WindowServer";
            else if ((found = screenIDFromPreferences())) from = @"WindowServer's preferences";
            else sleep(2);
            if (tries == 15) note(@"still finding the screen's ID for the touchscreen");
        }
        double s = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9;
        dispatch_async(tagQueue, ^{
            screenID = found;
            note([NSString stringWithFormat:@"the screen's ID is %@ (from %@, %.1f s)", found, from, s]);
            // A touchscreen in already (the helper updated, or the Mac resumed) is tagged now, and replugged.
            io_iterator_t it = 0;
            if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOHIDDevice"), &it) == KERN_SUCCESS) {
                touchscreenAppeared((void *)1, it);
                IOObjectRelease(it);
            }
            touchscreenSay();
        });
    });
}

static void recordTouchMessages(void); // below: a test Henge asks for

// MARK: The Mac's ports (helper 37)

// Every port this Mac listens on from outside itself is the same port on the device, as a MacBook's would be on its
// network (host/net_forward.c): what the kernel's socket tables say, once a second, sent when it changes and each time
// the link comes up. Root (this LaunchDaemon) sees every process's sockets. A table this can't read as it knows it
// (a macOS that changed it) sends nothing: Henge keeps the standard sharing ports.
static dispatch_queue_t portsQueue;
static char sentPorts[4096];

static void lookAtPorts(void) {
    static AgentPort ports[HV_SHARE_MAX_PORTS];
    static BOOL told, failed;
    int sockets = 0;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    int n = agent_ports_read(ports, HV_SHARE_MAX_PORTS, &sockets);
    uint64_t took = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    if (n < 0) {
        if (!failed) note([NSString stringWithFormat:@"ports: can't read this macOS's socket tables (%s): Henge keeps the standard sharing ports", strerror(errno)]);
        failed = YES;
        return;
    }
    if (!told) { // once: what the first look found, so the log shows root saw everyone's
        BOOL ssh = NO;
        for (int i = 0; i < n; i++) ssh |= !ports[i].udp && ports[i].port == 22;
        note([NSString stringWithFormat:@"ports: %d sockets seen as %s, %d listened on from outside%s, %.1f ms", sockets,
                                        geteuid() ? "not root (its own only)" : "root", n, ssh ? " (Remote Login's 22 among them)" : "", took / 1e6]);
        told = YES;
    }
    // What the looks cost in this running Mac, and how many UDP servers are on the ports 49152 and up (sent like the
    // others, as a MacBook's would be reached there): once an hour, and at once the first time in it a look is slow.
    static AgentCost cost;
    AgentCost c;
    if (agent_cost_add(&cost, t0, took, &c)) {
        int high = 0;
        for (int i = 0; i < n; i++) high += ports[i].udp && ports[i].port >= 49152;
        note([NSString stringWithFormat:@"ports: %d looks in %.0f min took %.2f ms at least, %.2f ms on average, %.2f ms at most%s; "
                                        "%d unconnected UDP socket%s on 49152 and up",
                                        c.looks, (t0 - c.since) / 6e10, c.least / 1e6, c.sum / 1e6 / c.looks, c.most / 1e6,
                                        took >= AGENT_COST_SLOW ? " (this one slow)" : "", high, high == 1 ? "" : "s"]);
    }
    char line[sizeof sentPorts];
    agent_ports_line(ports, n, line, sizeof line);
    if (!strcmp(line, sentPorts)) return;
    pthread_mutex_lock(&linkLock);
    BOOL linked = linkFd >= 0;
    pthread_mutex_unlock(&linkLock);
    if (!linked) return; // sent once it is, from scratch
    linkSend(@(line));
    strcpy(sentPorts, line);
}

static void watchPorts(void) {
    portsQueue = dispatch_queue_create("henge.ports", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    static dispatch_source_t timer;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, portsQueue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(timer, ^{ lookAtPorts(); });
    dispatch_resume(timer);
}

// The key that shows Henge this link is the system part's, root's (share.c takes the Mac's ports only from it): made
// the first time, in a file only root may read (agent-ports.h). Read again for each link, so a file put right counts
// at once; what is wrong with it is said each time that changes. Never said itself.
static NSString *const SystemKeyPath = @"/Library/Application Support/Henge/system-key";

static const char *systemKeyPath(void) {
    const char *test = getenv("HENGE_AGENT_TEST_KEY"); // tests: a key of their own, owned by whoever runs them
    return test ? test : SystemKeyPath.fileSystemRepresentation;
}

// "Henge-Key: <64 hex digits>\r\n", or "" without a key.
static void systemKeyLine(char *out, size_t cap) {
    static char said[512];
    uint8_t key[AGENT_KEY_SIZE];
    char why[400];
    int got = agent_system_key(systemKeyPath(), getenv("HENGE_AGENT_TEST_KEY") ? geteuid() : 0, key, why, sizeof why);
    out[0] = 0;
    if (got < 0) {
        if (strcmp(said, why)) {
            snprintf(said, sizeof said, "%s", why);
            note([NSString stringWithFormat:@"port forwarding of this Mac's own ports is off until its key file is fixed: %s", why]);
        }
        return;
    }
    char hex[2 * AGENT_KEY_SIZE + 1];
    agent_key_hex(key, hex);
    snprintf(out, cap, "Henge-Key: %s\r\n", hex);
    memset_s(key, sizeof key, 0, sizeof key);
    memset_s(hex, sizeof hex, 0, sizeof hex);
    if (got == 1) note(@"made this Mac's key for Henge (only root may read it)");
    if (said[0]) note(@"this Mac's key file is right again: port forwarding of its own ports is back on");
    said[0] = 0;
}

// Once linked, for the installer (henge-tools-installer.m): it waits for this before it starts the user's helper, so
// the system part's link is the first, and Henge can take a new key (macOS reinstalled) with nothing more to do.
// It holds this process's ID, so the installer knows the one it started linked, not the one before it.
static void systemLinkedMark(void) {
    NSString *mark = [[@(systemKeyPath()) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"system-linked"];
    int fd = open(mark.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char pid[24];
    int n = snprintf(pid, sizeof pid, "%d", (int)getpid());
    if (write(fd, pid, (size_t)n) != n) { /* the installer waits its 8 s instead */ }
    close(fd);
}

static void applySystemLightweightFlags(void) {
    @autoreleasepool {
        CFStringRef ua = CFSTR("com.apple.universalaccess");
        CFPreferencesSetValue(CFSTR("reduceMotion"), kCFBooleanTrue, ua, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        CFPreferencesSetValue(CFSTR("reduceTransparency"), kCFBooleanTrue, ua, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        CFPreferencesSynchronize(ua, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);

        CFPreferencesSetValue(CFSTR("NSAppSleepDisabled"), kCFBooleanTrue, kCFPreferencesAnyApplication, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        CFPreferencesSynchronize(kCFPreferencesAnyApplication, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);

        // Control Center: show Battery in Menu Bar
        CFStringRef cc = CFSTR("com.apple.controlcenter");
        CFPreferencesSetValue(CFSTR("NSStatusItem Visible Battery"), kCFBooleanTrue, cc, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        CFPreferencesSetValue(CFSTR("Battery"), (__bridge CFPropertyListRef)@18, cc, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        CFPreferencesSynchronize(cc, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);

        note(@"system: applied system-wide accessibility lightweight flags and battery menu bar visibility");
    }
}

static void keepSystemLinked(void) {
    for (;; usleep(500000)) {
        int fd = connect_host(0);
        if (fd < 0) continue;
        char key[128];
        systemKeyLine(key, sizeof key);
        NSString *head = [NSString stringWithFormat:@"GET /.henge/system?v=%d&boot=%.0f HTTP/1.1\r\nHost: " HOST "\r\n%sUpgrade: henge-link\r\nConnection: Upgrade\r\n\r\n",
                                                    HV_AGENT_VERSION, NSProcessInfo.processInfo.systemUptime, key];
        memset_s(key, sizeof key, 0, sizeof key);
        if (send_all(fd, head.UTF8String, strlen(head.UTF8String)) != 0) {
            close(fd);
            continue;
        }
        char buf[1024], line[256];
        size_t n = 0;
        BOOL upgraded = NO, inHead = YES, answered = NO;
        for (;;) {
            ssize_t got = recv(fd, buf, sizeof buf, 0);
            if (got <= 0) break;
            for (ssize_t i = 0; i < got; i++) {
                char ch = buf[i];
                if (ch == '\r') continue;
                if (ch != '\n') {
                    if (n + 1 < sizeof line) line[n++] = ch;
                    continue;
                }
                line[n] = 0;
                n = 0;
                if (inHead) {
                    if (!upgraded) {
                        answered = YES;
                        upgraded = !strncmp(line, "HTTP/1.1 101", 12);
                        if (!upgraded) goto done; // an older Henge, without the system link
                    } else if (!line[0]) {
                        inHead = NO;
                        pthread_mutex_lock(&linkLock);
                        linkFd = fd;
                        linkGeneration++;
                        pthread_mutex_unlock(&linkLock);
                        if (tagQueue) dispatch_async(tagQueue, ^{ touchscreenSay(); }); // ready only once the screen's ID is known
                        else touchscreenSay();
                        dispatch_async(sessionQueue, ^{ sentSession = nil; lookAtSession(); }); // told afresh
                        if (portsQueue) dispatch_async(portsQueue, ^{ sentPorts[0] = 0; lookAtPorts(); }); // and the ports
                        flushNotes();
                        systemLinkedMark();
                    }
                    continue;
                }
                if (!strcmp(line, "update")) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ update(); });
                else if (!strcmp(line, "trace")) recordTouchMessages(); // a test Henge asks for (HENGE_TOUCH_TRACE)
                else if (!strcmp(line, "lighten")) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ applySystemLightweightFlags(); });
                else {
                    double bpct;
                    int bext, bchg, bfl;
                    if (sscanf(line, "battery %lf %d %d %d", &bpct, &bext, &bchg, &bfl) == 4) {
                        updatePowerSource(bpct, bext, bchg, bfl);
                    }
                }
            }
        }
    done:
        pthread_mutex_lock(&linkLock);
        linkFd = -1;
        pthread_mutex_unlock(&linkLock);
        close(fd);
        if (!upgraded && answered) sleep(28);
    }
}

static void *systemLinkThread(void *unused) {
    (void)unused;
    keepSystemLinked();
    return NULL;
}

// MARK: Apple's own word on touches (a test, helper 27; only when Henge asks, helper 29)

// On macOS 27.2 apps act like a touch Mac's (big window buttons, wider menus) only once AppKit gets real direct-touch
// events (NSEventTypeDirectTouch: +[NSEvent _lastInteractionWasDirectTouch], NSHitTestContext's touch source), and
// with Henge's touchscreen they don't. What WindowServer (BackBoard's touch processor runs in it) and AppKit say
// about each touch, whole, goes to "Henge touch trace.txt" in the shared folder, with the Mac's touch feature flags
// first: Henge's log is too small for it. Root, so log stream sees everything; its debug level only while it runs.
// It keeps a busy Mac busier and writes up to 40 MB into the person's shared folder, so it runs only when Henge says
// "trace" on the system link (HENGE_TOUCH_TRACE=1 in henge-env.txt on the iPad), from then until the Mac restarts.
static void traceFlags(NSFileHandle *file) {
    BOOL (*enabled)(const char *, const char *) = (BOOL (*)(const char *, const char *))dlsym(RTLD_DEFAULT, "_os_feature_enabled_impl");
    static const char *flags[][2] = {
        {"WindowServer", "ModernSidecarProcessor"}, {"WindowServer", "DirectTouchEventType"}, {"WindowServer", "SidecarQFA"},
        {"WindowServer", "Sidecar2"}, {"WindowServer", "SystemGestures"}, {"WindowServer", "SystemGesturesSecurity"},
        {"WindowServer", "TrackpadSystemGestures"}, {"WindowServer", "RawRemoteContextCoordinates"}, {"WindowServer", "BackCountry"},
        {"WindowServer", "FuseBoard"}, {"WindowServer", "Calistoga"}, {"backboardd", "SupportExternalTouchscreen"},
        {"AppKit", "SidecarQFALater"}, {"AppKit", "flexiGlassMacOS"}, {"Accessibility", "LargeMenus"},
    };
    NSMutableString *text = [NSMutableString stringWithFormat:@"\n==== %@ helper %d: feature flags\n", NSDate.date, HV_AGENT_VERSION];
    for (size_t i = 0; i < sizeof flags / sizeof *flags; i++)
        [text appendFormat:@"%s/%s = %s\n", flags[i][0], flags[i][1], !enabled ? "?" : enabled(flags[i][0], flags[i][1]) ? "on" : "off"];
    [file writeData:[text dataUsingEncoding:NSUTF8StringEncoding]];
    NSMutableArray *said = [NSMutableArray new];
    for (size_t i = 0; i < sizeof flags / sizeof *flags; i++)
        [said addObject:[NSString stringWithFormat:@"%s %s", flags[i][1], !enabled ? "?" : enabled(flags[i][0], flags[i][1]) ? "on" : "off"]];
    note([@"touch flags: " stringByAppendingString:[said componentsJoinedByString:@", "]]);
}

static void recordTouchMessages(void) {
    if (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27) return;
    if (tracing) return; // asked again as the link comes back: once is enough
    tracing = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *path;
        for (int i = 0;; i++) { // the shared folder comes a little after the Mac starts
            NSString *folder = sharedFolderOnVolume();
            if (folder) {
                path = [folder stringByAppendingPathComponent:@"Henge touch trace.txt"];
                break;
            }
            if (i == 30) return note(@"touch trace: no shared folder to put it in");
            sleep(10);
        }
        if (![NSFileManager.defaultManager fileExistsAtPath:path]) [NSData.data writeToFile:path atomically:NO];
        NSFileHandle *file = [NSFileHandle fileHandleForWritingAtPath:path];
        if (!file) return note([@"touch trace: couldn't write " stringByAppendingString:path]);
        [file seekToEndOfFile];
        traceFlags(file);
        note([@"touch trace: recording Apple's touch messages in " stringByAppendingString:path]);
        NSString *predicate =
            @"(process == \"WindowServer\" AND (subsystem == \"com.apple.BackBoard\" OR category == \"Touch\" OR "
            @"category == \"SystemGestures\" OR category == \"IndirectGestures\" OR eventMessage CONTAINS[c] \"touch\" OR "
            @"eventMessage CONTAINS[c] \"digitizer\" OR eventMessage CONTAINS[c] \"sidecar\")) OR "
            @"(subsystem == \"com.apple.AppKit\" AND (category == \"Touch\" OR category == \"Sidecar2\" OR category == \"Gestures\"))";
        __block unsigned long long written = 0;
        for (int run = 0; run < 20; run++) {
            NSTask *task = [NSTask new];
            task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/log"];
            task.arguments = @[@"stream", @"--level", @"debug", @"--style", @"compact", @"--predicate", predicate];
            NSPipe *pipe = [NSPipe pipe];
            task.standardOutput = pipe;
            task.standardError = pipe;
            pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
                NSData *data = h.availableData;
                if (!data.length || written > 40000000ull) return; // 40 MB is plenty
                written += data.length;
                @try { [file writeData:data]; } @catch (NSException *e) { (void)e; }
                if (written > 40000000ull) [task terminate]; // log stream never ends on its own
            };
            if (![task launchAndReturnError:NULL]) return note(@"touch trace: couldn't run log");
            [task waitUntilExit];
            pipe.fileHandleForReading.readabilityHandler = nil;
            if (written > 40000000ull) return note(@"touch trace: 40 MB recorded, stopped");
            sleep(5);
        }
    });
}

static int runSystem(void) {
    startedAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!getenv("HENGE_NO_LIGHTEN")) applySystemLightweightFlags();
    sessionQueue = dispatch_queue_create("henge.session", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    keepAwake();
    turnOnTouchscreen(); // before the link: what it says first
    watchForTouchscreen(); // before the link too: watching before Henge can plug it in
    watchPorts(); // before the link: its queue is there when the link comes up
    pthread_t link;
    pthread_create(&link, NULL, systemLinkThread, NULL);
    pthread_detach(link);
    // Four times a second: cheap (a question to configd, and a look at the process list while no one is in), and a
    // login is seen before the desktop is drawn.
    static dispatch_source_t timer;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, sessionQueue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 4, NSEC_PER_SEC / 20);
    dispatch_source_set_event_handler(timer, ^{ lookAtSession(); });
    dispatch_resume(timer);
    static dispatch_source_t ping;
    ping = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(ping, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), 15 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(ping, ^{ linkSend(@"p\n"); });
    dispatch_resume(ping);
    dispatch_main();
}

int main(int argc, char **argv) {
    @autoreleasepool {
        toolsQueue = dispatch_queue_create("henge.tools", DISPATCH_QUEUE_SERIAL);
        if (argc == 2 && getenv("HENGE_AGENT_TEST_TOOLS") && !strcmp(argv[1], "--tools-eject")) { // tests, on their own disk
            ejectToolsDisk();
            return toolsVolume() || toolsDevice() ? 1 : 0;
        }
        if (argc == 4 && getenv("HENGE_AGENT_TEST_FORMAT") && !strcmp(argv[1], "--format")) { // tests: a disk of their own
            int blank = blankDisk(@(argv[2]));
            printf("blank %d\n", blank);
            return formatBlank(@(argv[2]), @(argv[3])) ? 0 : 1;
        }
        if (argc == 3 && !strcmp(argv[1], "--drive")) { // which disk a USB serial number is, for a look by hand
            NSString *disk = driveWithSerial(@(argv[2]));
            printf("%s\n", disk ? disk.UTF8String : "none");
            return disk ? 0 : 1;
        }
        if (argc == 2 && getenv("HENGE_AGENT_TEST_TOOLS") && !strcmp(argv[1], "--tools-open")) {
            openToolsInstaller();
            return toolsVolume() ? 0 : 1;
        }
        if (argc == 2 && !strcmp(argv[1], "--keep-mounted")) { // tests
            keepMounted();
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--display-uuid")) return printDisplayUUID(); // the system part's question
        if (argc == 2 && !strcmp(argv[1], "--system")) return runSystem(); // the LaunchDaemon (see "The system part")
        if (argc == 3 && !strcmp(argv[1], "--bench")) return benchChild(argv[2]); // the benchmark's steps (helper 36)
        if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--bench-work")) return benchWork(atoi(argv[2]), argc == 4 && !strcmp(argv[3], "x86_64"));
        if (argc == 2 && !strcmp(argv[1], "--version")) {
            printf("%d\n", HV_AGENT_VERSION);
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--lighten")) {
            applyLightweightUIFlags();
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--focus")) { // what the device would be told, for a look by hand
            NSString *why, *app;
            uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            int said = lookNow(&why, &app);
            const char *state = focus_said_word(said);
            printf("field %u %s%s %s in %s, in %.1f ms (trusted %d)\n", fieldID, state, said == FOCUS_SAID_HELD ? " (held)" : "", fieldKindSaid, app ? app.UTF8String : "?",
                   (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6, trusted());
            if (!CGRectIsNull(fieldFrame)) printf("at %s (fractions of the screen)\n", NSStringFromRect(NSRectFromCGRect(fieldFrame)).UTF8String);
            if (why) printf("%s: %s\n", !strcmp(state, "unknown") ? "can't tell" : "focus on", why.UTF8String);
            if (fieldElement && fieldEdit == EDIT_YES && !fieldSecure) {
                NSString *context = caretContext(fieldElement);
                if (context) printf("ctx %u %s\n", fieldID, context.UTF8String);
            }
            return 0;
        }
        if (argc == 4 && !strcmp(argv[1], "--probe")) {
            probing = YES;
            CGPoint at = onScreen(atof(argv[2]), atof(argv[3]));
            printf("at %.0f,%.0f (trusted %d):\n", at.x, at.y, trusted());
            uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            int quick = dragsFromWindows(at);
            printf("windows: %s, in %.1f ms\n", quick == HIT_DRAG ? "drag" : quick == 2 ? "a title bar, if the app agrees" : "ask the app",
                   (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
            t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            AXUIElementRef hit = NULL;
            if (AXUIElementCopyElementAtPosition(systemWide(), (float)at.x, (float)at.y, &hit) != kAXErrorSuccess || !hit) {
                printf("-> nothing answers there\n");
                return 0;
            }
            int drag = dragsAt(hit, at);
            CGRect press;
            BOOL pressed = pressableAt(hit, &press);
            CFRelease(hit);
            printf("-> %s%s, in %.1f ms\n", drag == HIT_DRAG ? "drag" : "scroll", pressed ? ", pressed as touched" : "",
                   (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
            printf("menus showing: %d\n", menusShowing());
            t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            NSString *tap = tapAnswer(0, at, NULL);
            printf("%s", tap.UTF8String);
            printf("(in %.1f ms)\n", (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
            return 0;
        }
        startedAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        dispatch_queue_attr_t quick = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        hitQueue = dispatch_queue_create("henge.hit", quick);
        tapQueue = dispatch_queue_create("henge.tap", quick);
        scrollQueue = dispatch_queue_create("henge.scroll", quick);
        menuQueue = dispatch_queue_create("henge.menu", quick);
        glideWatchQueue = dispatch_queue_create("henge.glide-watch", quick);
        volumeQueue = dispatch_queue_create("henge.volume", quick);
        watchVolume();
        // The link first: until it is up the device can't tell a login from the login window, nor scroll smoothly.
        // The rest (the clipboard, the shared folder, the tools disk) can wait the moment macOS is busiest.
        pthread_attr_t linkAttr;
        pthread_attr_init(&linkAttr);
        pthread_attr_set_qos_class_np(&linkAttr, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_t link;
        pthread_create(&link, &linkAttr, linkThread, NULL);
        pthread_detach(link);
        pthread_attr_destroy(&linkAttr);
        lastChange = NSPasteboard.generalPasteboard.changeCount;
        BOOL test = getenv("HENGE_AGENT_TEST_PORT") != NULL; // touch only: this Mac's folder and clipboard stay as they are
        if (!test) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ followDevice(); });
        watchFocus(); // only looks, so tests hear it too
        touchWatchQueue = dispatch_queue_create("henge.touch-watch", quick);
        if (trusted()) touchTapReady();
        watchTouchscreen();
        watchScreen();
        keepAwake();
        if (!test) {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                applyHostModelName();
                if (!getenv("HENGE_NO_LIGHTEN")) applyLightweightUIFlags();
                refreshDockCacheOnce();
            });
        }
        if (!test || getenv("HENGE_AGENT_TEST_TOOLS")) watchToolsDisk();
        // At login macOS mounts My Shared Files about when this starts: a moment for it before WebDAV instead.
        if (!test) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (sharedFolderOnVolume() ? 0 : 8) * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ keepMounted(); });
        if (!trusted() && !test) askForTrust();
        __block BOOL wasTrusted = trusted();
        if (!test) writeStatus(wasTrusted);
        [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            if (!test) checkClipboard();
            BOOL now = trusted();
            if (now && !touchTap) touchTapReady(); // allowed since: it watches from now on
            if (now != wasTrusted) {
                linkSend([NSString stringWithFormat:@"trust %d\n", now ? 1 : 0]);
                if (!test) writeStatus(now);
            }
            wasTrusted = now;
        }];
        [NSTimer scheduledTimerWithTimeInterval:15 repeats:YES block:^(NSTimer *timer) { (void)timer; linkSend(@"p\n"); }];
        if (!test) [NSTimer scheduledTimerWithTimeInterval:30 repeats:YES block:^(NSTimer *timer) { (void)timer; keepMounted(); }];
        [NSRunLoop.mainRunLoop run];
    }
    return 0;
}
