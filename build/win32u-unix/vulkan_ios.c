/* iOS override for dlls/win32u/vulkan.c.
 *
 * Upstream loads the Vulkan loader with dlopen(SONAME_LIBVULKAN) and then
 * dlsym's exactly two entry points; everything else is reached through
 * vkGetInstanceProcAddr. On iOS there is no loader and no dylib — MoltenVK
 * is linked statically (toolchains/MoltenVK/.../libMoltenVK.a, merged into
 * libwin32u_unix.a) — so the dlopen/dlsym pair is rewritten to hand back the
 * static symbols, exactly as freetype_ios.c does for freetype.
 *
 * config_ios.h force-#undefs SONAME_LIBVULKAN for every other TU; re-enable
 * it here only.
 *
 * The two targets are referenced through __asm__ aliases rather than
 * prototypes (the idiom gen_gnutls_symtab.sh uses) so this file needs no
 * Vulkan headers of its own and cannot collide with the declarations
 * vulkan.c already pulls in.
 */

#define SONAME_LIBVULKAN "libMoltenVK.a"

/* Rewrites apply to <dlfcn.h>'s prototypes too, so the shims are non-static
 * and match dlfcn.h's signatures exactly. */
#define dlopen  ios_vk_dlopen
#define dlsym   ios_vk_dlsym
#define dlclose ios_vk_dlclose

#include "vulkan.c"

/* MoltenVK's two loader entry points, by assembler name. */
extern char ios_vk_get_instance_proc_addr __asm__("_vkGetInstanceProcAddr");
extern char ios_vk_get_device_proc_addr   __asm__("_vkGetDeviceProcAddr");

static char ios_vk_sentinel_obj;
#define IOS_VK_SENTINEL ((void *)&ios_vk_sentinel_obj)

void *ios_vk_dlopen( const char *path, int mode )
{
    /* Match whatever SONAME the build was configured with, plus the generic
     * loader names, so this keeps working if SONAME_LIBVULKAN changes. */
    if (path && (strstr( path, "MoltenVK" ) || strstr( path, "vulkan" ))) return IOS_VK_SENTINEL;
    return NULL;
}

int ios_vk_dlclose( void *handle )
{
    return 0;
}

void *ios_vk_dlsym( void *handle, const char *symbol )
{
    if (handle != IOS_VK_SENTINEL || !symbol) return NULL;
    if (!strcmp( symbol, "vkGetInstanceProcAddr" )) return (void *)&ios_vk_get_instance_proc_addr;
    if (!strcmp( symbol, "vkGetDeviceProcAddr" ))   return (void *)&ios_vk_get_device_proc_addr;
    return NULL;
}
