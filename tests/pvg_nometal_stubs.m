/* The stand-ins test_pvg_present_nometal links instead of Metal and QuartzCore (tests/pvg_nometal.h renames the uses).
   - The command-buffer error domain: Metal's string. The media clock: CACurrentMediaTime's own definition (the host's
     mach clock in seconds).
   - The texture descriptor: a value with Metal's defaults, its resource options packing the storage, CPU cache and
     hazard tracking modes as Metal's do (MTLTexture.h), as the surface, video and mapping tests only hand it to the
     tests' fake device. A copy is a copy of its values, as Metal's is. test_texture_descriptor checks all of this
     against Metal's own in test_pvg_present_nogpu, and against this one here.
   - CATransaction: nothing to do. The tests' layer is a plain object (the presenter never draws), so a transaction has
     no layer change to group.
   - Every other class (the pipeline, sampler, render-pass, depth-stencil and heap descriptors, the linked functions, the
     Metal layer) stops the run with its name if anything makes one. The tests of what pvg_metal.m does with those
     (test_sampler_made, test_pipeline_timed, test_stage_kinds, test_pipeline_checks, test_building_note)
     check it against Metal's own objects, so they run only in test_pvg_present_nogpu (Metal linked, no GPU device). */
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

NSErrorDomain const MTLCommandBufferErrorDomain = @"MTLCommandBufferErrorDomain";

double CACurrentMediaTime(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

#pragma clang diagnostic ignored "-Wincomplete-implementation"
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wnullability"
#pragma clang diagnostic ignored "-Wobjc-property-implementation"

@implementation MTLTextureDescriptor {
    MTLResourceOptions options; /* as Metal's: storageMode, cpuCacheMode and hazardTrackingMode are packed in it */
}
- (MTLResourceOptions)resourceOptions { return options; }
- (void)setResourceOptions:(MTLResourceOptions)o { options = o; }
- (MTLCPUCacheMode)cpuCacheMode { return (options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift; }
- (void)setCpuCacheMode:(MTLCPUCacheMode)m {
    options = (options & ~MTLResourceCPUCacheModeMask) | ((MTLResourceOptions)m << MTLResourceCPUCacheModeShift & MTLResourceCPUCacheModeMask);
}
- (MTLStorageMode)storageMode { return (options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift; }
- (void)setStorageMode:(MTLStorageMode)m {
    options = (options & ~MTLResourceStorageModeMask) | ((MTLResourceOptions)m << MTLResourceStorageModeShift & MTLResourceStorageModeMask);
}
- (MTLHazardTrackingMode)hazardTrackingMode {
    return (options & MTLResourceHazardTrackingModeMask) >> MTLResourceHazardTrackingModeShift;
}
- (void)setHazardTrackingMode:(MTLHazardTrackingMode)m {
    options = (options & ~MTLResourceHazardTrackingModeMask) |
              ((MTLResourceOptions)m << MTLResourceHazardTrackingModeShift & MTLResourceHazardTrackingModeMask);
}
- (instancetype)init {
    if (!(self = [super init])) return nil;
    self.textureType = MTLTextureType2D, self.pixelFormat = MTLPixelFormatRGBA8Unorm;
    self.width = self.height = self.depth = 1;
    self.mipmapLevelCount = self.sampleCount = self.arrayLength = 1;
    self.storageMode = MTLStorageModeManaged /* Metal's default on macOS, as test_texture_descriptor shows */, self.usage = MTLTextureUsageShaderRead, self.allowGPUOptimizedContents = YES;
    self.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                                 MTLTextureSwizzleAlpha);
    return self;
}
+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format width:(NSUInteger)w
                                                      height:(NSUInteger)h mipmapped:(BOOL)mipmapped {
    MTLTextureDescriptor *d = [self new];
    d.pixelFormat = format, d.width = w, d.height = h;
    NSUInteger levels = 1;
    for (NSUInteger s = w > h ? w : h; mipmapped && s > 1; s >>= 1) levels++;
    d.mipmapLevelCount = levels;
    return d;
}
- (id)copyWithZone:(NSZone *)zone {
    (void)zone;
    MTLTextureDescriptor *c = [[self class] new];
    unsigned n = 0;
    objc_property_t *props = class_copyPropertyList([MTLTextureDescriptor class], &n);
    for (unsigned i = 0; i < n; i++) {
        char *ro = property_copyAttributeValue(props[i], "R");
        if (!ro) {
            NSString *key = @(property_getName(props[i]));
            [c setValue:[self valueForKey:key] forKey:key];
        }
        free(ro);
    }
    free(props);
    return c;
}
@end

@interface CATransaction : NSObject
@end
@implementation CATransaction
+ (void)begin {
}
+ (void)commit {
}
+ (void)setDisableActions:(BOOL)disable {
    (void)disable;
}
@end

static void needs_metal(const char *name) {
    fprintf(stderr, "%s is Metal's own object: a test that makes one runs in test_pvg_present_nogpu, not here\n", name);
    abort();
}
#define NEEDS_METAL(C)                                                                                                  \
    @implementation C                                                                                                   \
    +(instancetype)allocWithZone : (NSZone *)zone { (void)zone, needs_metal(#C); return nil; }                         \
    @end
NEEDS_METAL(MTLComputePipelineDescriptor)
NEEDS_METAL(MTLDepthStencilDescriptor)
NEEDS_METAL(MTLHeapDescriptor)
NEEDS_METAL(MTLLinkedFunctions)
NEEDS_METAL(MTLRenderPipelineDescriptor)
NEEDS_METAL(MTLSamplerDescriptor)
NEEDS_METAL(MTLStencilDescriptor)
NEEDS_METAL(MTLTileRenderPipelineDescriptor)
@implementation MTLRenderPassDescriptor
+ (instancetype)allocWithZone:(NSZone *)zone { (void)zone, needs_metal("MTLRenderPassDescriptor"); return nil; }
+ (MTLRenderPassDescriptor *)renderPassDescriptor { needs_metal("MTLRenderPassDescriptor"); return nil; }
@end
@interface CAMetalLayer : NSObject
@end
@implementation CAMetalLayer
+ (instancetype)allocWithZone:(NSZone *)zone { (void)zone, needs_metal("CAMetalLayer"); return nil; }
+ (id)layer { needs_metal("CAMetalLayer"); return nil; }
@end
