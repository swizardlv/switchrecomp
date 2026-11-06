/* Public-release build without NVIDIA DLSS (build_snxvk.py with SWR_NO_DLSS=1): the DLSS SDK licence does not allow
   combining it with this GPL-3.0 renderer in a distributed program, so the release links these no-ops instead of dlss.c.
   Same entry points as dlss.c; `dlss=` in the ini has no effect. */
#include <stddef.h>
typedef unsigned long long u64; typedef unsigned u32;
int vk_dlss_wanted(void) { return 0; }
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max) { (void)ext; (void)next; (void)max; }
void vk_dlss_device_exts(void* pd, const char** ext, u32* next, u32 max) { (void)pd; (void)ext; (void)next; (void)max; }
int vk_dlss_init(void) { return 0; }
void vk_dlss_draw(void* rt0, void* zt, int depth_test) { (void)rt0; (void)zt; (void)depth_test; }
void vk_dlss_pass(void** rt, u32 n) { (void)rt; (void)n; }
void vk_dlss_frame(void) {}
int vk_dlss_xfb_wanted(void) { return 0; }
const void* vk_dlss_device_chain(const void* next) { return next; }
void vk_dlss_draw_begin(void) {}
void vk_dlss_rt_read(void) {}
void vk_dlss_jitter(void* zt, int depth_write, int depth_equal, void* vp) { (void)zt; (void)depth_write; (void)depth_equal; (void)vp; }
int vk_dlss_capture_begin(void* cb, void* zt, int depth_write, int plain_tris, u64 key, u32 verts, const void* vp, const void* vpj) {
    (void)cb; (void)zt; (void)depth_write; (void)plain_tris; (void)key; (void)verts; (void)vp; (void)vpj; return 0; }
void vk_dlss_capture_end(void* cb) { (void)cb; }
void vk_dlss_before_read(u64 gva) { (void)gva; }
