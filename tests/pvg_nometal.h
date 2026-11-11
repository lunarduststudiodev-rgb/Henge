/* test_pvg_present_nometal: the presenter's tests with no Metal (nor QuartzCore) linked, so they can be run where
   nothing may link Metal. Force-included (-include) before anything else, so every use of the 14 names Metal and
   QuartzCore would have to supply, in pvg_metal.m, the tests and the headers' own declarations, names the stand-in
   in tests/pvg_nometal_stubs.m instead. Renamed rather than defined under Metal's names: macOS loads Metal into every
   process that links Foundation (or CoreFoundation), and two classes of one name in a process is undefined.
   What each stand-in is, and which tests can't use them, is in tests/pvg_nometal_stubs.m. */
#define MTLCommandBufferErrorDomain HvNoMetalCommandBufferErrorDomain
#define CACurrentMediaTime HvNoMetalMediaTime
#define MTLTextureDescriptor HvNoMetalTextureDescriptor
#define CATransaction HvNoMetalTransaction
#define CAMetalLayer HvNoMetalLayer
#define MTLComputePipelineDescriptor HvNoMetalComputePipelineDescriptor
#define MTLDepthStencilDescriptor HvNoMetalDepthStencilDescriptor
#define MTLHeapDescriptor HvNoMetalHeapDescriptor
#define MTLLinkedFunctions HvNoMetalLinkedFunctions
#define MTLRenderPassDescriptor HvNoMetalRenderPassDescriptor
#define MTLRenderPipelineDescriptor HvNoMetalRenderPipelineDescriptor
#define MTLSamplerDescriptor HvNoMetalSamplerDescriptor
#define MTLStencilDescriptor HvNoMetalStencilDescriptor
#define MTLTileRenderPipelineDescriptor HvNoMetalTileRenderPipelineDescriptor
