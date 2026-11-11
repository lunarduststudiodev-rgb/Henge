#import <Foundation/Foundation.h>

int tlfw_counter = 100;
__thread int tlfw_tls = 3;
static int fw_loads;

@interface TLFWThing : NSObject
+ (int)loads;
- (NSString *)what;
@end
@implementation TLFWThing
+ (void)load { fw_loads++; }
+ (int)loads { return fw_loads; }
- (NSString *)what { return @"a class from the app's framework"; }
@end

__attribute__((constructor)) static void fw_init(void) { tlfw_counter += 1; }
int tlfw_add(int a, int b) { return a + b + (tlfw_tls++); }
