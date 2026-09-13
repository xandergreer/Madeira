/* winios.drv Vulkan support — the iOS counterpart to winemac.drv/vulkan.c.
 *
 * Wine asks the display driver for a VkSurfaceKHR bound to a window. On macOS
 * winemac.drv creates a Cocoa view, wraps it in a metal view and hands the
 * resulting CAMetalLayer to vkCreateMetalSurfaceEXT. iOS has no Cocoa views,
 * but Madeira's compositor already keeps a CAMetalLayer per HWND for DXMT to
 * present into (Winios.m: winios_metal_layer_for_hwnd), so we can skip the
 * device/view dance entirely and hand that same layer to MoltenVK.
 *
 * That layer is owned by the compositor, so the client_surface hooks here are
 * deliberately inert: destroying a Vulkan surface must not tear down a layer
 * DXMT may still be using, and presentation happens through the swapchain
 * rather than through win32u.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include "ntstatus.h"
#include "ntgdi_private.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

/* Implemented in app/Madeira/Winios/Winios.m, linked into the same binary.
 * Weak so this file still links if the compositor side is absent — the
 * surface path then declines cleanly instead of failing to load. */
/* Resolves per-window (desktop mode) or the fullscreen singleton (game mode),
 * mirroring what DXMT gets -- see IOSDisplayShim.m. Using
 * winios_metal_layer_for_hwnd directly only works in desktop mode and returns
 * NULL for games. */
extern void *madeira_metal_layer_for_hwnd( void *hwnd ) __attribute__((weak));

struct winios_client_surface
{
    struct client_surface client;
    void *layer;   /* CAMetalLayer, owned by the compositor — not ours to free */
};

static void winios_surface_destroy( struct client_surface *client )
{
    TRACE( "%s\n", debugstr_client_surface( client ) );
}

static void winios_surface_detach( struct client_surface *client )
{
    TRACE( "%s\n", debugstr_client_surface( client ) );
}

static void winios_surface_update( struct client_surface *client )
{
    TRACE( "%s\n", debugstr_client_surface( client ) );
}

static void winios_surface_present( struct client_surface *client, HDC hdc )
{
    /* MoltenVK presents through the swapchain straight to the CAMetalLayer. */
}

static const struct client_surface_funcs winios_client_surface_funcs =
{
    .destroy = winios_surface_destroy,
    .detach  = winios_surface_detach,
    .update  = winios_surface_update,
    .present = winios_surface_present,
};

static VkResult winios_vulkan_surface_create( HWND hwnd, const struct vulkan_instance *instance,
                                              VkSurfaceKHR *handle, struct client_surface **client )
{
    VkMetalSurfaceCreateInfoEXT create_info_host;
    struct winios_client_surface *surface;
    void *layer;
    VkResult res;

    TRACE( "%p %p %p %p\n", hwnd, instance, handle, client );

    if (!madeira_metal_layer_for_hwnd)
    {
        ERR( "winios compositor not present, no Metal layer to bind\n" );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    if (!(layer = madeira_metal_layer_for_hwnd( hwnd )))
    {
        ERR( "no CAMetalLayer for hwnd %p\n", hwnd );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    if (!instance->p_vkCreateMetalSurfaceEXT)
    {
        /* MoltenVK always exports it; VK_MVK_macos_surface is macOS-only so
         * there is no legacy fallback to attempt here. */
        ERR( "VK_EXT_metal_surface unavailable\n" );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }

    if (!(surface = client_surface_create( sizeof(*surface), &winios_client_surface_funcs, hwnd )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    surface->layer = layer;

    create_info_host.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.pLayer = layer;

    res = instance->p_vkCreateMetalSurfaceEXT( instance->host.instance, &create_info_host, NULL, handle );
    if (res != VK_SUCCESS)
    {
        ERR( "Failed to create MoltenVK surface, res=%d\n", res );
        client_surface_release( &surface->client );
        return res;
    }

    *client = &surface->client;
    TRACE( "created surface=0x%s client=%p layer=%p\n", wine_dbgstr_longlong( *handle ), *client, layer );
    return VK_SUCCESS;
}

static VkBool32 winios_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device,
                                                                 uint32_t index )
{
    /* Single Metal device, every queue family can present to a CAMetalLayer. */
    return VK_TRUE;
}

static void winios_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    /* Guests ask for VK_KHR_win32_surface; MoltenVK provides VK_EXT_metal_surface.
     * Advertise each as implying the other, exactly as winemac.drv does. */
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_metal_surface = 1;
    if (extensions->has_VK_EXT_metal_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void winios_map_device_extensions( struct vulkan_device_extensions *extensions )
{
}

static const struct vulkan_driver_funcs winios_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = winios_vulkan_surface_create,
    .p_get_physical_device_presentation_support = winios_get_physical_device_presentation_support,
    .p_map_instance_extensions = winios_map_instance_extensions,
    .p_map_device_extensions = winios_map_device_extensions,
};

UINT winios_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but winios has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }

    ERR( "winios vulkan driver installed (version %u)\n", version );
    *driver_funcs = &winios_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
