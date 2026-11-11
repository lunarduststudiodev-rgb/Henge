#import <Foundation/Foundation.h>
#include <pthread.h>

@interface Greeter : NSObject
@property(nonatomic, copy) NSString *name;
- (NSString *)greet;
@end

static int loads;

@implementation Greeter
+ (void)load { loads++; }
- (NSString *)greet { return [NSString stringWithFormat:@"hello %@", self.name]; }
@end

@interface NSString (TLTest)
- (NSString *)tl_shout;
@end
@implementation NSString (TLTest)
- (NSString *)tl_shout { return [[self uppercaseString] stringByAppendingString:@"!"]; }
@end

static __thread int per_thread = 5;

static void *worker(void *arg) {
    per_thread += 10;
    *(int *)arg = per_thread;
    return NULL;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        Greeter *g = [Greeter new];
        g.name = @"translation layer";
        NSArray *items = @[ @3, @1, @2 ];
        NSArray *sorted = [items sortedArrayUsingSelector:@selector(compare:)];
        NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"tl-fnd-test.txt"];
        NSError *error = nil;
        BOOL wrote = [@"written" writeToFile:tmp atomically:YES encoding:NSUTF8StringEncoding error:&error];
        BOOL exists = [[NSFileManager defaultManager] fileExistsAtPath:tmp];
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
        NSString *caught = @"none";
        @try {
            [NSException raise:@"TLTest" format:@"thrown %d", 42];
        } @catch (NSException *e) {
            caught = [NSString stringWithFormat:@"%@: %@", e.name, e.reason];
        }
        __block int total = 0;
        dispatch_apply(4, dispatch_get_global_queue(0, 0), ^(size_t i) { __sync_fetch_and_add(&total, (int)i); });
        int from_thread = 0;
        pthread_t t;
        pthread_create(&t, NULL, worker, &from_thread);
        pthread_join(t, NULL);
        Class cls = NSClassFromString(@"Greeter");
        NSDictionary *json = [NSJSONSerialization JSONObjectWithData:[@"{\"a\":[1,2]}" dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        NSLog(@"NSLog works: %@", [g greet]);
        printf("%s | %s | sorted %s | file %d %d | %s | apply %d | tls %d %d | class %s | loads %d | json %s\n",
               [g greet].UTF8String, [@"hey" tl_shout].UTF8String, [sorted componentsJoinedByString:@","].UTF8String,
               wrote, exists, caught.UTF8String, total, per_thread, from_thread, cls == [Greeter class] ? "found" : "MISSING",
               loads, [json[@"a"] description].length ? "parsed" : "FAILED");
        printf("PASS fnd\n");
    }
    return 0;
}
