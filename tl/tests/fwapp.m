#import <Foundation/Foundation.h>

extern int tlfw_counter;
int tlfw_add(int a, int b);
@interface TLFWThing : NSObject
+ (int)loads;
- (NSString *)what;
@end
@interface TLFWThing (FromApp)
- (NSString *)shouted;
@end
@implementation TLFWThing (FromApp)
- (NSString *)shouted { return [self.what uppercaseString]; }
@end

int main(void) {
    @autoreleasepool {
        TLFWThing *t = [TLFWThing new];
        printf("%s | %s | counter %d | add %d %d | loads %d\n", t.what.UTF8String, t.shouted.UTF8String, tlfw_counter,
               tlfw_add(1, 2), tlfw_add(1, 2), [TLFWThing loads]);
        printf("PASS fwapp\n");
    }
    return 0;
}
